#include "sender/sender_ctx.h"
#include "core/fc_header.h"

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

uint32_t sender_feedback_rx_run_tick(sender_ctx_t *ctx) {
    struct rte_mbuf *rx_pkts[256];
    uint32_t processed = 0;
    uint16_t burst = (ctx->cfg.rx_burst_size > 256U) ? 256U : (uint16_t)ctx->cfg.rx_burst_size;
    uint16_t nb_rx = 0;
    uint16_t i = 0;

    if (ctx->cfg.fc_mode != FC_MODE_CBFC) {
        return 0;
    }

    nb_rx = rte_eth_rx_burst(ctx->cfg.port_id, ctx->cfg.rx_queue_id, rx_pkts, burst);
    if (nb_rx == 0U) {
        return 0;
    }

    for (i = 0; i < nb_rx; i++) {
        struct rte_mbuf *mbuf = rx_pkts[i];
        const struct rte_ether_hdr *eth_hdr = NULL;
        const cbfc_feedback_header_t *fb_hdr = NULL;
        uint32_t vc_id = 0;
        uint64_t fccl = 0;

        if (mbuf->pkt_len < (uint32_t)(sizeof(struct rte_ether_hdr) + CBFC_FEEDBACK_HEADER_SIZE)) {
            continue;
        }

        eth_hdr = rte_pktmbuf_mtod(mbuf, const struct rte_ether_hdr *);
        if (eth_hdr->ether_type != rte_cpu_to_be_16(CBFC_FEEDBACK_ETHER_TYPE)) {
            continue;
        }

        fb_hdr = (const cbfc_feedback_header_t *)((const char *)eth_hdr + sizeof(*eth_hdr));
        vc_id = rte_be_to_cpu_32(fb_hdr->vc_id);
        if (vc_id >= ctx->cfg.nb_vc) {
            continue;
        }

        fccl = fc_be64_to_cpu(fb_hdr->fccl);
        __atomic_store_n(&ctx->vc_fc_states[vc_id].fccl, fccl, __ATOMIC_RELEASE);
        processed++;
    }

    for (i = 0; i < nb_rx; i++) {
        rte_pktmbuf_free(rx_pkts[i]);
    }

    return processed;
}
