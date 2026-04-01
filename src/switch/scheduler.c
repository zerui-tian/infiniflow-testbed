#include "switch/switch_ctx.h"
#include "core/fc_header.h"

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

static int try_reserve_vc_slot(switch_vc_fc_state_t *state) {
    uint64_t old_occupancy = 0;

    old_occupancy = __atomic_fetch_add(&state->occupancy, 1U, __ATOMIC_RELAXED);
    if (old_occupancy >= state->capacity) {
        __atomic_fetch_sub(&state->occupancy, 1U, __ATOMIC_RELAXED);
        return -1;
    }
    return 0;
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
        if (eth_hdr->ether_type == rte_cpu_to_be_16(FC_ETHER_TYPE)) {
            const fc_data_header_t *fc_hdr = NULL;
            uint32_t vc_id = 0;
            switch_vc_fc_state_t *vc_state = NULL;
            switch_vc_stats_t *vc_stats = NULL;
            size_t stats_idx = 0;

            if (mbuf->pkt_len < sizeof(*eth_hdr) + FC_DATA_HEADER_SIZE) {
                rte_pktmbuf_free(mbuf);
                continue;
            }

            fc_hdr = (const fc_data_header_t *)((const char *)eth_hdr + sizeof(*eth_hdr));
            vc_id = rte_be_to_cpu_32(fc_hdr->vc_id);
            if (vc_id >= ctx->cfg.nb_vc) {
                rte_pktmbuf_free(mbuf);
                continue;
            }

            stats_idx = (size_t)ingress_idx * ctx->cfg.nb_vc + vc_id;
            vc_stats = &ctx->vc_stats[stats_idx];
            __atomic_fetch_add(&vc_stats->rx_pkts, 1U, __ATOMIC_RELAXED);

            vc_state = &ctx->vc_states[vc_id];
            if (try_reserve_vc_slot(vc_state) != 0) {
                rte_pktmbuf_free(mbuf);
                __atomic_fetch_add(&vc_stats->drop_capacity_pkts, 1U, __ATOMIC_RELAXED);
                continue;
            }

            if (rte_ring_mp_enqueue(ctx->vc_queues[vc_id].ring, mbuf) != 0) {
                __atomic_fetch_sub(&vc_state->occupancy, 1U, __ATOMIC_RELAXED);
                rte_pktmbuf_free(mbuf);
                __atomic_fetch_add(&vc_stats->drop_other_pkts, 1U, __ATOMIC_RELAXED);
                continue;
            }

            enqueued++;
            continue;
        }

        rte_pktmbuf_free(mbuf);
    }

    return enqueued;
}
