#include "sender/sender_ctx.h"

#include <string.h>

#include <rte_mbuf.h>
#include <rte_ring.h>

int scheduler_run_tick(sender_ctx_t *ctx) {
    uint32_t i = 0;
    int enqueued = 0;

    for (i = 0; i < ctx->nb_active; i++) {
        uint32_t flow_id = ctx->active_ids[i];
        flow_desc_t *flow = &ctx->flows[flow_id];
        struct rte_mbuf *mbuf = NULL;
        char *payload = NULL;
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

        payload = rte_pktmbuf_append(mbuf, ctx->cfg.packet_size);
        if (payload == NULL) {
            rte_pktmbuf_free(mbuf);
            continue;
        }

        memset(payload, 0, ctx->cfg.packet_size);
        if (ctx->cfg.packet_size >= (sizeof(uint32_t) * 3U)) {
            uint32_t *meta = (uint32_t *)payload;
            meta[0] = flow->fid;
            meta[1] = flow->dest;
            meta[2] = flow->sent_count;
        }

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
