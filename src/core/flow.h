#ifndef INFINIFLOW_CORE_FLOW_H
#define INFINIFLOW_CORE_FLOW_H

#include <stdint.h>

typedef enum flow_state_e {
    FLOW_STATE_PENDING = 0,
    FLOW_STATE_ACTIVE = 1,
    FLOW_STATE_DEAD = 2
} flow_state_t;

typedef struct flow_desc_s {
    uint32_t fid;
    uint32_t vc;
    uint32_t len;
    double stime_sec;
    uint32_t dest;
    uint32_t sent_count;
    flow_state_t state;
} flow_desc_t;

#endif
