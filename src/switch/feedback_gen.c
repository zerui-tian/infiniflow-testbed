#include "switch/switch_ctx.h"
#include "core/fc_header.h"

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

uint32_t switch_feedback_gen_run_tick(switch_ctx_t *ctx, uint16_t ingress_idx) {
    struct rte_mbuf *tx_pkts[256];
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

    for (prepared = 0; prepared < burst; prepared++) {
        switch_feedback_msg_t *msg = NULL;
        struct rte_mbuf *mbuf = NULL;
        char *packet = NULL;
        struct rte_ether_hdr *eth_hdr = NULL;
        cbfc_feedback_header_t *fb_hdr = NULL;

        if (rte_ring_sc_dequeue(ctx->feedback_queues[ingress_idx], (void **)&msg) != 0) {
            break;
        }

        mbuf = rte_pktmbuf_alloc(ctx->mbuf_pool);
        if (mbuf == NULL) {
            rte_ring_sp_enqueue(ctx->feedback_free_queues[ingress_idx], msg);
            break;
        }

        packet = rte_pktmbuf_append(mbuf, sizeof(*eth_hdr) + CBFC_FEEDBACK_HEADER_SIZE);
        if (packet == NULL) {
            rte_pktmbuf_free(mbuf);
            rte_ring_sp_enqueue(ctx->feedback_free_queues[ingress_idx], msg);
            continue;
        }

        eth_hdr = (struct rte_ether_hdr *)packet;
        fb_hdr = (cbfc_feedback_header_t *)(packet + sizeof(*eth_hdr));
        rte_ether_addr_copy(&msg->dst_addr, &eth_hdr->dst_addr);
        rte_ether_addr_copy(&src_mac, &eth_hdr->src_addr);
        eth_hdr->ether_type = rte_cpu_to_be_16(CBFC_FEEDBACK_ETHER_TYPE);
        fb_hdr->vc_id = rte_cpu_to_be_32(msg->vc_id);
        fb_hdr->fccl = fc_cpu_to_be64(msg->fccl);
        tx_pkts[prepared] = mbuf;

        rte_ring_sp_enqueue(ctx->feedback_free_queues[ingress_idx], msg);
    }

    if (prepared == 0U) {
        return 0;
    }

    tx_count = rte_eth_tx_burst(ingress_port, ctx->cfg.ingress_tx_queue_id, tx_pkts, (uint16_t)prepared);
    ctx->total_feedback_sent += tx_count;

    for (i = tx_count; i < prepared; i++) {
        rte_pktmbuf_free(tx_pkts[i]);
    }

    return tx_count;
}
