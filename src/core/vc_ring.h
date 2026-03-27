#ifndef INFINIFLOW_CORE_VC_RING_H
#define INFINIFLOW_CORE_VC_RING_H

#include <stdint.h>

struct rte_ring;

typedef struct vc_queue_s {
    uint32_t vc_id;
    struct rte_ring *ring;
} vc_queue_t;

#endif
