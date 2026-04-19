#include "switch/switch_ctx.h"
#include "core/fc_header.h"

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

static switch_ingress_vc_state_t *switch_feedback_gen_get_vc_state(switch_ctx_t *ctx,
                                                                   uint16_t ingress_idx,
                                                                   uint32_t vc_id) {
    if (ingress_idx >= ctx->cfg.nb_ingress_ports || vc_id >= ctx->cfg.nb_vc) {
        return NULL;
    }

    return &ctx->ingress_vc_states[switch_ingress_vc_state_index(ctx, ingress_idx, vc_id)];
}

static void switch_feedback_gen_append_pending_locked(switch_ingress_vc_state_t *vc_state,
                                                      switch_feedback_msg_t *msg) {
    msg->pending_next = NULL;
    if (vc_state->pending_feedback_tail == NULL) {
        vc_state->pending_feedback_head = msg;
        vc_state->pending_feedback_tail = msg;
        return;
    }

    vc_state->pending_feedback_tail->pending_next = msg;
    vc_state->pending_feedback_tail = msg;
}

static void switch_feedback_gen_detach_pending_locked(switch_ingress_vc_state_t *vc_state,
                                                      switch_feedback_msg_t *msg) {
    switch_feedback_msg_t *prev = NULL;
    switch_feedback_msg_t *cur = vc_state->pending_feedback_head;

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

static void switch_feedback_gen_restore_pending_locked(switch_ingress_vc_state_t *vc_state,
                                                       switch_feedback_msg_t *msg, bool requeued) {
    if (requeued) {
        switch_feedback_gen_append_pending_locked(vc_state, msg);
        return;
    }

    if ((msg->flags & INFINIFLOW_FEEDBACK_FLAG_TA) != 0U) {
        vc_state->pending_feedback_flags |= INFINIFLOW_FEEDBACK_FLAG_TA;
        vc_state->state = 1U;
        msg->flags &= ~INFINIFLOW_FEEDBACK_FLAG_TA;
    }
}

uint32_t switch_feedback_gen_run_tick(switch_ctx_t *ctx, uint16_t ingress_idx) {
    struct rte_mbuf *tx_pkts[256];
    switch_feedback_msg_t *tx_msgs[256];
    switch_ingress_vc_state_t *vc_state = NULL;
    uint32_t burst = (ctx->cfg.tx_burst_size > 256U) ? 256U : ctx->cfg.tx_burst_size;
    uint32_t prepared = 0;
    uint16_t tx_count = 0;
    uint32_t i = 0;
    uint16_t ingress_port = ctx->cfg.ingress_ports[ingress_idx];
    struct rte_ether_addr src_mac;

    if (burst == 0U || ingress_idx >= ctx->cfg.nb_ingress_ports) {
        return 0;
    }

    rte_eth_macaddr_get(ingress_port, &src_mac);

    while (prepared < burst) {
        switch_feedback_msg_t *msg = NULL;
        struct rte_mbuf *mbuf = NULL;
        char *packet = NULL;
        struct rte_ether_hdr *eth_hdr = NULL;
        cbfc_feedback_header_t *cbfc_hdr = NULL;
        infiniflow_feedback_header_t *infi_hdr = NULL;
        uint32_t packet_len = sizeof(*eth_hdr) + CBFC_FEEDBACK_HEADER_SIZE;

        if (rte_ring_sc_dequeue(ctx->feedback_queues[ingress_idx], (void **)&msg) != 0) {
            break;
        }

        vc_state = switch_feedback_gen_get_vc_state(ctx, ingress_idx, msg->vc_id);
        if (vc_state != NULL) {
            rte_spinlock_lock(&vc_state->pending_feedback_lock);
            switch_feedback_gen_detach_pending_locked(vc_state, msg);
            rte_spinlock_unlock(&vc_state->pending_feedback_lock);
        }

        mbuf = rte_pktmbuf_alloc(ctx->mbuf_pool);
        if (mbuf == NULL) {
            int requeue_ok = rte_ring_mp_enqueue(ctx->feedback_queues[ingress_idx], msg) == 0 ? 1 : 0;

            if (vc_state != NULL) {
                rte_spinlock_lock(&vc_state->pending_feedback_lock);
                switch_feedback_gen_restore_pending_locked(vc_state, msg, requeue_ok != 0);
                rte_spinlock_unlock(&vc_state->pending_feedback_lock);
            }
            if (!requeue_ok) {
                rte_ring_mp_enqueue(ctx->feedback_free_queues[ingress_idx], msg);
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
            requeue_ok = rte_ring_mp_enqueue(ctx->feedback_queues[ingress_idx], msg) == 0 ? 1 : 0;
            if (vc_state != NULL) {
                rte_spinlock_lock(&vc_state->pending_feedback_lock);
                switch_feedback_gen_restore_pending_locked(vc_state, msg, requeue_ok != 0);
                rte_spinlock_unlock(&vc_state->pending_feedback_lock);
            }
            if (!requeue_ok) {
                rte_ring_mp_enqueue(ctx->feedback_free_queues[ingress_idx], msg);
            }
            continue;
        }

        eth_hdr = (struct rte_ether_hdr *)packet;
        rte_ether_addr_copy(&msg->dst_addr, &eth_hdr->dst_addr);
        rte_ether_addr_copy(&src_mac, &eth_hdr->src_addr);
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

    tx_count = rte_eth_tx_burst(ingress_port, ctx->cfg.ingress_tx_queue_id, tx_pkts, (uint16_t)prepared);
    __atomic_fetch_add(&ctx->feedback_stats[ingress_idx].tx_ok_pkts, tx_count, __ATOMIC_RELAXED);

    for (i = 0; i < tx_count; i++) {
        if (rte_ring_mp_enqueue(ctx->feedback_free_queues[ingress_idx], tx_msgs[i]) != 0) {
            RTE_LOG(ERR, USER1, "switch: failed to return sent feedback msg to free ring\n");
        }
    }

    for (i = (uint32_t)tx_count; i < prepared; i++) {
        rte_pktmbuf_free(tx_pkts[i]);
        __atomic_fetch_add(&ctx->feedback_stats[ingress_idx].tx_retry_pkts, 1U, __ATOMIC_RELAXED);
        vc_state = switch_feedback_gen_get_vc_state(ctx, ingress_idx, tx_msgs[i]->vc_id);
        if (vc_state != NULL) {
            rte_spinlock_lock(&vc_state->pending_feedback_lock);
            switch_feedback_gen_restore_pending_locked(vc_state, tx_msgs[i], true);
            rte_spinlock_unlock(&vc_state->pending_feedback_lock);
        }
        if (rte_ring_mp_enqueue(ctx->feedback_queues[ingress_idx], tx_msgs[i]) != 0) {
            if (vc_state != NULL) {
                rte_spinlock_lock(&vc_state->pending_feedback_lock);
                switch_feedback_gen_detach_pending_locked(vc_state, tx_msgs[i]);
                switch_feedback_gen_restore_pending_locked(vc_state, tx_msgs[i], false);
                rte_spinlock_unlock(&vc_state->pending_feedback_lock);
            }
            if (rte_ring_mp_enqueue(ctx->feedback_free_queues[ingress_idx], tx_msgs[i]) != 0) {
                RTE_LOG(ERR, USER1, "switch: failed to recycle unsent feedback msg\n");
            }
        }
    }

    return tx_count;
}
