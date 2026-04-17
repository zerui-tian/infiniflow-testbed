#include "switch/switch_ctx.h"
#include "core/fc_header.h"

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

static int try_reserve_ingress_slot(switch_ctx_t *ctx, switch_ingress_port_state_t *port_state,
                                    switch_ingress_vc_state_t *vc_state) {
    if (ctx->cfg.fc_mode == FC_MODE_INFINIFLOW) {
        uint64_t occupancy = __atomic_load_n(&port_state->occupancy, __ATOMIC_ACQUIRE);

        while (occupancy < ctx->cfg.port_buffer_pkts) {
            if (__atomic_compare_exchange_n(&port_state->occupancy, &occupancy, occupancy + 1U, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                __atomic_fetch_add(&vc_state->occupancy, 1U, __ATOMIC_RELAXED);
                return 0;
            }
        }
        return -1;
    }

    {
        uint64_t occupancy = __atomic_load_n(&vc_state->occupancy, __ATOMIC_ACQUIRE);

        while (occupancy < vc_state->capacity) {
            if (__atomic_compare_exchange_n(&vc_state->occupancy, &occupancy, occupancy + 1U, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                return 0;
            }
        }
    }

    return -1;
}

int switch_scheduler_run_tick(switch_ctx_t *ctx, uint16_t ingress_idx) {
    struct rte_mbuf *rx_pkts[256];
    uint16_t port_id = ctx->cfg.ingress_ports[ingress_idx];
    uint16_t burst = (ctx->cfg.rx_burst_size > 256U) ? 256U : (uint16_t)ctx->cfg.rx_burst_size;
    uint16_t nb_rx = 0;
    uint16_t i = 0;
    int enqueued = 0;

    nb_rx = rte_eth_rx_burst(port_id, ctx->cfg.ingress_rx_queue_id, rx_pkts, burst);
    if (nb_rx == 0U) {
        return 0;
    }

    for (i = 0; i < nb_rx; i++) {
        struct rte_mbuf *mbuf = rx_pkts[i];
        const struct rte_ether_hdr *eth_hdr = NULL;

        if (mbuf->pkt_len < sizeof(struct rte_ether_hdr)) {
            rte_pktmbuf_free(mbuf);
            continue;
        }

        eth_hdr = rte_pktmbuf_mtod(mbuf, const struct rte_ether_hdr *);
        if (eth_hdr->ether_type != rte_cpu_to_be_16(FC_ETHER_TYPE)) {
            rte_pktmbuf_free(mbuf);
            continue;
        }

        if (mbuf->pkt_len < sizeof(*eth_hdr) + FC_DATA_HEADER_SIZE) {
            rte_pktmbuf_free(mbuf);
            continue;
        }

        {
            const fc_data_header_t *fc_hdr = (const fc_data_header_t *)((const char *)eth_hdr + sizeof(*eth_hdr));
            uint32_t vc_id = rte_be_to_cpu_32(fc_hdr->vc_id);
            uint32_t flow_id = rte_be_to_cpu_32(fc_hdr->flow_id);
            uint16_t egress_port = 0;
            uint16_t egress_idx = UINT16_MAX;
            size_t ingress_state_idx = 0;
            size_t egress_queue_idx = 0;
            size_t stats_idx = 0;
            switch_vc_stats_t *vc_stats = NULL;
            switch_ingress_vc_state_t *ingress_state = NULL;
            switch_ingress_port_state_t *port_state = NULL;
            uint32_t flags = fc_be32_to_cpu(fc_hdr->flags);

            if (vc_id >= ctx->cfg.nb_vc) {
                rte_pktmbuf_free(mbuf);
                continue;
            }

            stats_idx = switch_ingress_vc_state_index(ctx, ingress_idx, vc_id);
            vc_stats = &ctx->vc_stats[stats_idx];
            __atomic_fetch_add(&vc_stats->rx_pkts, 1U, __ATOMIC_RELAXED);

            egress_port = switch_flow_map_lookup(ctx, flow_id);
            egress_idx = switch_egress_index_from_port(ctx, egress_port);
            if (egress_idx == UINT16_MAX) {
                rte_pktmbuf_free(mbuf);
                __atomic_fetch_add(&vc_stats->drop_other_pkts, 1U, __ATOMIC_RELAXED);
                continue;
            }

            ingress_state_idx = switch_ingress_vc_state_index(ctx, ingress_idx, vc_id);
            ingress_state = &ctx->ingress_vc_states[ingress_state_idx];
            port_state = &ctx->ingress_port_states[ingress_idx];
            if (try_reserve_ingress_slot(ctx, port_state, ingress_state) != 0) {
                rte_pktmbuf_free(mbuf);
                __atomic_fetch_add(&vc_stats->drop_capacity_pkts, 1U, __ATOMIC_RELAXED);
                continue;
            }

            if ((flags & FC_DATA_FLAG_TA) != 0U) {
                __atomic_store_n(&ingress_state->state, 1U, __ATOMIC_RELEASE);
                __atomic_fetch_or(&ingress_state->pending_feedback_flags, INFINIFLOW_FEEDBACK_FLAG_TA,
                                  __ATOMIC_RELAXED);
            }

            egress_queue_idx = switch_egress_vc_state_index(ctx, egress_idx, vc_id);
            if (rte_ring_mp_enqueue(ctx->egress_vc_queues[egress_queue_idx].ring, mbuf) != 0) {
                __atomic_fetch_sub(&ingress_state->occupancy, 1U, __ATOMIC_RELAXED);
                if (ctx->cfg.fc_mode == FC_MODE_INFINIFLOW) {
                    __atomic_fetch_sub(&port_state->occupancy, 1U, __ATOMIC_RELAXED);
                }
                rte_pktmbuf_free(mbuf);
                __atomic_fetch_add(&vc_stats->drop_other_pkts, 1U, __ATOMIC_RELAXED);
                continue;
            }

            __atomic_fetch_add(&ingress_state->total_received, 1U, __ATOMIC_RELAXED);
            if (ctx->cfg.fc_mode == FC_MODE_INFINIFLOW) {
                __atomic_fetch_add(&port_state->total_received, 1U, __ATOMIC_RELAXED);
            }
            enqueued++;
        }
    }

    return enqueued;
}
