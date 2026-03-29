#include "sender/sender_ctx.h"

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
        }

        n_deq = rte_ring_sc_dequeue_burst(ctx->vc_queues[i].ring, (void **)burst, max_deq, NULL);
        if (n_deq == 0) {
            continue;
        }

        n_tx = rte_eth_tx_burst(ctx->cfg.port_id, ctx->cfg.tx_queue_id, burst, (uint16_t)n_deq);
        total_tx += n_tx;
        ctx->total_pkts_tx += n_tx;

        if (ctx->cfg.fc_mode == FC_MODE_CBFC && n_tx > 0) {
            sender_vc_fc_state_t *st = &ctx->vc_fc_states[i];
            __atomic_fetch_add(&st->fctbs, n_tx, __ATOMIC_RELAXED);
        }

        for (j = n_tx; j < n_deq; j++) {
            rte_pktmbuf_free(burst[j]);
        }
    }

    return total_tx;
}
