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
                                   uint32_t vc_id, uint64_t fccl) {
    receiver_feedback_msg_t *msg = NULL;

    if (ctx->feedback_ring == NULL || ctx->feedback_free_ring == NULL) {
        return;
    }

    if (rte_ring_sc_dequeue(ctx->feedback_free_ring, (void **)&msg) != 0) {
        ctx->feedback_enqueue_drop++;
        return;
    }

    msg->vc_id = vc_id;
    msg->fccl = fccl;
    rte_ether_addr_copy(dst_addr, &msg->dst_addr);

    if (rte_ring_mp_enqueue(ctx->feedback_ring, msg) != 0) {
        if (rte_ring_mp_enqueue(ctx->feedback_free_ring, msg) != 0) {
            RTE_LOG(ERR, USER1, "receiver: failed to return feedback msg to free ring\n");
        }
        ctx->feedback_enqueue_drop++;
    }
}

uint32_t receiver_feedback_tx_run_tick(receiver_ctx_t *ctx) {
    struct rte_mbuf *tx_pkts[256];
    receiver_feedback_msg_t *tx_msgs[256];
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
        cbfc_feedback_header_t *fb_hdr = NULL;

        if (rte_ring_sc_dequeue(ctx->feedback_ring, (void **)&msg) != 0) {
            break;
        }

        mbuf = rte_pktmbuf_alloc(ctx->mbuf_pool);
        if (mbuf == NULL) {
            if (rte_ring_mp_enqueue(ctx->feedback_ring, msg) != 0) {
                if (rte_ring_mp_enqueue(ctx->feedback_free_ring, msg) != 0) {
                    RTE_LOG(ERR, USER1, "receiver: failed to recycle feedback msg after mbuf OOM\n");
                }
                ctx->feedback_enqueue_drop++;
            }
            break;
        }

        packet = rte_pktmbuf_append(mbuf, sizeof(*eth_hdr) + CBFC_FEEDBACK_HEADER_SIZE);
        if (packet == NULL) {
            rte_pktmbuf_free(mbuf);
            if (rte_ring_mp_enqueue(ctx->feedback_ring, msg) != 0) {
                if (rte_ring_mp_enqueue(ctx->feedback_free_ring, msg) != 0) {
                    RTE_LOG(ERR, USER1, "receiver: failed to recycle feedback msg after append fail\n");
                }
                ctx->feedback_enqueue_drop++;
            }
            continue;
        }

        eth_hdr = (struct rte_ether_hdr *)packet;
        fb_hdr = (cbfc_feedback_header_t *)(packet + sizeof(*eth_hdr));
        rte_ether_addr_copy(&msg->dst_addr, &eth_hdr->dst_addr);
        rte_ether_addr_copy(&ctx->port_mac, &eth_hdr->src_addr);
        eth_hdr->ether_type = rte_cpu_to_be_16(CBFC_FEEDBACK_ETHER_TYPE);
        fb_hdr->vc_id = rte_cpu_to_be_32(msg->vc_id);
        fb_hdr->fccl = fc_cpu_to_be64(msg->fccl);
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
        if (rte_ring_mp_enqueue(ctx->feedback_ring, tx_msgs[i]) != 0) {
            if (rte_ring_mp_enqueue(ctx->feedback_free_ring, tx_msgs[i]) != 0) {
                RTE_LOG(ERR, USER1, "receiver: failed to recycle unsent feedback msg\n");
            }
            ctx->feedback_enqueue_drop++;
        }
    }

    return (uint32_t)tx_count;
}
