#ifndef INFINIFLOW_CORE_FC_HEADER_H
#define INFINIFLOW_CORE_FC_HEADER_H

#include <rte_byteorder.h>
#include <stdint.h>

typedef struct fc_data_header_s {
    rte_be32_t flow_id;
    rte_be32_t vc_id;
    rte_be32_t flags;
} fc_data_header_t;

typedef struct cbfc_feedback_header_s {
    rte_be32_t vc_id;
    rte_be64_t fccl;
} cbfc_feedback_header_t;

typedef struct infiniflow_feedback_header_s {
    rte_be32_t vc_id;
    rte_be32_t flags;
    rte_be64_t vc_dr;
    rte_be64_t vc_bklg;
    rte_be64_t fccl;
} infiniflow_feedback_header_t;

#define FC_ETHER_TYPE 0x88B5U
#define CBFC_FEEDBACK_ETHER_TYPE 0x88B6U
#define INFINIFLOW_FEEDBACK_ETHER_TYPE 0x88B7U
#define FC_DATA_HEADER_SIZE sizeof(fc_data_header_t)
#define CBFC_FEEDBACK_HEADER_SIZE sizeof(cbfc_feedback_header_t)
#define INFINIFLOW_FEEDBACK_HEADER_SIZE sizeof(infiniflow_feedback_header_t)

#define FC_DATA_FLAG_TA 0x1U
#define INFINIFLOW_FEEDBACK_FLAG_TA 0x1U

static inline rte_be64_t fc_cpu_to_be64(uint64_t value) {
    return rte_cpu_to_be_64(value);
}

static inline uint64_t fc_be64_to_cpu(rte_be64_t value) {
    return rte_be_to_cpu_64(value);
}

static inline rte_be32_t fc_cpu_to_be32(uint32_t value) {
    return rte_cpu_to_be_32(value);
}

static inline uint32_t fc_be32_to_cpu(rte_be32_t value) {
    return rte_be_to_cpu_32(value);
}

#endif
