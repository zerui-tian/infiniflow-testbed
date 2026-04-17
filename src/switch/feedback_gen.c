#include "switch/switch_ctx.h"
#include "core/fc_header.h"

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

uint32_t switch_feedback_gen_run_tick(switch_ctx_t *ctx, uint16_t ingress_idx) {
    struct rte_mbuf *tx_pkts[256];
    switch_feedback_msg_t *tx_msgs[256];
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

    while (prepared < burst) {
        switch_feedback_msg_t *msg = NULL;
        struct rte_mbuf *mbuf = NULL;
        char *packet = NULL;
        struct rte_ether_hdr *eth_hdr = NULL;
        cbfc_feedback_header_t *cbfc_hdr = NULL;
        infiniflow_feedback_header_t *infi_hdr = NULL;
        uint32_t packet_len = sizeof(*eth_hdr) + CBFC_FEEDBACK_HEADER_SIZE;

        if (rte_ring_sc_dequeue(ctx->feedback_queues[ingress_idx], (void **)&msg) != 0) {
            break;
        }

        mbuf = rte_pktmbuf_alloc(ctx->mbuf_pool);
        if (mbuf == NULL) {
            if (rte_ring_mp_enqueue(ctx->feedback_queues[ingress_idx], msg) != 0) {
                rte_ring_mp_enqueue(ctx->feedback_free_queues[ingress_idx], msg);
            }
            break;
        }

        if (ctx->cfg.fc_mode == FC_MODE_INFINIFLOW) {
            packet_len = sizeof(*eth_hdr) + INFINIFLOW_FEEDBACK_HEADER_SIZE;
        }

        packet = rte_pktmbuf_append(mbuf, packet_len);
        if (packet == NULL) {
            rte_pktmbuf_free(mbuf);
            if (rte_ring_mp_enqueue(ctx->feedback_queues[ingress_idx], msg) != 0) {
                rte_ring_mp_enqueue(ctx->feedback_free_queues[ingress_idx], msg);
            }
            continue;
        }

        eth_hdr = (struct rte_ether_hdr *)packet;
        rte_ether_addr_copy(&msg->dst_addr, &eth_hdr->dst_addr);
        rte_ether_addr_copy(&src_mac, &eth_hdr->src_addr);
        if (ctx->cfg.fc_mode == FC_MODE_INFINIFLOW) {
            infi_hdr = (infiniflow_feedback_header_t *)(packet + sizeof(*eth_hdr));
            eth_hdr->ether_type = rte_cpu_to_be_16(INFINIFLOW_FEEDBACK_ETHER_TYPE);
            infi_hdr->vc_id = rte_cpu_to_be_32(msg->vc_id);
            infi_hdr->flags = fc_cpu_to_be32(msg->flags);
            infi_hdr->vc_dr = fc_cpu_to_be64(msg->vc_dr);
            infi_hdr->vc_bklg = fc_cpu_to_be64(msg->vc_bklg);
            infi_hdr->fccl = fc_cpu_to_be64(msg->fccl);
        } else {
            cbfc_hdr = (cbfc_feedback_header_t *)(packet + sizeof(*eth_hdr));
            eth_hdr->ether_type = rte_cpu_to_be_16(CBFC_FEEDBACK_ETHER_TYPE);
            cbfc_hdr->vc_id = rte_cpu_to_be_32(msg->vc_id);
            cbfc_hdr->fccl = fc_cpu_to_be64(msg->fccl);
        }
        tx_pkts[prepared] = mbuf;
        tx_msgs[prepared] = msg;
        prepared++;
    }

    if (prepared == 0U) {
        return 0;
    }

    tx_count = rte_eth_tx_burst(ingress_port, ctx->cfg.ingress_tx_queue_id, tx_pkts, (uint16_t)prepared);
    __atomic_fetch_add(&ctx->feedback_stats[ingress_idx].tx_ok_pkts, tx_count, __ATOMIC_RELAXED);

    for (i = 0; i < tx_count; i++) {
        if (rte_ring_mp_enqueue(ctx->feedback_free_queues[ingress_idx], tx_msgs[i]) != 0) {
            RTE_LOG(ERR, USER1, "switch: failed to return sent feedback msg to free ring\n");
        }
    }

    for (i = (uint32_t)tx_count; i < prepared; i++) {
        rte_pktmbuf_free(tx_pkts[i]);
        __atomic_fetch_add(&ctx->feedback_stats[ingress_idx].tx_retry_pkts, 1U, __ATOMIC_RELAXED);
        if (rte_ring_mp_enqueue(ctx->feedback_queues[ingress_idx], tx_msgs[i]) != 0) {
            if (rte_ring_mp_enqueue(ctx->feedback_free_queues[ingress_idx], tx_msgs[i]) != 0) {
                RTE_LOG(ERR, USER1, "switch: failed to recycle unsent feedback msg\n");
            }
        }
    }

    return tx_count;
}
