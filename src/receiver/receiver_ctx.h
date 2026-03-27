#ifndef INFINIFLOW_RECEIVER_CTX_H
#define INFINIFLOW_RECEIVER_CTX_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <rte_mempool.h>

#include "core/fc_mode.h"

typedef struct receiver_config_s {
    uint16_t port_id;
    uint16_t rx_queue_id;
    uint16_t tx_queue_id;
    uint32_t rx_burst_size;
    uint32_t tx_burst_size;
    uint32_t nb_vc;
    uint32_t packet_size;
    uint32_t mempool_size;
    uint64_t cbfc_total_buffer_pkts;
    fc_mode_t fc_mode;
    const char *output_path;
} receiver_config_t;

typedef struct receiver_vc_cbfc_state_s {
    uint64_t buffer_cap;
    uint64_t received;
} receiver_vc_cbfc_state_t;

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
    struct rte_mempool *mbuf_pool;
    flow_record_t *records;
    uint32_t records_cap;
    uint32_t records_used;
    uint64_t hz;
    uint64_t start_cycles;
    receiver_vc_cbfc_state_t *vc_cbfc_states;
} receiver_ctx_t;

#endif
