#include "switch/switch_ctx.h"
#include "core/fc_header.h"

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

static void enqueue_feedback_msg(switch_ctx_t *ctx, uint16_t ingress_idx, uint32_t vc_id,
                                 uint64_t fccl, uint64_t vc_dr, uint64_t vc_bklg, uint32_t flags,
                                 const struct rte_ether_addr *dst_addr) {
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
    msg->vc_dr = vc_dr;
    msg->vc_bklg = vc_bklg;
    msg->flags = flags;
    rte_ether_addr_copy(dst_addr, &msg->dst_addr);
    if (rte_ring_mp_enqueue(ctx->feedback_queues[ingress_idx], msg) != 0) {
        rte_ring_mp_enqueue(ctx->feedback_free_queues[ingress_idx], msg);
        __atomic_fetch_add(&ctx->feedback_stats[ingress_idx].drop_queue_full_pkts, 1U, __ATOMIC_RELAXED);
        return;
    }

    __atomic_fetch_add(&ctx->feedback_stats[ingress_idx].enqueue_ok_pkts, 1U, __ATOMIC_RELAXED);
}

static uint64_t release_ingress_credit(switch_ctx_t *ctx, uint16_t ingress_idx, uint32_t vc_id,
                                       uint64_t *vc_dr_out, uint64_t *vc_bklg_out,
                                       uint32_t *flags_out) {
    switch_ingress_vc_state_t *vc_state = NULL;
    switch_ingress_port_state_t *port_state = NULL;
    uint64_t occupancy = 0;
    uint64_t port_occupancy = 0;
    uint64_t fccl = 0;

    if (ingress_idx >= ctx->cfg.nb_ingress_ports) {
        return 0U;
    }

    vc_state = &ctx->ingress_vc_states[switch_ingress_vc_state_index(ctx, ingress_idx, vc_id)];
    occupancy = __atomic_load_n(&vc_state->occupancy, __ATOMIC_ACQUIRE);
    while (occupancy > 0U) {
        if (__atomic_compare_exchange_n(&vc_state->occupancy, &occupancy, occupancy - 1U, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            break;
        }
    }

    occupancy = __atomic_load_n(&vc_state->occupancy, __ATOMIC_ACQUIRE);

    if (ctx->cfg.fc_mode == FC_MODE_INFINIFLOW) {
        port_state = &ctx->ingress_port_states[ingress_idx];
        port_occupancy = __atomic_load_n(&port_state->occupancy, __ATOMIC_ACQUIRE);
        while (port_occupancy > 0U) {
            if (__atomic_compare_exchange_n(&port_state->occupancy, &port_occupancy, port_occupancy - 1U,
                                            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                break;
            }
        }
        port_occupancy = __atomic_load_n(&port_state->occupancy, __ATOMIC_ACQUIRE);

        if (vc_dr_out != NULL) {
            *vc_dr_out = __atomic_add_fetch(&vc_state->total_drained, 1U, __ATOMIC_RELAXED);
        }
        if (vc_bklg_out != NULL) {
            *vc_bklg_out = occupancy;
        }
        if (flags_out != NULL) {
            *flags_out = __atomic_exchange_n(&vc_state->pending_feedback_flags, 0U, __ATOMIC_ACQ_REL);
            if ((*flags_out & INFINIFLOW_FEEDBACK_FLAG_TA) != 0U) {
                __atomic_store_n(&vc_state->state, 0U, __ATOMIC_RELEASE);
            }
        }

        __atomic_fetch_add(&port_state->total_drained, 1U, __ATOMIC_RELAXED);
        fccl = __atomic_load_n(&port_state->total_received, __ATOMIC_RELAXED) +
               ((ctx->cfg.port_buffer_pkts > port_occupancy) ? (ctx->cfg.port_buffer_pkts - port_occupancy)
                                                             : 0U);
        return fccl;
    }

    if (vc_dr_out != NULL) {
        *vc_dr_out = 0U;
    }
    if (vc_bklg_out != NULL) {
        *vc_bklg_out = 0U;
    }
    if (flags_out != NULL) {
        *flags_out = 0U;
    }
    return __atomic_load_n(&vc_state->total_received, __ATOMIC_RELAXED) +
           ((vc_state->capacity > occupancy) ? (vc_state->capacity - occupancy) : 0U);
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

        if (ctx->cfg.fc_mode != FC_MODE_NONE && ctx->fc_ops != NULL && ctx->fc_ops->calc_deq_limit != NULL) {
            want_deq = ctx->fc_ops->calc_deq_limit(ctx, egress_idx, vc_id, want_deq);
            if (want_deq == 0U) {
                continue;
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
                    uint64_t feedback_fccl = 0;
                    uint64_t feedback_vc_dr = 0;
                    uint64_t feedback_vc_bklg = 0;
                    uint32_t feedback_flags = 0;

                    feedback_fccl = release_ingress_credit(ctx, ingress_idx, vc_id, &feedback_vc_dr,
                                                           &feedback_vc_bklg, &feedback_flags);

                    __atomic_fetch_add(
                        &ctx->vc_stats[switch_ingress_vc_state_index(ctx, ingress_idx, vc_id)].drop_other_pkts,
                        1U, __ATOMIC_RELAXED);
                    if (ctx->cfg.fc_mode != FC_MODE_NONE) {
                        enqueue_feedback_msg(ctx, ingress_idx, vc_id, feedback_fccl, feedback_vc_dr,
                                             feedback_vc_bklg, feedback_flags, &invalid_eth_hdr->src_addr);
                    }
                }
                rte_pktmbuf_free(mbuf);
                continue;
            }

            eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
            rte_ether_addr_copy(&eth_hdr->src_addr, &feedback_dst_addrs[n_tx_candidates]);
            rte_ether_addr_copy(&ctx->egress_macs[egress_idx], &eth_hdr->src_addr);
            if (n_tx_candidates == 0U && ctx->cfg.fc_mode != FC_MODE_NONE && ctx->fc_ops != NULL &&
                ctx->fc_ops->on_tx_prepare != NULL) {
                fc_data_header_t *fc_hdr = (fc_data_header_t *)((char *)eth_hdr + sizeof(*eth_hdr));

                ctx->fc_ops->on_tx_prepare(ctx, egress_idx, vc_id, fc_hdr);
            }
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
            bool ta_sent = false;

            if (ctx->cfg.fc_mode != FC_MODE_NONE && n_tx > 0U && candidates[0]->pkt_len >=
                                                             sizeof(struct rte_ether_hdr) + FC_DATA_HEADER_SIZE) {
                const struct rte_ether_hdr *eth_hdr =
                    rte_pktmbuf_mtod(candidates[0], const struct rte_ether_hdr *);
                const fc_data_header_t *fc_hdr = (const fc_data_header_t *)((const char *)eth_hdr + sizeof(*eth_hdr));

                ta_sent = (fc_be32_to_cpu(fc_hdr->flags) & FC_DATA_FLAG_TA) != 0U;
            }

            total_tx += n_tx;

            if (ctx->cfg.fc_mode != FC_MODE_NONE && ctx->fc_ops != NULL && ctx->fc_ops->on_tx_success != NULL &&
                n_tx > 0U) {
                ctx->fc_ops->on_tx_success(ctx, egress_idx, vc_id, n_tx, ta_sent);
            }

            for (k = 0; k < n_tx; k++) {
                uint16_t ingress_idx = candidate_ingress_idxs[k];
                uint64_t feedback_fccl = 0;
                uint64_t feedback_vc_dr = 0;
                uint64_t feedback_vc_bklg = 0;
                uint32_t feedback_flags = 0;

                if (ingress_idx < ctx->cfg.nb_ingress_ports) {
                    feedback_fccl = release_ingress_credit(ctx, ingress_idx, vc_id, &feedback_vc_dr,
                                                           &feedback_vc_bklg, &feedback_flags);
                    if (ctx->cfg.fc_mode != FC_MODE_NONE) {
                        enqueue_feedback_msg(ctx, ingress_idx, vc_id, feedback_fccl, feedback_vc_dr,
                                             feedback_vc_bklg, feedback_flags, &feedback_dst_addrs[k]);
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
                    uint64_t feedback_vc_dr = 0;
                    uint64_t feedback_vc_bklg = 0;
                    uint32_t feedback_flags = 0;

                    if (ingress_idx < ctx->cfg.nb_ingress_ports) {
                        uint64_t feedback_fccl = release_ingress_credit(ctx, ingress_idx, vc_id, &feedback_vc_dr,
                                                                       &feedback_vc_bklg, &feedback_flags);

                        __atomic_fetch_add(
                            &ctx->vc_stats[switch_ingress_vc_state_index(ctx, ingress_idx, vc_id)]
                                 .drop_other_pkts,
                            1U, __ATOMIC_RELAXED);
                        if (ctx->cfg.fc_mode != FC_MODE_NONE) {
                            enqueue_feedback_msg(ctx, ingress_idx, vc_id, feedback_fccl, feedback_vc_dr,
                                                 feedback_vc_bklg, feedback_flags, &feedback_dst_addrs[k]);
                        }
                    }
                }

                rte_pktmbuf_free(candidates[k]);
            }
        }
    }

    return total_tx;
}
