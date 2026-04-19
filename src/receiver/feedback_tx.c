#include "receiver/receiver_ctx.h"
#include "core/fc_header.h"

#include <inttypes.h>
#include <stdlib.h>

#include <rte_byteorder.h>
#include <rte_errno.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

static receiver_vc_infiniflow_state_t *receiver_get_infiniflow_vc_state(receiver_ctx_t *ctx,
                                                                        uint32_t vc_id) {
    if (ctx->cfg.fc_mode != FC_MODE_INFINIFLOW || ctx->vc_infiniflow_states == NULL ||
        vc_id >= ctx->cfg.nb_vc) {
        return NULL;
    }

    return &ctx->vc_infiniflow_states[vc_id];
}

static void receiver_feedback_append_pending_locked(receiver_vc_infiniflow_state_t *vc_state,
                                                    receiver_feedback_msg_t *msg) {
    msg->pending_next = NULL;
    if (vc_state->pending_feedback_tail == NULL) {
        vc_state->pending_feedback_head = msg;
        vc_state->pending_feedback_tail = msg;
        return;
    }

    vc_state->pending_feedback_tail->pending_next = msg;
    vc_state->pending_feedback_tail = msg;
}

static void receiver_feedback_detach_pending_locked(receiver_vc_infiniflow_state_t *vc_state,
                                                    receiver_feedback_msg_t *msg) {
    receiver_feedback_msg_t *prev = NULL;
    receiver_feedback_msg_t *cur = vc_state->pending_feedback_head;

    while (cur != NULL) {
        if (cur == msg) {
            if (prev == NULL) {
                vc_state->pending_feedback_head = cur->pending_next;
            } else {
                prev->pending_next = cur->pending_next;
            }
            if (vc_state->pending_feedback_tail == cur) {
                vc_state->pending_feedback_tail = prev;
            }
            cur->pending_next = NULL;
            return;
        }
        prev = cur;
        cur = cur->pending_next;
    }
}

static void receiver_feedback_bind_pending_flags_locked(receiver_vc_infiniflow_state_t *vc_state,
                                                        receiver_feedback_msg_t *msg) {
    uint32_t pending_flags = vc_state->pending_feedback_flags;

    if (pending_flags == 0U) {
        return;
    }

    if (vc_state->pending_feedback_head != NULL) {
        vc_state->pending_feedback_head->flags |= pending_flags;
    } else {
        msg->flags |= pending_flags;
    }

    vc_state->pending_feedback_flags = 0U;
    if ((pending_flags & INFINIFLOW_FEEDBACK_FLAG_TA) != 0U) {
        vc_state->state = 0U;
    }
}

static void receiver_feedback_restore_pending_locked(receiver_vc_infiniflow_state_t *vc_state,
                                                     receiver_feedback_msg_t *msg, bool requeued) {
    if (requeued) {
        receiver_feedback_append_pending_locked(vc_state, msg);
        return;
    }

    if ((msg->flags & INFINIFLOW_FEEDBACK_FLAG_TA) != 0U) {
        vc_state->pending_feedback_flags |= INFINIFLOW_FEEDBACK_FLAG_TA;
        vc_state->state = 1U;
        msg->flags &= ~INFINIFLOW_FEEDBACK_FLAG_TA;
    }
}

void receiver_feedback_note_ta(receiver_ctx_t *ctx, uint32_t vc_id) {
    receiver_vc_infiniflow_state_t *vc_state = receiver_get_infiniflow_vc_state(ctx, vc_id);

    if (vc_state == NULL) {
        return;
    }

    rte_spinlock_lock(&vc_state->pending_feedback_lock);
    vc_state->state = 1U;
    if (vc_state->pending_feedback_head != NULL) {
        vc_state->pending_feedback_head->flags |= INFINIFLOW_FEEDBACK_FLAG_TA;
        vc_state->state = 0U;
    } else {
        vc_state->pending_feedback_flags |= INFINIFLOW_FEEDBACK_FLAG_TA;
    }
    rte_spinlock_unlock(&vc_state->pending_feedback_lock);
}

int receiver_feedback_init(receiver_ctx_t *ctx) {
    uint32_t j = 0;
    unsigned int feedback_ring_flags = RING_F_SC_DEQ | RING_F_EXACT_SZ;
    /* Match switch: SC dequeue on free ring; enqueue uses rte_ring_mp_enqueue. */
    unsigned int free_ring_flags = RING_F_SC_DEQ | RING_F_EXACT_SZ;

    if (ctx->cfg.feedback_ring_size == 0U) {
        RTE_LOG(ERR, USER1, "receiver port=%" PRIu16 " invalid feedback ring size=0\n",
                ctx->cfg.port_id);
        return -1;
    }

    ctx->feedback_ring = rte_ring_create(ctx->feedback_ring_name, ctx->cfg.feedback_ring_size,
                                         rte_socket_id(), feedback_ring_flags);
    ctx->feedback_free_ring =
        rte_ring_create(ctx->feedback_free_ring_name, ctx->cfg.feedback_ring_size, rte_socket_id(),
                        free_ring_flags);
    if (ctx->feedback_ring == NULL || ctx->feedback_free_ring == NULL) {
        RTE_LOG(ERR, USER1,
                "receiver port=%" PRIu16 " feedback ring create failed: ring=%s free_ring=%s"
                " size=%" PRIu32 " err=%d(%s)\n",
                ctx->cfg.port_id, ctx->feedback_ring_name, ctx->feedback_free_ring_name,
                ctx->cfg.feedback_ring_size, rte_errno, rte_strerror(rte_errno));
        return -1;
    }

    ctx->feedback_pool = calloc((size_t)ctx->cfg.feedback_ring_size, sizeof(*ctx->feedback_pool));
    if (ctx->feedback_pool == NULL) {
        RTE_LOG(ERR, USER1,
                "receiver port=%" PRIu16 " feedback pool alloc failed: entries=%" PRIu32 "\n",
                ctx->cfg.port_id, ctx->cfg.feedback_ring_size);
        return -1;
    }

    for (j = 0; j < ctx->cfg.feedback_ring_size; j++) {
        receiver_feedback_msg_t *msg = &ctx->feedback_pool[j];
        if (rte_ring_mp_enqueue(ctx->feedback_free_ring, msg) != 0) {
            RTE_LOG(ERR, USER1,
                    "receiver port=%" PRIu16 " feedback free ring prefill failed: idx=%" PRIu32
                    " err=%d(%s)\n",
                    ctx->cfg.port_id, j, rte_errno, rte_strerror(rte_errno));
            return -1;
        }
    }

    return 0;
}

void receiver_feedback_cleanup(receiver_ctx_t *ctx) {
    void *ptr = NULL;

    if (ctx->feedback_ring != NULL) {
        while (rte_ring_sc_dequeue(ctx->feedback_ring, &ptr) == 0) {
        }
        rte_ring_free(ctx->feedback_ring);
        ctx->feedback_ring = NULL;
    }
    if (ctx->feedback_free_ring != NULL) {
        while (rte_ring_sc_dequeue(ctx->feedback_free_ring, &ptr) == 0) {
        }
        rte_ring_free(ctx->feedback_free_ring);
        ctx->feedback_free_ring = NULL;
    }

    free(ctx->feedback_pool);
    ctx->feedback_pool = NULL;
}

void receiver_feedback_try_enqueue(receiver_ctx_t *ctx, const struct rte_ether_addr *dst_addr,
                                   uint32_t vc_id, uint64_t fccl, uint64_t vc_dr,
                                   uint64_t vc_bklg, uint32_t flags) {
    receiver_feedback_msg_t *msg = NULL;
    receiver_vc_infiniflow_state_t *vc_state = NULL;

    if (ctx->feedback_ring == NULL || ctx->feedback_free_ring == NULL) {
        return;
    }

    if (rte_ring_sc_dequeue(ctx->feedback_free_ring, (void **)&msg) != 0) {
        ctx->feedback_enqueue_drop++;
        return;
    }

    msg->vc_id = vc_id;
    msg->fccl = fccl;
    msg->vc_dr = vc_dr;
    msg->vc_bklg = vc_bklg;
    msg->flags = flags;
    msg->pending_next = NULL;
    rte_ether_addr_copy(dst_addr, &msg->dst_addr);

    vc_state = receiver_get_infiniflow_vc_state(ctx, vc_id);
    if (vc_state != NULL) {
        rte_spinlock_lock(&vc_state->pending_feedback_lock);
        receiver_feedback_bind_pending_flags_locked(vc_state, msg);
        receiver_feedback_append_pending_locked(vc_state, msg);
        rte_spinlock_unlock(&vc_state->pending_feedback_lock);
    }

    if (rte_ring_mp_enqueue(ctx->feedback_ring, msg) != 0) {
        if (vc_state != NULL) {
            rte_spinlock_lock(&vc_state->pending_feedback_lock);
            receiver_feedback_detach_pending_locked(vc_state, msg);
            if ((msg->flags & INFINIFLOW_FEEDBACK_FLAG_TA) != 0U) {
                vc_state->pending_feedback_flags |= INFINIFLOW_FEEDBACK_FLAG_TA;
                vc_state->state = 1U;
                msg->flags &= ~INFINIFLOW_FEEDBACK_FLAG_TA;
            }
            rte_spinlock_unlock(&vc_state->pending_feedback_lock);
        }
        if (rte_ring_mp_enqueue(ctx->feedback_free_ring, msg) != 0) {
            RTE_LOG(ERR, USER1, "receiver: failed to return feedback msg to free ring\n");
        }
        ctx->feedback_enqueue_drop++;
    }
}

uint32_t receiver_feedback_tx_run_tick(receiver_ctx_t *ctx) {
    struct rte_mbuf *tx_pkts[256];
    receiver_feedback_msg_t *tx_msgs[256];
    receiver_vc_infiniflow_state_t *vc_state = NULL;
    uint32_t burst = (ctx->cfg.tx_burst_size > 256U) ? 256U : ctx->cfg.tx_burst_size;
    uint32_t prepared = 0;
    uint16_t tx_count = 0;
    uint32_t i = 0;

    if (burst == 0U || ctx->feedback_ring == NULL) {
        return 0;
    }

    while (prepared < burst) {
        receiver_feedback_msg_t *msg = NULL;
        struct rte_mbuf *mbuf = NULL;
        char *packet = NULL;
        struct rte_ether_hdr *eth_hdr = NULL;
        cbfc_feedback_header_t *cbfc_hdr = NULL;
        infiniflow_feedback_header_t *infi_hdr = NULL;
        uint32_t packet_len = sizeof(*eth_hdr) + CBFC_FEEDBACK_HEADER_SIZE;

        if (rte_ring_sc_dequeue(ctx->feedback_ring, (void **)&msg) != 0) {
            break;
        }

        vc_state = receiver_get_infiniflow_vc_state(ctx, msg->vc_id);
        if (vc_state != NULL) {
            rte_spinlock_lock(&vc_state->pending_feedback_lock);
            receiver_feedback_detach_pending_locked(vc_state, msg);
            rte_spinlock_unlock(&vc_state->pending_feedback_lock);
        }

        mbuf = rte_pktmbuf_alloc(ctx->mbuf_pool);
        if (mbuf == NULL) {
            int requeue_ok = rte_ring_mp_enqueue(ctx->feedback_ring, msg) == 0 ? 1 : 0;

            if (vc_state != NULL) {
                rte_spinlock_lock(&vc_state->pending_feedback_lock);
                receiver_feedback_restore_pending_locked(vc_state, msg, requeue_ok != 0);
                rte_spinlock_unlock(&vc_state->pending_feedback_lock);
            }
            if (!requeue_ok) {
                if (rte_ring_mp_enqueue(ctx->feedback_free_ring, msg) != 0) {
                    RTE_LOG(ERR, USER1, "receiver: failed to recycle feedback msg after mbuf OOM\n");
                }
                ctx->feedback_enqueue_drop++;
            }
            break;
        }

        if (ctx->cfg.fc_mode == FC_MODE_INFINIFLOW) {
            packet_len = sizeof(*eth_hdr) + INFINIFLOW_FEEDBACK_HEADER_SIZE;
        }

        packet = rte_pktmbuf_append(mbuf, packet_len);
        if (packet == NULL) {
            int requeue_ok = 0;

            rte_pktmbuf_free(mbuf);
            requeue_ok = rte_ring_mp_enqueue(ctx->feedback_ring, msg) == 0 ? 1 : 0;
            if (vc_state != NULL) {
                rte_spinlock_lock(&vc_state->pending_feedback_lock);
                receiver_feedback_restore_pending_locked(vc_state, msg, requeue_ok != 0);
                rte_spinlock_unlock(&vc_state->pending_feedback_lock);
            }
            if (!requeue_ok) {
                if (rte_ring_mp_enqueue(ctx->feedback_free_ring, msg) != 0) {
                    RTE_LOG(ERR, USER1, "receiver: failed to recycle feedback msg after append fail\n");
                }
                ctx->feedback_enqueue_drop++;
            }
            continue;
        }

        eth_hdr = (struct rte_ether_hdr *)packet;
        rte_ether_addr_copy(&msg->dst_addr, &eth_hdr->dst_addr);
        rte_ether_addr_copy(&ctx->port_mac, &eth_hdr->src_addr);
        if (ctx->cfg.fc_mode == FC_MODE_INFINIFLOW) {
            infi_hdr = (infiniflow_feedback_header_t *)(packet + sizeof(*eth_hdr));
            eth_hdr->ether_type = rte_cpu_to_be_16(INFINIFLOW_FEEDBACK_ETHER_TYPE);
            infi_hdr->vc_id = rte_cpu_to_be_32(msg->vc_id);
            infi_hdr->flags = fc_cpu_to_be32(msg->flags);
            infi_hdr->vc_dr = fc_cpu_to_be64(msg->vc_dr);
            infi_hdr->vc_bklg = fc_cpu_to_be64(msg->vc_bklg);
            infi_hdr->fccl = fc_cpu_to_be64(msg->fccl);
        } else {
            cbfc_hdr = (cbfc_feedback_header_t *)(packet + sizeof(*eth_hdr));
            eth_hdr->ether_type = rte_cpu_to_be_16(CBFC_FEEDBACK_ETHER_TYPE);
            cbfc_hdr->vc_id = rte_cpu_to_be_32(msg->vc_id);
            cbfc_hdr->fccl = fc_cpu_to_be64(msg->fccl);
        }
        tx_pkts[prepared] = mbuf;
        tx_msgs[prepared] = msg;
        prepared++;
    }

    if (prepared == 0U) {
        return 0;
    }

    tx_count = rte_eth_tx_burst(ctx->cfg.port_id, ctx->cfg.tx_queue_id, tx_pkts,
                                (uint16_t)prepared);
    ctx->tx_cbfc_feedback_pkts += (uint64_t)tx_count;

    for (i = 0; i < tx_count; i++) {
        if (rte_ring_mp_enqueue(ctx->feedback_free_ring, tx_msgs[i]) != 0) {
            RTE_LOG(ERR, USER1, "receiver: failed to return sent feedback msg to free ring\n");
        }
    }

    for (i = (uint32_t)tx_count; i < prepared; i++) {
        rte_pktmbuf_free(tx_pkts[i]);
        vc_state = receiver_get_infiniflow_vc_state(ctx, tx_msgs[i]->vc_id);
        if (vc_state != NULL) {
            rte_spinlock_lock(&vc_state->pending_feedback_lock);
            receiver_feedback_restore_pending_locked(vc_state, tx_msgs[i], true);
            rte_spinlock_unlock(&vc_state->pending_feedback_lock);
        }
        if (rte_ring_mp_enqueue(ctx->feedback_ring, tx_msgs[i]) != 0) {
            if (vc_state != NULL) {
                rte_spinlock_lock(&vc_state->pending_feedback_lock);
                receiver_feedback_detach_pending_locked(vc_state, tx_msgs[i]);
                receiver_feedback_restore_pending_locked(vc_state, tx_msgs[i], false);
                rte_spinlock_unlock(&vc_state->pending_feedback_lock);
            }
            if (rte_ring_mp_enqueue(ctx->feedback_free_ring, tx_msgs[i]) != 0) {
                RTE_LOG(ERR, USER1, "receiver: failed to recycle unsent feedback msg\n");
            }
            ctx->feedback_enqueue_drop++;
        }
    }

    return (uint32_t)tx_count;
}
