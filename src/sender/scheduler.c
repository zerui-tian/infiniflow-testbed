#include "sender/sender_ctx.h"
#include "core/fc_header.h"

#include <string.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

int scheduler_run_tick(sender_ctx_t *ctx) {
    uint32_t i = 0;
    int enqueued = 0;

    for (i = 0; i < ctx->nb_active; i++) {
        uint32_t flow_id = ctx->active_ids[i];
        flow_desc_t *flow = &ctx->flows[flow_id];
        struct rte_mbuf *mbuf = NULL;
        char *packet = NULL;
        struct rte_ether_hdr *eth_hdr = NULL;
        struct rte_ipv4_hdr *ipv4_hdr = NULL;
        fc_header_t *fc_hdr = NULL;
        char *payload = NULL;
        uint32_t l3_l4_len = 0;
        uint32_t payload_len = 0;
        struct rte_ether_addr src_mac;
        struct rte_ether_addr dst_mac;
        vc_queue_t *q = NULL;

        if (flow->state != FLOW_STATE_ACTIVE) {
            continue;
        }
        if (flow->sent_count >= flow->len) {
            flow->state = FLOW_STATE_DEAD;
            continue;
        }
        if (flow->vc >= ctx->cfg.nb_vc) {
            flow->state = FLOW_STATE_DEAD;
            continue;
        }

        q = &ctx->vc_queues[flow->vc];
        mbuf = rte_pktmbuf_alloc(ctx->mbuf_pool);
        if (mbuf == NULL) {
            continue;
        }

        packet = rte_pktmbuf_append(mbuf, ctx->cfg.packet_size);
        if (packet == NULL) {
            rte_pktmbuf_free(mbuf);
            continue;
        }

        eth_hdr = (struct rte_ether_hdr *)packet;
        ipv4_hdr = (struct rte_ipv4_hdr *)(packet + sizeof(*eth_hdr));
        fc_hdr = (fc_header_t *)((char *)ipv4_hdr + sizeof(*ipv4_hdr));
        payload = (char *)(fc_hdr + 1);

        l3_l4_len = (uint32_t)(sizeof(*ipv4_hdr) + sizeof(*fc_hdr));
        payload_len = ctx->cfg.packet_size - (uint32_t)sizeof(*eth_hdr) - l3_l4_len;

        memset(&dst_mac, 0xFF, sizeof(dst_mac));
        rte_eth_macaddr_get(ctx->cfg.port_id, &src_mac);
        rte_ether_addr_copy(&dst_mac, &eth_hdr->dst_addr);
        rte_ether_addr_copy(&src_mac, &eth_hdr->src_addr);
        eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

        ipv4_hdr->version_ihl = RTE_IPV4_VHL_DEF;
        ipv4_hdr->type_of_service = 0;
        ipv4_hdr->total_length = rte_cpu_to_be_16((uint16_t)l3_l4_len + (uint16_t)payload_len);
        ipv4_hdr->packet_id = rte_cpu_to_be_16((uint16_t)flow->sent_count);
        ipv4_hdr->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
        ipv4_hdr->time_to_live = 64;
        ipv4_hdr->next_proto_id = FC_IPPROTO;
        ipv4_hdr->hdr_checksum = 0;
        ipv4_hdr->src_addr = rte_cpu_to_be_32(FC_IPV4_SRC_ADDR);
        ipv4_hdr->dst_addr = rte_cpu_to_be_32(flow->dest);
        ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);

        fc_hdr->flow_id = rte_cpu_to_be_32(flow->fid);
        memset(payload, 0, payload_len);

        if (rte_ring_sp_enqueue(q->ring, mbuf) != 0) {
            rte_pktmbuf_free(mbuf);
            continue;
        }

        flow->sent_count++;
        enqueued++;
        ctx->total_pkts_enqueued++;

        if (flow->sent_count >= flow->len) {
            flow->state = FLOW_STATE_DEAD;
        }
    }

    return enqueued;
}
