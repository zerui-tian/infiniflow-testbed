#ifndef INFINIFLOW_RECEIVER_CTX_H
#define INFINIFLOW_RECEIVER_CTX_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <rte_ether.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_spinlock.h>

#include "core/fc_mode.h"

#define RECEIVER_RESOURCE_NAME_LEN 64U

typedef struct receiver_feedback_msg_s {
    uint32_t vc_id;
    uint64_t fccl;
    uint64_t vc_dr;
    uint64_t vc_bklg;
    uint32_t flags;
    struct rte_ether_addr dst_addr;
    struct receiver_feedback_msg_s *pending_next;
} receiver_feedback_msg_t;

typedef struct receiver_config_s {
    uint16_t port_id;
    uint16_t rx_queue_id;
    uint16_t tx_queue_id;
    uint32_t rx_burst_size;
    uint32_t tx_burst_size;
    uint32_t nb_vc;
    uint32_t packet_size;
    uint32_t mempool_size;
    uint32_t feedback_ring_size;
    uint64_t cbfc_total_buffer_pkts;
    uint64_t qmin;
    uint64_t qmax;
    uint64_t initial_threshold;
    uint64_t port_buffer_pkts;
    fc_mode_t fc_mode;
    const char *output_path;
} receiver_config_t;

typedef struct receiver_vc_cbfc_state_s {
    uint64_t buffer_cap;
    uint64_t received;
} receiver_vc_cbfc_state_t;

typedef struct receiver_vc_infiniflow_state_s {
    uint64_t received;
    uint64_t drained;
    uint64_t backlog;
    uint32_t state;
    uint32_t pending_feedback_flags;
    rte_spinlock_t pending_feedback_lock;
    receiver_feedback_msg_t *pending_feedback_head;
    receiver_feedback_msg_t *pending_feedback_tail;
} receiver_vc_infiniflow_state_t;

typedef struct receiver_port_infiniflow_state_s {
    uint64_t total_received;
    uint64_t total_drained;
} receiver_port_infiniflow_state_t;

typedef struct flow_record_s {
    bool used;
    uint32_t flow_id;
    uint64_t pkt_count;
    uint64_t byte_count;
    uint64_t first_ts_cycles;
    uint64_t last_ts_cycles;
    uint64_t *timestamps;
    uint32_t ts_count;
    uint32_t ts_cap;
} flow_record_t;

typedef struct receiver_ctx_s {
    receiver_config_t cfg;
    struct rte_ether_addr port_mac;
    char mempool_name[RECEIVER_RESOURCE_NAME_LEN];
    char feedback_ring_name[RECEIVER_RESOURCE_NAME_LEN];
    char feedback_free_ring_name[RECEIVER_RESOURCE_NAME_LEN];
    struct rte_mempool *mbuf_pool;
    uint64_t rx_fc_data_pkts;
    uint64_t tx_cbfc_feedback_pkts;
    uint64_t feedback_enqueue_drop;
    flow_record_t *records;
    uint32_t records_cap;
    uint32_t records_used;
    uint64_t hz;
    uint64_t start_cycles;
    receiver_vc_cbfc_state_t *vc_cbfc_states;
    receiver_vc_infiniflow_state_t *vc_infiniflow_states;
    receiver_port_infiniflow_state_t port_infiniflow_state;
    struct rte_ring *feedback_ring;
    struct rte_ring *feedback_free_ring;
    receiver_feedback_msg_t *feedback_pool;
} receiver_ctx_t;

int receiver_feedback_init(receiver_ctx_t *ctx);
void receiver_feedback_cleanup(receiver_ctx_t *ctx);
void receiver_feedback_note_ta(receiver_ctx_t *ctx, uint32_t vc_id);
void receiver_feedback_try_enqueue(receiver_ctx_t *ctx, const struct rte_ether_addr *dst_addr,
                                   uint32_t vc_id, uint64_t fccl, uint64_t vc_dr,
                                   uint64_t vc_bklg, uint32_t flags);
uint32_t receiver_feedback_tx_run_tick(receiver_ctx_t *ctx);

#endif
