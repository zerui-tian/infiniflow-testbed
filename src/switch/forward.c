#include "switch/switch_ctx.h"
#include "core/fc_header.h"

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

static uint16_t ingress_index_from_port(const switch_ctx_t *ctx, uint16_t port_id) {
    uint16_t i = 0;

    for (i = 0; i < ctx->cfg.nb_ingress_ports; i++) {
        if (ctx->cfg.ingress_ports[i] == port_id) {
            return i;
        }
    }
    return UINT16_MAX;
}

static void enqueue_feedback_msg(switch_ctx_t *ctx, uint16_t ingress_idx, uint32_t vc_id,
                                 uint64_t fccl, const struct rte_ether_addr *dst_addr) {
    switch_feedback_msg_t *msg = NULL;

    if (ingress_idx >= ctx->cfg.nb_ingress_ports) {
        return;
    }
    if (rte_ring_sc_dequeue(ctx->feedback_free_queues[ingress_idx], (void **)&msg) != 0) {
        return;
    }

    msg->vc_id = vc_id;
    msg->fccl = fccl;
    rte_ether_addr_copy(dst_addr, &msg->dst_addr);
    if (rte_ring_sp_enqueue(ctx->feedback_queues[ingress_idx], msg) != 0) {
        rte_ring_mp_enqueue(ctx->feedback_free_queues[ingress_idx], msg);
        return;
    }
}

uint32_t switch_forward_run_tick(switch_ctx_t *ctx) {
    uint32_t total_tx = 0;
    uint32_t i = 0;

    for (i = 0; i < ctx->cfg.nb_vc; i++) {
        struct rte_mbuf *burst[256];
        struct rte_mbuf *tx_burst[256];
        struct rte_ether_addr feedback_dst_addrs[256];
        uint64_t credit = 0;
        uint32_t want_deq = 1;
        uint32_t n_deq = 0;
        uint32_t n_tx_candidates = 0;
        uint16_t n_tx = 0;
        uint32_t j = 0;

        if (ctx->cfg.fc_mode == FC_MODE_CBFC && ctx->fc_ops != NULL && ctx->fc_ops->calc_credit != NULL) {
            credit = ctx->fc_ops->calc_credit(ctx, i);
            if (credit == 0U) {
                continue;
            }
            if (credit < want_deq) {
                want_deq = (uint32_t)credit;
            }
        }
        if (want_deq == 0U) {
            continue;
        }

        n_deq = rte_ring_sc_dequeue_burst(ctx->vc_queues[i].ring, (void **)burst, want_deq, NULL);
        if (n_deq == 0U) {
            continue;
        }
        __atomic_fetch_sub(&ctx->vc_states[i].occupancy, n_deq, __ATOMIC_RELAXED);

        for (j = 0; j < n_deq; j++) {
            struct rte_mbuf *mbuf = burst[j];
            struct rte_ether_hdr *eth_hdr = NULL;
            const fc_data_header_t *fc_hdr = NULL;
            uint32_t flow_id = 0;
            uint16_t egress_port = 0;

            if (mbuf->pkt_len < sizeof(struct rte_ether_hdr) + FC_DATA_HEADER_SIZE) {
                rte_pktmbuf_free(mbuf);
                continue;
            }

            eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
            fc_hdr = (const fc_data_header_t *)((const char *)eth_hdr + sizeof(*eth_hdr));
            flow_id = rte_be_to_cpu_32(fc_hdr->flow_id);
            egress_port = switch_flow_map_lookup(ctx, flow_id);
            if (egress_port != ctx->cfg.egress_port) {
                rte_pktmbuf_free(mbuf);
                continue;
            }

            rte_ether_addr_copy(&eth_hdr->src_addr, &feedback_dst_addrs[n_tx_candidates]);
            rte_ether_addr_copy(&ctx->egress_mac, &eth_hdr->src_addr);
            tx_burst[n_tx_candidates++] = mbuf;
        }

        if (n_tx_candidates == 0U) {
            continue;
        }

        n_tx = rte_eth_tx_burst(ctx->cfg.egress_port, ctx->cfg.egress_tx_queue_id, tx_burst,
                                (uint16_t)n_tx_candidates);
        total_tx += n_tx;
        ctx->egress_tx_ok_pkts += n_tx;

        for (j = 0; j < n_tx; j++) {
            struct rte_mbuf *mbuf = tx_burst[j];
            uint16_t ingress_idx = ingress_index_from_port(ctx, mbuf->port);
            uint64_t feedback_fccl = 0;

            if (ctx->cfg.fc_mode == FC_MODE_CBFC && ctx->fc_ops != NULL &&
                ctx->fc_ops->on_tx_success != NULL) {
                feedback_fccl = ctx->fc_ops->on_tx_success(ctx, i);
                enqueue_feedback_msg(ctx, ingress_idx, i, feedback_fccl, &feedback_dst_addrs[j]);
            }
            rte_pktmbuf_free(mbuf);
        }
        // TODO: 发送失败时，需要处理失败的情况，比如重发

        for (j = n_tx; j < n_tx_candidates; j++) {
            rte_pktmbuf_free(tx_burst[j]);
            ctx->egress_tx_drop_pkts++;
        }
    }

    return total_tx;
}
