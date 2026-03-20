#ifndef INFINIFLOW_SENDER_CTX_H
#define INFINIFLOW_SENDER_CTX_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "core/flow.h"
#include "core/vc_ring.h"

typedef struct sender_config_s {
    const char *csv_path;
    uint16_t port_id;
    uint16_t tx_queue_id;
    uint32_t nb_vc;
    uint32_t ring_size;
    uint32_t packet_size;
    uint32_t mempool_size;
    uint32_t tx_burst_size;
    uint32_t tick_us;
} sender_config_t;

typedef struct sender_ctx_s {
    sender_config_t cfg;

    flow_desc_t *flows;
    uint32_t nb_flows;

    uint32_t *pending_order;
    uint32_t pending_pos;

    uint32_t *active_ids;
    uint32_t nb_active;

    vc_queue_t *vc_queues;

    struct rte_mempool *mbuf_pool;

    uint64_t start_cycles;
    uint64_t hz;

    uint64_t total_pkts_enqueued;
    uint64_t total_pkts_tx;
} sender_ctx_t;

int csv_loader_load(const char *path, flow_desc_t **flows_out, uint32_t *nb_flows_out,
                    uint32_t *max_vc_out);

int activity_manager_init(sender_ctx_t *ctx);
void activity_manager_activate_ready(sender_ctx_t *ctx, double now_sec);
void activity_manager_compact(sender_ctx_t *ctx);
bool activity_manager_all_done(const sender_ctx_t *ctx);

int scheduler_run_tick(sender_ctx_t *ctx);
uint32_t forward_run_tick(sender_ctx_t *ctx);

double sender_now_sec(const sender_ctx_t *ctx);

#endif
