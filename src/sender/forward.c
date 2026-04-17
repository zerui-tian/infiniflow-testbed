#include "sender/sender_ctx.h"
#include "core/fc_header.h"

#include <rte_byteorder.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

uint32_t forward_run_tick(sender_ctx_t *ctx) {
    const uint32_t burst_size = (ctx->cfg.tx_burst_size > 256U) ? 256U : ctx->cfg.tx_burst_size;
    uint32_t total_tx = 0;
    uint16_t i = 0;

    for (i = 0; i < ctx->cfg.nb_vc; i++) {
        struct rte_mbuf *burst[256];
        uint32_t n_deq = 0;
        uint16_t n_tx = 0;
        uint32_t j = 0;
        uint32_t max_deq = burst_size;

        if (ctx->cfg.fc_mode == FC_MODE_CBFC) {
            sender_vc_fc_state_t *st = &ctx->vc_fc_states[i];
            uint64_t fccl = __atomic_load_n(&st->fccl, __ATOMIC_ACQUIRE);
            uint64_t fctbs = __atomic_load_n(&st->fctbs, __ATOMIC_RELAXED);
            uint64_t credit = (fccl > fctbs) ? (fccl - fctbs) : 0U;

            if (credit == 0U) {
                continue;
            }
            max_deq = (credit < (uint64_t)burst_size) ? (uint32_t)credit : burst_size;
        } else if (ctx->cfg.fc_mode == FC_MODE_INFINIFLOW) {
            sender_vc_fc_state_t *st = &ctx->vc_fc_states[i];
            uint64_t port_fccl = __atomic_load_n(&ctx->port_fc_state.fccl, __ATOMIC_ACQUIRE);
            uint64_t port_fctbs = __atomic_load_n(&ctx->port_fc_state.fctbs, __ATOMIC_RELAXED);
            uint64_t threshold = __atomic_load_n(&st->threshold, __ATOMIC_ACQUIRE);
            uint64_t tx_pkts = __atomic_load_n(&st->tx_pkts, __ATOMIC_RELAXED);
            uint64_t vc_dr = __atomic_load_n(&st->vc_dr, __ATOMIC_RELAXED);
            uint64_t credit_pool = (port_fccl > port_fctbs) ? (port_fccl - port_fctbs) : 0U;
            uint64_t inflight = (tx_pkts > vc_dr) ? (tx_pkts - vc_dr) : 0U;
            uint64_t vc_credit = (threshold > inflight) ? (threshold - inflight) : 0U;

            if (credit_pool == 0U || vc_credit == 0U) {
                continue;
            }
            if (credit_pool < (uint64_t)max_deq) {
                max_deq = (uint32_t)credit_pool;
            }
            if (vc_credit < (uint64_t)max_deq) {
                max_deq = (uint32_t)vc_credit;
            }
        }

        n_deq = rte_ring_sc_dequeue_burst(ctx->vc_queues[i].ring, (void **)burst, max_deq, NULL);
        if (n_deq == 0) {
            continue;
        }

        if (ctx->cfg.fc_mode == FC_MODE_INFINIFLOW) {
            sender_vc_fc_state_t *st = &ctx->vc_fc_states[i];
            uint32_t state = __atomic_load_n(&st->state, __ATOMIC_ACQUIRE);

            if (state == SENDER_INFINIFLOW_STATE_TA && burst[0]->pkt_len >=
                                                       sizeof(struct rte_ether_hdr) + FC_DATA_HEADER_SIZE) {
                struct rte_ether_hdr *eth_hdr =
                    rte_pktmbuf_mtod(burst[0], struct rte_ether_hdr *);
                fc_data_header_t *fc_hdr = (fc_data_header_t *)((char *)eth_hdr + sizeof(*eth_hdr));
                uint32_t flags = fc_be32_to_cpu(fc_hdr->flags);

                fc_hdr->flags = fc_cpu_to_be32(flags | FC_DATA_FLAG_TA);
            }
        }

        n_tx = rte_eth_tx_burst(ctx->cfg.port_id, ctx->cfg.tx_queue_id, burst, (uint16_t)n_deq);
        total_tx += n_tx;
        ctx->total_pkts_tx += n_tx;

        if (ctx->cfg.fc_mode == FC_MODE_CBFC && n_tx > 0) {
            sender_vc_fc_state_t *st = &ctx->vc_fc_states[i];
            __atomic_fetch_add(&st->fctbs, n_tx, __ATOMIC_RELAXED);
        } else if (ctx->cfg.fc_mode == FC_MODE_INFINIFLOW && n_tx > 0) {
            sender_vc_fc_state_t *st = &ctx->vc_fc_states[i];
            uint32_t state = __atomic_load_n(&st->state, __ATOMIC_RELAXED);

            __atomic_fetch_add(&ctx->port_fc_state.fctbs, n_tx, __ATOMIC_RELAXED);
            __atomic_fetch_add(&st->tx_pkts, n_tx, __ATOMIC_RELAXED);
            if (state == SENDER_INFINIFLOW_STATE_TA) {
                __atomic_store_n(&st->state, SENDER_INFINIFLOW_STATE_WT, __ATOMIC_RELEASE);
            }
        }

        for (j = n_tx; j < n_deq; j++) {
            rte_pktmbuf_free(burst[j]);
        }
    }

    return total_tx;
}
