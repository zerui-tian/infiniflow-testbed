#ifndef INFINIFLOW_SWITCH_CTX_H
#define INFINIFLOW_SWITCH_CTX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>

#include "core/fc_header.h"
#include "core/fc_mode.h"
#include "core/vc_ring.h"

#define SWITCH_MAX_INGRESS_PORTS 8U
#define SWITCH_MAX_EGRESS_PORTS 8U
#define SWITCH_MAX_ROUTE_CSV_PATH 512U

typedef struct switch_ingress_vc_state_s {
    uint64_t occupancy;
    uint64_t capacity;
    uint64_t total_received;
    uint64_t total_drained;
    uint32_t state;
    uint32_t pending_feedback_flags;
} switch_ingress_vc_state_t;

typedef struct switch_egress_vc_state_s {
    uint64_t fccl;
    uint64_t fctbs;
    uint64_t tx_pkts;
    uint64_t vc_dr;
    uint64_t vc_bklg;
    uint64_t threshold;
    uint32_t state;
} switch_egress_vc_state_t;

typedef struct switch_ingress_port_state_s {
    uint64_t occupancy;
    uint64_t total_received;
    uint64_t total_drained;
} switch_ingress_port_state_t;

typedef struct switch_egress_port_state_s {
    uint64_t fccl;
    uint64_t fctbs;
} switch_egress_port_state_t;

typedef struct switch_feedback_msg_s {
    uint32_t vc_id;
    uint64_t fccl;
    uint64_t vc_dr;
    uint64_t vc_bklg;
    uint32_t flags;
    struct rte_ether_addr dst_addr;
} switch_feedback_msg_t;

typedef struct switch_vc_stats_s {
    uint64_t rx_pkts;
    uint64_t tx_ok_pkts;
    uint64_t drop_capacity_pkts;
    uint64_t drop_other_pkts;
} switch_vc_stats_t;

typedef struct switch_feedback_stats_s {
    uint64_t enqueue_ok_pkts;
    uint64_t drop_no_free_pkts;
    uint64_t drop_queue_full_pkts;
    uint64_t tx_ok_pkts;
    uint64_t tx_retry_pkts;
} switch_feedback_stats_t;

typedef struct switch_route_table_s {
    uint16_t *ports;
    uint32_t nb_entries;
} switch_route_table_t;

struct switch_ctx_s;
typedef struct switch_fc_ops_s {
    uint32_t (*calc_deq_limit)(const struct switch_ctx_s *ctx, uint16_t egress_idx, uint32_t vc_id,
                               uint32_t burst_size);
    void (*on_feedback_rx)(struct switch_ctx_s *ctx, uint16_t egress_idx, uint32_t vc_id, uint64_t fccl,
                           uint64_t vc_dr, uint64_t vc_bklg, uint32_t flags);
    void (*on_tx_prepare)(struct switch_ctx_s *ctx, uint16_t egress_idx, uint32_t vc_id,
                          fc_data_header_t *fc_hdr);
    void (*on_tx_success)(struct switch_ctx_s *ctx, uint16_t egress_idx, uint32_t vc_id,
                          uint32_t pkt_count, bool ta_sent);
} switch_fc_ops_t;

typedef struct switch_config_s {
    uint16_t ingress_ports[SWITCH_MAX_INGRESS_PORTS];
    uint16_t nb_ingress_ports;
    uint16_t egress_ports[SWITCH_MAX_EGRESS_PORTS];
    uint16_t nb_egress_ports;
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
    uint64_t qmin;
    uint64_t qmax;
    uint64_t initial_threshold;
    uint64_t port_buffer_pkts;
    fc_mode_t fc_mode;
    char route_csv_path[SWITCH_MAX_ROUTE_CSV_PATH];
} switch_config_t;

typedef struct switch_ctx_s {
    switch_config_t cfg;
    struct rte_ether_addr egress_macs[SWITCH_MAX_EGRESS_PORTS];
    switch_route_table_t route_table;

    vc_queue_t *egress_vc_queues;
    struct rte_ring **feedback_queues;
    struct rte_ring **feedback_free_queues;
    switch_ingress_port_state_t *ingress_port_states;
    switch_ingress_vc_state_t *ingress_vc_states;
    switch_egress_port_state_t *egress_port_states;
    switch_egress_vc_state_t *egress_vc_states;
    switch_feedback_msg_t *feedback_pool;

    struct rte_mempool *mbuf_pool;

    const switch_fc_ops_t *fc_ops;

    switch_vc_stats_t *vc_stats;
    switch_feedback_stats_t *feedback_stats;
} switch_ctx_t;

int switch_scheduler_run_tick(switch_ctx_t *ctx, uint16_t ingress_idx);
uint32_t switch_forward_run_tick(switch_ctx_t *ctx, uint16_t egress_idx);
uint32_t switch_feedback_gen_run_tick(switch_ctx_t *ctx, uint16_t ingress_idx);
uint32_t switch_feedback_handler_run_tick(switch_ctx_t *ctx);

int switch_route_table_load(switch_ctx_t *ctx);
void switch_route_table_reset(switch_ctx_t *ctx);
uint16_t switch_flow_map_lookup(const switch_ctx_t *ctx, uint32_t flow_id);
uint16_t switch_egress_index_from_port(const switch_ctx_t *ctx, uint16_t port_id);
uint16_t switch_ingress_index_from_port(const switch_ctx_t *ctx, uint16_t port_id);

void switch_cbfc_ops_init(switch_ctx_t *ctx);
void switch_infiniflow_ops_init(switch_ctx_t *ctx);

static inline size_t switch_ingress_vc_state_index(const switch_ctx_t *ctx, uint16_t ingress_idx,
                                                   uint32_t vc_id) {
    return (size_t)ingress_idx * ctx->cfg.nb_vc + vc_id;
}

static inline size_t switch_egress_vc_state_index(const switch_ctx_t *ctx, uint16_t egress_idx,
                                                  uint32_t vc_id) {
    return (size_t)egress_idx * ctx->cfg.nb_vc + vc_id;
}

#endif
