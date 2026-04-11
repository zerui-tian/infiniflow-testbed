#include "switch/switch_ctx.h"
#include "core/fc_header.h"

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

static void enqueue_feedback_msg(switch_ctx_t *ctx, uint16_t ingress_idx, uint32_t vc_id,
                                 uint64_t fccl, const struct rte_ether_addr *dst_addr) {
    switch_feedback_msg_t *msg = NULL;

    if (ingress_idx >= ctx->cfg.nb_ingress_ports) {
        return;
    }
    if (rte_ring_sc_dequeue(ctx->feedback_free_queues[ingress_idx], (void **)&msg) != 0) {
        __atomic_fetch_add(&ctx->feedback_stats[ingress_idx].drop_no_free_pkts, 1U, __ATOMIC_RELAXED);
        return;
    }

    msg->vc_id = vc_id;
    msg->fccl = fccl;
    rte_ether_addr_copy(dst_addr, &msg->dst_addr);
    if (rte_ring_mp_enqueue(ctx->feedback_queues[ingress_idx], msg) != 0) {
        rte_ring_mp_enqueue(ctx->feedback_free_queues[ingress_idx], msg);
        __atomic_fetch_add(&ctx->feedback_stats[ingress_idx].drop_queue_full_pkts, 1U, __ATOMIC_RELAXED);
        return;
    }

    __atomic_fetch_add(&ctx->feedback_stats[ingress_idx].enqueue_ok_pkts, 1U, __ATOMIC_RELAXED);
}

static uint64_t release_ingress_credit(switch_ctx_t *ctx, uint16_t ingress_idx, uint32_t vc_id) {
    switch_ingress_vc_state_t *state = NULL;
    uint64_t occupancy = 0;

    if (ingress_idx >= ctx->cfg.nb_ingress_ports) {
        return 0U;
    }

    state = &ctx->ingress_vc_states[switch_ingress_vc_state_index(ctx, ingress_idx, vc_id)];
    occupancy = __atomic_load_n(&state->occupancy, __ATOMIC_ACQUIRE);
    while (occupancy > 0U) {
        if (__atomic_compare_exchange_n(&state->occupancy, &occupancy, occupancy - 1U, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            break;
        }
    }

    occupancy = __atomic_load_n(&state->occupancy, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&state->total_received, __ATOMIC_RELAXED) +
           ((state->capacity > occupancy) ? (state->capacity - occupancy) : 0U);
}

uint32_t switch_forward_run_tick(switch_ctx_t *ctx, uint16_t egress_idx) {
    uint16_t egress_port = ctx->cfg.egress_ports[egress_idx];
    uint32_t total_tx = 0;
    uint32_t vc_id = 0;

    for (vc_id = 0; vc_id < ctx->cfg.nb_vc; vc_id++) {
        struct rte_mbuf *burst[256];
        struct rte_mbuf *candidates[256];
        struct rte_ether_addr feedback_dst_addrs[256];
        uint16_t candidate_ingress_idxs[256];
        uint32_t want_deq = (ctx->cfg.tx_burst_size > 256U) ? 256U : ctx->cfg.tx_burst_size;
        uint32_t n_deq = 0;
        uint32_t n_tx_candidates = 0;
        uint32_t j = 0;
        size_t queue_idx = switch_egress_vc_state_index(ctx, egress_idx, vc_id);
        struct rte_ring *queue = ctx->egress_vc_queues[queue_idx].ring;

        if (want_deq == 0U) {
            continue;
        }

        if (ctx->cfg.fc_mode == FC_MODE_CBFC && ctx->fc_ops != NULL && ctx->fc_ops->calc_credit != NULL) {
            uint64_t credit = ctx->fc_ops->calc_credit(ctx, egress_idx, vc_id);
            if (credit == 0U) {
                continue;
            }
            if (credit < want_deq) {
                want_deq = (uint32_t)credit;
            }
        }

        n_deq = rte_ring_sc_dequeue_burst(queue, (void **)burst, want_deq, NULL);
        if (n_deq == 0U) {
            continue;
        }

        for (j = 0; j < n_deq; j++) {
            struct rte_mbuf *mbuf = burst[j];
            struct rte_ether_hdr *eth_hdr = NULL;
            uint16_t ingress_idx = switch_ingress_index_from_port(ctx, mbuf->port);

            if (mbuf->pkt_len < sizeof(struct rte_ether_hdr) + FC_DATA_HEADER_SIZE) {
                struct rte_ether_hdr *invalid_eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);

                if (ingress_idx < ctx->cfg.nb_ingress_ports) {
                    uint64_t feedback_fccl = release_ingress_credit(ctx, ingress_idx, vc_id);

                    __atomic_fetch_add(
                        &ctx->vc_stats[switch_ingress_vc_state_index(ctx, ingress_idx, vc_id)].drop_other_pkts,
                        1U, __ATOMIC_RELAXED);
                    if (ctx->cfg.fc_mode == FC_MODE_CBFC) {
                        enqueue_feedback_msg(ctx, ingress_idx, vc_id, feedback_fccl,
                                             &invalid_eth_hdr->src_addr);
                    }
                }
                rte_pktmbuf_free(mbuf);
                continue;
            }

            eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
            rte_ether_addr_copy(&eth_hdr->src_addr, &feedback_dst_addrs[n_tx_candidates]);
            rte_ether_addr_copy(&ctx->egress_macs[egress_idx], &eth_hdr->src_addr);
            candidate_ingress_idxs[n_tx_candidates] = ingress_idx;
            candidates[n_tx_candidates++] = mbuf;
        }

        if (n_tx_candidates == 0U) {
            continue;
        }

        {
            uint16_t n_tx =
                rte_eth_tx_burst(egress_port, ctx->cfg.egress_tx_queue_id, candidates, (uint16_t)n_tx_candidates);
            uint32_t k = 0;

            total_tx += n_tx;

            for (k = 0; k < n_tx; k++) {
                uint16_t ingress_idx = candidate_ingress_idxs[k];
                uint64_t feedback_fccl = 0;

                if (ingress_idx < ctx->cfg.nb_ingress_ports) {
                    if (ctx->cfg.fc_mode == FC_MODE_CBFC && ctx->fc_ops != NULL &&
                        ctx->fc_ops->on_tx_success != NULL) {
                        feedback_fccl = ctx->fc_ops->on_tx_success(ctx, ingress_idx, egress_idx, vc_id);
                        enqueue_feedback_msg(ctx, ingress_idx, vc_id, feedback_fccl, &feedback_dst_addrs[k]);
                    } else {
                        (void)release_ingress_credit(ctx, ingress_idx, vc_id);
                    }
                }

                if (ingress_idx < ctx->cfg.nb_ingress_ports) {
                    __atomic_fetch_add(
                        &ctx->vc_stats[switch_ingress_vc_state_index(ctx, ingress_idx, vc_id)].tx_ok_pkts, 1U,
                        __ATOMIC_RELAXED);
                }
            }

            for (k = n_tx; k < n_tx_candidates; k++) {
                if (rte_ring_mp_enqueue(queue, candidates[k]) == 0) {
                    continue;
                }

                {
                    uint16_t ingress_idx = candidate_ingress_idxs[k];

                    if (ingress_idx < ctx->cfg.nb_ingress_ports) {
                        uint64_t feedback_fccl = release_ingress_credit(ctx, ingress_idx, vc_id);

                        __atomic_fetch_add(
                            &ctx->vc_stats[switch_ingress_vc_state_index(ctx, ingress_idx, vc_id)]
                                 .drop_other_pkts,
                            1U, __ATOMIC_RELAXED);
                        if (ctx->cfg.fc_mode == FC_MODE_CBFC) {
                            enqueue_feedback_msg(ctx, ingress_idx, vc_id, feedback_fccl,
                                                 &feedback_dst_addrs[k]);
                        }
                    }
                }

                rte_pktmbuf_free(candidates[k]);
            }
        }
    }

    return total_tx;
}
