#include "switch/switch_ctx.h"
#include "core/fc_header.h"

#include <inttypes.h>
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_log.h>
#include <rte_mbuf.h>

uint32_t switch_feedback_handler_run_tick(switch_ctx_t *ctx) {
    static bool g_feedback_rx_port_logged = false;
    struct rte_mbuf *rx_pkts[256];
    uint16_t burst = (ctx->cfg.rx_burst_size > 256U) ? 256U : (uint16_t)ctx->cfg.rx_burst_size;
    uint16_t nb_rx = 0;
    uint16_t i = 0;
    uint32_t processed = 0;

    nb_rx = rte_eth_rx_burst(ctx->cfg.egress_port, ctx->cfg.egress_rx_queue_id, rx_pkts, burst);
    if (nb_rx == 0U) {
        return 0;
    }
    if (!g_feedback_rx_port_logged) {
        RTE_LOG(INFO, USER1, "switch: feedback RX on egress port %" PRIu16 "\n", ctx->cfg.egress_port);
        g_feedback_rx_port_logged = true;
    }

    for (i = 0; i < nb_rx; i++) {
        struct rte_mbuf *mbuf = rx_pkts[i];
        const struct rte_ether_hdr *eth_hdr = NULL;
        const cbfc_feedback_header_t *fb_hdr = NULL;
        uint32_t vc_id = 0;
        uint64_t fccl = 0;

        if (mbuf->pkt_len < sizeof(struct rte_ether_hdr) + CBFC_FEEDBACK_HEADER_SIZE) {
            rte_pktmbuf_free(mbuf);
            continue;
        }

        eth_hdr = rte_pktmbuf_mtod(mbuf, const struct rte_ether_hdr *);
        if (eth_hdr->ether_type != rte_cpu_to_be_16(CBFC_FEEDBACK_ETHER_TYPE)) {
            rte_pktmbuf_free(mbuf);
            continue;
        }

        fb_hdr = (const cbfc_feedback_header_t *)((const char *)eth_hdr + sizeof(*eth_hdr));
        vc_id = rte_be_to_cpu_32(fb_hdr->vc_id);
        if (vc_id < ctx->cfg.nb_vc && ctx->fc_ops != NULL && ctx->fc_ops->on_feedback_rx != NULL) {
            fccl = fc_be64_to_cpu(fb_hdr->fccl);
            ctx->fc_ops->on_feedback_rx(ctx, vc_id, fccl);
            processed++;
        }

        rte_pktmbuf_free(mbuf);
    }

    return processed;
}
