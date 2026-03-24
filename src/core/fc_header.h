#ifndef INFINIFLOW_CORE_FC_HEADER_H
#define INFINIFLOW_CORE_FC_HEADER_H

#include <rte_byteorder.h>
#include <stdint.h>

typedef struct fc_data_header_s {
    rte_be32_t flow_id;
    rte_be32_t vc_id;
} fc_data_header_t;

typedef struct cbfc_feedback_header_s {
    rte_be32_t vc_id;
    rte_be64_t fccl;
} cbfc_feedback_header_t;

#define FC_ETHER_TYPE 0x88B5U
#define CBFC_FEEDBACK_ETHER_TYPE 0x88B6U
#define FC_DATA_HEADER_SIZE sizeof(fc_data_header_t)
#define CBFC_FEEDBACK_HEADER_SIZE sizeof(cbfc_feedback_header_t)

static inline rte_be64_t fc_cpu_to_be64(uint64_t value) {
    return rte_cpu_to_be_64(value);
}

static inline uint64_t fc_be64_to_cpu(rte_be64_t value) {
    return rte_be_to_cpu_64(value);
}

#endif
