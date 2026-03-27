#include "sender/sender_ctx.h"

#include <stdlib.h>

#include <rte_ring.h>

static sender_ctx_t *g_sort_ctx = NULL;

static int cmp_pending_flow(const void *lhs, const void *rhs) {
    const uint32_t id_l = *(const uint32_t *)lhs;
    const uint32_t id_r = *(const uint32_t *)rhs;
    const flow_desc_t *flow_l = &g_sort_ctx->flows[id_l];
    const flow_desc_t *flow_r = &g_sort_ctx->flows[id_r];

    if (flow_l->stime_sec < flow_r->stime_sec) {
        return -1;
    }
    if (flow_l->stime_sec > flow_r->stime_sec) {
        return 1;
    }
    if (flow_l->fid < flow_r->fid) {
        return -1;
    }
    if (flow_l->fid > flow_r->fid) {
        return 1;
    }
    return 0;
}

int activity_manager_init(sender_ctx_t *ctx) {
    uint32_t i = 0;

    if (ctx == NULL || ctx->flows == NULL || ctx->nb_flows == 0) {
        return -1;
    }

    ctx->pending_order = calloc(ctx->nb_flows, sizeof(*ctx->pending_order));
    ctx->active_ids = calloc(ctx->nb_flows, sizeof(*ctx->active_ids));
    if (ctx->pending_order == NULL || ctx->active_ids == NULL) {
        free(ctx->pending_order);
        free(ctx->active_ids);
        ctx->pending_order = NULL;
        ctx->active_ids = NULL;
        return -1;
    }

    for (i = 0; i < ctx->nb_flows; i++) {
        ctx->pending_order[i] = i;
    }

    g_sort_ctx = ctx;
    qsort(ctx->pending_order, ctx->nb_flows, sizeof(*ctx->pending_order), cmp_pending_flow);
    g_sort_ctx = NULL;

    ctx->pending_pos = 0;
    ctx->nb_active = 0;
    return 0;
}

void activity_manager_activate_ready(sender_ctx_t *ctx, double now_sec) {
    while (ctx->pending_pos < ctx->nb_flows) {
        uint32_t flow_id = ctx->pending_order[ctx->pending_pos];
        flow_desc_t *flow = &ctx->flows[flow_id];

        if (flow->state != FLOW_STATE_PENDING) {
            ctx->pending_pos++;
            continue;
        }
        if (flow->stime_sec > now_sec) {
            break;
        }

        flow->state = FLOW_STATE_ACTIVE;
        ctx->active_ids[ctx->nb_active] = flow_id;
        ctx->nb_active++;
        ctx->pending_pos++;
    }
}

void activity_manager_compact(sender_ctx_t *ctx) {
    uint32_t write_idx = 0;
    uint32_t i = 0;

    for (i = 0; i < ctx->nb_active; i++) {
        uint32_t flow_id = ctx->active_ids[i];
        if (ctx->flows[flow_id].state == FLOW_STATE_ACTIVE) {
            ctx->active_ids[write_idx] = flow_id;
            write_idx++;
        }
    }

    ctx->nb_active = write_idx;
}

bool activity_manager_all_done(const sender_ctx_t *ctx) {
    uint32_t i = 0;

    if (ctx->pending_pos < ctx->nb_flows) {
        return false;
    }
    if (ctx->nb_active != 0) {
        return false;
    }
    for (i = 0; i < ctx->cfg.nb_vc; i++) {
        if (ctx->vc_queues[i].ring != NULL && rte_ring_count(ctx->vc_queues[i].ring) != 0U) {
            return false;
        }
    }
    return true;
}
