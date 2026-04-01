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
} switch_config_t;

typedef struct switch_ctx_s {
    switch_config_t cfg;
    struct rte_ether_addr egress_mac;

    vc_queue_t *vc_queues;
    struct rte_ring **feedback_queues;
    struct rte_ring **feedback_free_queues;
    switch_vc_fc_state_t *vc_states;
    switch_feedback_msg_t *feedback_pool;

    struct rte_mempool *mbuf_pool;

    const switch_fc_ops_t *fc_ops;

    uint64_t ingress_rx_pkts[SWITCH_MAX_INGRESS_PORTS];
    uint64_t ingress_enqueued_pkts[SWITCH_MAX_INGRESS_PORTS];
    uint64_t ingress_drop_pkts[SWITCH_MAX_INGRESS_PORTS];
    uint64_t egress_tx_ok_pkts;
    uint64_t egress_tx_drop_pkts;
} switch_ctx_t;

int switch_scheduler_run_tick(switch_ctx_t *ctx, uint16_t ingress_idx);
uint32_t switch_forward_run_tick(switch_ctx_t *ctx);
uint32_t switch_feedback_gen_run_tick(switch_ctx_t *ctx, uint16_t ingress_idx);
uint32_t switch_feedback_handler_run_tick(switch_ctx_t *ctx);

uint16_t switch_flow_map_lookup(const switch_ctx_t *ctx, uint32_t flow_id);

void switch_cbfc_ops_init(switch_ctx_t *ctx);

#endif
