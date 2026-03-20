#ifndef INFINIFLOW_CORE_FC_HEADER_H
#define INFINIFLOW_CORE_FC_HEADER_H

#include <rte_byteorder.h>

typedef struct fc_header_s {
    rte_be32_t flow_id;
} fc_header_t;

#define FC_ETHER_TYPE 0x88B5U
#define FC_HEADER_SIZE sizeof(fc_header_t)

#endif
