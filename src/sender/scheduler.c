#include "sender/sender_ctx.h"
#include "core/fc_header.h"

#include <string.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
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
        fc_data_header_t *fc_hdr = NULL;
        char *payload = NULL;
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

        if (ctx->cfg.fc_mode == FC_MODE_CBFC) {
            sender_vc_fc_state_t *fc_state = &ctx->vc_fc_states[flow->vc];
            uint64_t fccl = __atomic_load_n(&fc_state->fccl, __ATOMIC_ACQUIRE);
            uint64_t fctbs = __atomic_load_n(&fc_state->fctbs, __ATOMIC_RELAXED);

            if (fccl <= fctbs) {
                continue;
            }
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
        fc_hdr = (fc_data_header_t *)(packet + sizeof(*eth_hdr));
        payload = (char *)(fc_hdr + 1);

        payload_len = ctx->cfg.packet_size - (uint32_t)sizeof(*eth_hdr) - (uint32_t)sizeof(*fc_hdr);

        memset(&dst_mac, 0xFF, sizeof(dst_mac));
        rte_eth_macaddr_get(ctx->cfg.port_id, &src_mac);
        rte_ether_addr_copy(&dst_mac, &eth_hdr->dst_addr);
        rte_ether_addr_copy(&src_mac, &eth_hdr->src_addr);
        eth_hdr->ether_type = rte_cpu_to_be_16(FC_ETHER_TYPE);

        fc_hdr->flow_id = rte_cpu_to_be_32(flow->fid);
        fc_hdr->vc_id = rte_cpu_to_be_32(flow->vc);
        memset(payload, 0, payload_len);

        if (rte_ring_sp_enqueue(q->ring, mbuf) != 0) {
            rte_pktmbuf_free(mbuf);
            continue;
        }

        flow->sent_count++;
        if (ctx->cfg.fc_mode == FC_MODE_CBFC) {
            __atomic_fetch_add(&ctx->vc_fc_states[flow->vc].fctbs, 1U, __ATOMIC_RELAXED);
        }
        enqueued++;
        ctx->total_pkts_enqueued++;

        if (flow->sent_count >= flow->len) {
            flow->state = FLOW_STATE_DEAD;
        }
    }

    return enqueued;
}
