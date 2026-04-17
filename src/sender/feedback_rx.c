#include "sender/sender_ctx.h"
#include "core/fc_header.h"

#include <inttypes.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_log.h>
#include <rte_mbuf.h>

uint32_t sender_feedback_rx_run_tick(sender_ctx_t *ctx) {
    struct rte_mbuf *rx_pkts[256];
    uint32_t processed = 0;
    uint16_t burst = (ctx->cfg.rx_burst_size > 256U) ? 256U : (uint16_t)ctx->cfg.rx_burst_size;
    uint16_t nb_rx = 0;
    uint16_t i = 0;

    if (ctx->cfg.fc_mode == FC_MODE_NONE) {
        return 0;
    }

    nb_rx = rte_eth_rx_burst(ctx->cfg.port_id, ctx->cfg.rx_queue_id, rx_pkts, burst);
    if (nb_rx == 0U) {
        return 0;
    }

    for (i = 0; i < nb_rx; i++) {
        struct rte_mbuf *mbuf = rx_pkts[i];
        const struct rte_ether_hdr *eth_hdr = NULL;
        const cbfc_feedback_header_t *cbfc_hdr = NULL;
        const infiniflow_feedback_header_t *infi_hdr = NULL;
        uint32_t vc_id = 0;
        uint64_t fccl = 0;
        uint64_t vc_dr = 0;
        uint64_t vc_bklg = 0;
        uint32_t flags = 0;

        if (mbuf->pkt_len < sizeof(struct rte_ether_hdr)) {
            continue;
        }

        eth_hdr = rte_pktmbuf_mtod(mbuf, const struct rte_ether_hdr *);
        if (ctx->cfg.fc_mode == FC_MODE_CBFC) {
            if (eth_hdr->ether_type != rte_cpu_to_be_16(CBFC_FEEDBACK_ETHER_TYPE) ||
                mbuf->pkt_len < (uint32_t)(sizeof(struct rte_ether_hdr) + CBFC_FEEDBACK_HEADER_SIZE)) {
                continue;
            }

            cbfc_hdr = (const cbfc_feedback_header_t *)((const char *)eth_hdr + sizeof(*eth_hdr));
            vc_id = rte_be_to_cpu_32(cbfc_hdr->vc_id);
            if (vc_id >= ctx->cfg.nb_vc) {
                continue;
            }

            fccl = fc_be64_to_cpu(cbfc_hdr->fccl);
            {
                sender_vc_fc_state_t *vc_state = &ctx->vc_fc_states[vc_id];
                uint64_t old_fccl = __atomic_load_n(&vc_state->fccl, __ATOMIC_RELAXED);
                uint64_t fctbs = __atomic_load_n(&vc_state->fctbs, __ATOMIC_RELAXED);
                uint64_t old_credit = (old_fccl > fctbs) ? (old_fccl - fctbs) : 0U;
                uint64_t new_credit = (fccl > fctbs) ? (fccl - fctbs) : 0U;

                if (old_fccl >= fccl) {
                    continue;
                }

                RTE_LOG(DEBUG, USER1,
                        "[CBFC][sender][feedback] vc=%" PRIu32
                        " old_fccl=%" PRIu64 " new_fccl=%" PRIu64
                        " fctbs=%" PRIu64 " old_credit=%" PRIu64 " new_credit=%" PRIu64 "\n",
                        vc_id, old_fccl, fccl, fctbs, old_credit, new_credit);
            }
            __atomic_store_n(&ctx->vc_fc_states[vc_id].fccl, fccl, __ATOMIC_RELEASE);
        } else if (ctx->cfg.fc_mode == FC_MODE_INFINIFLOW) {
            sender_vc_fc_state_t *vc_state = NULL;
            uint64_t tx_pkts = 0;
            uint64_t inflight = 0;
            uint64_t threshold = 0;
            uint64_t port_fctbs = 0;
            uint64_t limit = 1U;

            if (eth_hdr->ether_type != rte_cpu_to_be_16(INFINIFLOW_FEEDBACK_ETHER_TYPE) ||
                mbuf->pkt_len < (uint32_t)(sizeof(struct rte_ether_hdr) + INFINIFLOW_FEEDBACK_HEADER_SIZE)) {
                continue;
            }

            infi_hdr = (const infiniflow_feedback_header_t *)((const char *)eth_hdr + sizeof(*eth_hdr));
            vc_id = rte_be_to_cpu_32(infi_hdr->vc_id);
            if (vc_id >= ctx->cfg.nb_vc) {
                continue;
            }

            fccl = fc_be64_to_cpu(infi_hdr->fccl);
            vc_dr = fc_be64_to_cpu(infi_hdr->vc_dr);
            vc_bklg = fc_be64_to_cpu(infi_hdr->vc_bklg);
            flags = fc_be32_to_cpu(infi_hdr->flags);

            vc_state = &ctx->vc_fc_states[vc_id];
            __atomic_store_n(&ctx->port_fc_state.fccl, fccl, __ATOMIC_RELEASE);
            __atomic_store_n(&vc_state->vc_dr, vc_dr, __ATOMIC_RELEASE);
            __atomic_store_n(&vc_state->vc_bklg, vc_bklg, __ATOMIC_RELEASE);

            if ((flags & INFINIFLOW_FEEDBACK_FLAG_TA) != 0U &&
                __atomic_load_n(&vc_state->state, __ATOMIC_ACQUIRE) == SENDER_INFINIFLOW_STATE_WT) {
                __atomic_store_n(&vc_state->state, SENDER_INFINIFLOW_STATE_UN, __ATOMIC_RELEASE);
            }

            if (__atomic_load_n(&vc_state->state, __ATOMIC_ACQUIRE) == SENDER_INFINIFLOW_STATE_UN) {
                tx_pkts = __atomic_load_n(&vc_state->tx_pkts, __ATOMIC_RELAXED);
                threshold = __atomic_load_n(&vc_state->threshold, __ATOMIC_RELAXED);
                inflight = (tx_pkts > vc_dr) ? (tx_pkts - vc_dr) : 0U;
                port_fctbs = __atomic_load_n(&ctx->port_fc_state.fctbs, __ATOMIC_RELAXED);

                if (vc_bklg >= ctx->cfg.qmax) {
                    uint64_t reduction = vc_bklg - ctx->cfg.qmin;
                    uint64_t new_threshold = (inflight > reduction) ? (inflight - reduction) : 1U;

                    __atomic_store_n(&vc_state->threshold, new_threshold, __ATOMIC_RELEASE);
                    __atomic_store_n(&vc_state->state, SENDER_INFINIFLOW_STATE_TA, __ATOMIC_RELEASE);
                } else if (vc_bklg < ctx->cfg.qmin) {
                    uint64_t headroom = (fccl > port_fctbs) ? (fccl - port_fctbs) : 1U;
                    uint64_t increase = ctx->cfg.qmin - vc_bklg;
                    uint64_t new_threshold = threshold + increase;

                    limit = (headroom > 0U) ? headroom : 1U;
                    if (new_threshold > limit) {
                        new_threshold = limit;
                    }
                    if (new_threshold == 0U) {
                        new_threshold = 1U;
                    }
                    __atomic_store_n(&vc_state->threshold, new_threshold, __ATOMIC_RELEASE);
                    __atomic_store_n(&vc_state->state, SENDER_INFINIFLOW_STATE_TA, __ATOMIC_RELEASE);
                }
            }
        } else {
            continue;
        }
        __atomic_fetch_add(&ctx->feedback_rx_pkts[vc_id], 1U, __ATOMIC_RELAXED);
        processed++;
    }

    for (i = 0; i < nb_rx; i++) {
        rte_pktmbuf_free(rx_pkts[i]);
    }

    return processed;
}
