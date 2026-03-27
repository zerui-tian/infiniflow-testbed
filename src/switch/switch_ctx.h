#ifndef INFINIFLOW_SWITCH_CTX_H
#define INFINIFLOW_SWITCH_CTX_H

#include <stdbool.h>
#include <stdint.h>

#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>

#include "core/fc_mode.h"
#include "core/vc_ring.h"

#define SWITCH_MAX_INGRESS_PORTS 8U

typedef struct switch_vc_fc_state_s {
    uint64_t fccl;
    uint64_t fctbs;
    uint64_t occupancy;
    uint64_t capacity;
} switch_vc_fc_state_t;

typedef struct switch_feedback_msg_s {
    uint32_t vc_id;
    uint64_t fccl;
    struct rte_ether_addr dst_addr;
} switch_feedback_msg_t;

typedef struct switch_flow_map_entry_s {
    bool used;
    uint32_t flow_id;
    uint16_t egress_port;
} switch_flow_map_entry_t;

struct switch_ctx_s;
typedef struct switch_fc_ops_s {
    uint64_t (*calc_credit)(const struct switch_ctx_s *ctx, uint32_t vc_id);
    void (*on_feedback_rx)(struct switch_ctx_s *ctx, uint32_t vc_id, uint64_t fccl);
    uint64_t (*on_tx_success)(struct switch_ctx_s *ctx, uint32_t vc_id);
} switch_fc_ops_t;

typedef struct switch_config_s {
    uint16_t ingress_ports[SWITCH_MAX_INGRESS_PORTS];
    uint16_t nb_ingress_ports;
    uint16_t egress_port;
    uint16_t ingress_rx_queue_id;
    uint16_t ingress_tx_queue_id;
    uint16_t egress_rx_queue_id;
    uint16_t egress_tx_queue_id;
    uint32_t nb_vc;
    uint32_t vc_ring_size;
    uint32_t feedback_ring_size;
    uint32_t mempool_size;
    uint32_t packet_size;
    uint32_t tx_burst_size;
    uint32_t rx_burst_size;
    uint64_t initial_fccl;
    uint64_t vc_capacity_pkts;
    fc_mode_t fc_mode;
    uint32_t flow_map_capacity;
    uint16_t default_egress_port;
    const char *flow_map_spec;
} switch_config_t;

typedef struct switch_ctx_s {
    switch_config_t cfg;

    vc_queue_t *vc_queues;
    struct rte_ring **feedback_queues;
    struct rte_ring **feedback_free_queues;
    switch_vc_fc_state_t *vc_states;
    switch_feedback_msg_t *feedback_pool;

    switch_flow_map_entry_t *flow_map_entries;
    uint32_t flow_map_mask;

    struct rte_mempool *mbuf_pool;

    const switch_fc_ops_t *fc_ops;

    uint64_t total_data_rx;
    uint64_t total_feedback_rx;
    uint64_t total_enqueued;
    uint64_t total_tx_ok;
    uint64_t total_tx_drop;
    uint64_t total_feedback_generated;
    uint64_t total_feedback_sent;
} switch_ctx_t;

int switch_scheduler_run_tick(switch_ctx_t *ctx, uint16_t ingress_idx);
uint32_t switch_forward_run_tick(switch_ctx_t *ctx);
uint32_t switch_feedback_gen_run_tick(switch_ctx_t *ctx, uint16_t ingress_idx);
uint32_t switch_feedback_handler_run_tick(switch_ctx_t *ctx);

int switch_flow_map_init(switch_ctx_t *ctx);
void switch_flow_map_free(switch_ctx_t *ctx);
int switch_flow_map_insert(switch_ctx_t *ctx, uint32_t flow_id, uint16_t egress_port);
uint16_t switch_flow_map_lookup(const switch_ctx_t *ctx, uint32_t flow_id);

void switch_cbfc_ops_init(switch_ctx_t *ctx);

#endif
