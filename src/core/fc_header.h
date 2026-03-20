#ifndef INFINIFLOW_CORE_FC_HEADER_H
#define INFINIFLOW_CORE_FC_HEADER_H

#include <rte_byteorder.h>
#include <rte_ip.h>

typedef struct fc_header_s {
    rte_be32_t flow_id;
} fc_header_t;

#define FC_IPPROTO 253U
#define FC_HEADER_SIZE sizeof(fc_header_t)
#define FC_IPV4_SRC_ADDR RTE_IPV4(10, 0, 0, 1)

#endif
