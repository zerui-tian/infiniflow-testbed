#include "switch/switch_ctx.h"
#include "core/fc_header.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

#define DEFAULT_INGRESS_PORTS "0,1"
#define DEFAULT_EGRESS_PORTS "2"
#define DEFAULT_NB_VC 8U
#define DEFAULT_VC_RING_SIZE 1024U
#define DEFAULT_FEEDBACK_RING_SIZE 1024U
#define DEFAULT_PKT_SIZE 8000U
#define DEFAULT_MEMPOOL_SIZE 32768U
#define DEFAULT_TX_BURST 64U
#define DEFAULT_RX_BURST 64U
#define DEFAULT_INITIAL_FCCL 1024U
#define DEFAULT_VC_CAPACITY 1024U
#define DEFAULT_INFINIFLOW_QMIN 1U
#define DEFAULT_INFINIFLOW_QMAX 1U
#define DEFAULT_INFINIFLOW_INITIAL_THRESHOLD 1U
#define DEFAULT_INFINIFLOW_PORT_BUFFER_PKTS 1U
#define MBUF_CACHE_SIZE 256U

static volatile sig_atomic_t g_force_quit = 0;

typedef struct ingress_worker_arg_s {
    switch_ctx_t *ctx;
    uint16_t ingress_idx;
} ingress_worker_arg_t;

typedef struct egress_worker_arg_s {
    switch_ctx_t *ctx;
    uint16_t egress_idx;
} egress_worker_arg_t;

static void handle_signal(int signum) {
    (void)signum;
    g_force_quit = 1;
}

static void sync_user1_log_level_with_global(void) {
    uint32_t level = rte_log_get_global_level();

    if (rte_log_set_level(RTE_LOGTYPE_USER1, level) < 0) {
        fprintf(stderr, "failed to sync USER1 log level to global level=%" PRIu32 "\n", level);
    }
}

static int parse_u16(const char *s, uint16_t *out) {
    unsigned long v = 0;
    char *end = NULL;

    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > UINT16_MAX) {
        return -1;
    }

    *out = (uint16_t)v;
    return 0;
}

static int parse_u32(const char *s, uint32_t *out) {
    unsigned long v = 0;
    char *end = NULL;

    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > UINT32_MAX) {
        return -1;
    }

    *out = (uint32_t)v;
    return 0;
}

static int parse_u64(const char *s, uint64_t *out) {
    unsigned long long v = 0;
    char *end = NULL;

    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return -1;
    }

    *out = (uint64_t)v;
    return 0;
}

static int parse_fc_mode(const char *s, fc_mode_t *mode) {
    if (strcmp(s, "none") == 0) {
        *mode = FC_MODE_NONE;
        return 0;
    }
    if (strcmp(s, "cbfc") == 0) {
        *mode = FC_MODE_CBFC;
        return 0;
    }
    if (strcmp(s, "infiniflow") == 0) {
        *mode = FC_MODE_INFINIFLOW;
        return 0;
    }
    return -1;
}

static int copy_string_field(const char *src, char *dst, size_t dst_len) {
    size_t src_len = 0;

    if (src == NULL || dst == NULL || dst_len == 0U) {
        return -1;
    }

    src_len = strlen(src);
    if (src_len >= dst_len) {
        return -1;
    }

    memcpy(dst, src, src_len + 1U);
    return 0;
}

static int parse_port_list(const char *spec, uint16_t *ports, uint16_t max_ports, uint16_t *count_out) {
    char *dup = NULL;
    char *saveptr = NULL;
    char *token = NULL;
    uint16_t count = 0;

    dup = strdup(spec);
    if (dup == NULL) {
        return -1;
    }

    token = strtok_r(dup, ",", &saveptr);
    while (token != NULL) {
        uint16_t port_id = 0;
        uint16_t i = 0;

        if (count >= max_ports) {
            free(dup);
            return -1;
        }
        if (parse_u16(token, &port_id) != 0) {
            free(dup);
            return -1;
        }
        for (i = 0; i < count; i++) {
            if (ports[i] == port_id) {
                free(dup);
                return -1;
            }
        }
        ports[count++] = port_id;
        token = strtok_r(NULL, ",", &saveptr);
    }

    free(dup);
    *count_out = count;
    return count == 0U ? -1 : 0;
}

static int parse_ingress_ports(const char *spec, switch_config_t *cfg) {
    return parse_port_list(spec, cfg->ingress_ports, SWITCH_MAX_INGRESS_PORTS, &cfg->nb_ingress_ports);
}

static int parse_egress_ports(const char *spec, switch_config_t *cfg) {
    return parse_port_list(spec, cfg->egress_ports, SWITCH_MAX_EGRESS_PORTS, &cfg->nb_egress_ports);
}

static void usage(const char *prog) {
    printf("Usage: %s [EAL args] -- [options]\n", prog);
    printf("Options:\n");
    printf("  --ingress-ports <a,b>   Ingress port list (default: %s)\n", DEFAULT_INGRESS_PORTS);
    printf("  --egress-ports <a,b>    Egress port list (default: %s)\n", DEFAULT_EGRESS_PORTS);
    printf("  --vcs <num>             Number of VCs per ingress/egress (default: %u)\n", DEFAULT_NB_VC);
    printf("  --vc-ring-size <num>    VC ring size (default: %u)\n", DEFAULT_VC_RING_SIZE);
    printf("  --feedback-ring-size <num> Per-ingress feedback ring size (default: %u)\n",
           DEFAULT_FEEDBACK_RING_SIZE);
    printf("  --pkt-size <bytes>      Packet size in bytes (default: %u)\n", DEFAULT_PKT_SIZE);
    printf("  --mempool <num>         Mempool object count (default: %u)\n", DEFAULT_MEMPOOL_SIZE);
    printf("  --tx-burst <num>        TX burst size (default: %u)\n", DEFAULT_TX_BURST);
    printf("  --rx-burst <num>        RX burst size (default: %u)\n", DEFAULT_RX_BURST);
    printf("  --fc-mode <mode>        Flow control mode: none|cbfc|infiniflow (default: cbfc)\n");
    printf("  --initial-fccl <num>    Initial FCCL per VC (default: %u)\n", DEFAULT_INITIAL_FCCL);
    printf("  --vc-capacity <num>     Fixed VC capacity in packets (default: %u)\n",
           DEFAULT_VC_CAPACITY);
    printf("  --qmin <num>            InfiniFlow qmin (default: 1)\n");
    printf("  --qmax <num>            InfiniFlow qmax (default: 1)\n");
    printf("  --initial-threshold <num> InfiniFlow initial threshold (default: 1)\n");
    printf("  --port-buffer-pkts <num>  InfiniFlow shared ingress buffer packets (default: 1)\n");
    printf("  --route-csv <path>      Route CSV with fid,port columns (required)\n");
}

static int parse_app_args(int argc, char **argv, switch_config_t *cfg) {
    static const struct option long_opts[] = {
        {"ingress-ports", required_argument, 0, 'i'},
        {"egress-ports", required_argument, 0, 'e'},
        {"egress-port", required_argument, 0, 'o'},
        {"vcs", required_argument, 0, 'v'},
        {"vc-ring-size", required_argument, 0, 'r'},
        {"feedback-ring-size", required_argument, 0, 'q'},
        {"pkt-size", required_argument, 0, 's'},
        {"mempool", required_argument, 0, 'm'},
        {"tx-burst", required_argument, 0, 'b'},
        {"rx-burst", required_argument, 0, 'x'},
        {"fc-mode", required_argument, 0, 'f'},
        {"initial-fccl", required_argument, 0, 'c'},
        {"vc-capacity", required_argument, 0, 'k'},
        {"qmin", required_argument, 0, 'n'},
        {"qmax", required_argument, 0, 'N'},
        {"initial-threshold", required_argument, 0, 'T'},
        {"port-buffer-pkts", required_argument, 0, 'B'},
        {"route-csv", required_argument, 0, 'p'},
        {0, 0, 0, 0},
    };
    int opt = 0;

    while ((opt = getopt_long(argc, argv, "i:e:o:v:r:q:s:m:b:x:f:c:k:n:N:T:B:p:", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'i':
                if (parse_ingress_ports(optarg, cfg) != 0) {
                    return -1;
                }
                break;
            case 'e':
                if (parse_egress_ports(optarg, cfg) != 0) {
                    return -1;
                }
                break;
            case 'o':
                if (parse_egress_ports(optarg, cfg) != 0) {
                    return -1;
                }
                break;
            case 'v':
                if (parse_u32(optarg, &cfg->nb_vc) != 0) {
                    return -1;
                }
                break;
            case 'r':
                if (parse_u32(optarg, &cfg->vc_ring_size) != 0) {
                    return -1;
                }
                break;
            case 'q':
                if (parse_u32(optarg, &cfg->feedback_ring_size) != 0) {
                    return -1;
                }
                break;
            case 's':
                if (parse_u32(optarg, &cfg->packet_size) != 0) {
                    return -1;
                }
                break;
            case 'm':
                if (parse_u32(optarg, &cfg->mempool_size) != 0) {
                    return -1;
                }
                break;
            case 'b':
                if (parse_u32(optarg, &cfg->tx_burst_size) != 0) {
                    return -1;
                }
                break;
            case 'x':
                if (parse_u32(optarg, &cfg->rx_burst_size) != 0) {
                    return -1;
                }
                break;
            case 'f':
                if (parse_fc_mode(optarg, &cfg->fc_mode) != 0) {
                    return -1;
                }
                break;
            case 'c':
                if (parse_u64(optarg, &cfg->initial_fccl) != 0) {
                    return -1;
                }
                break;
            case 'k':
                if (parse_u64(optarg, &cfg->vc_capacity_pkts) != 0) {
                    return -1;
                }
                break;
            case 'n':
                if (parse_u64(optarg, &cfg->qmin) != 0) {
                    return -1;
                }
                break;
            case 'N':
                if (parse_u64(optarg, &cfg->qmax) != 0) {
                    return -1;
                }
                break;
            case 'T':
                if (parse_u64(optarg, &cfg->initial_threshold) != 0) {
                    return -1;
                }
                break;
            case 'B':
                if (parse_u64(optarg, &cfg->port_buffer_pkts) != 0) {
                    return -1;
                }
                break;
            case 'p':
                if (copy_string_field(optarg, cfg->route_csv_path, sizeof(cfg->route_csv_path)) != 0) {
                    return -1;
                }
                break;
            default:
                return -1;
        }
    }

    if (cfg->nb_ingress_ports == 0U || cfg->nb_vc == 0U || cfg->vc_ring_size == 0U ||
        cfg->feedback_ring_size == 0U || cfg->packet_size == 0U || cfg->mempool_size == 0U ||
        cfg->tx_burst_size == 0U || cfg->rx_burst_size == 0U || cfg->vc_capacity_pkts == 0U ||
        cfg->nb_egress_ports == 0U || cfg->route_csv_path[0] == '\0') {
        return -1;
    }

    return 0;
}

static int validate_roles(const switch_config_t *cfg) {
    uint16_t i = 0;
    uint16_t j = 0;

    for (i = 0; i < cfg->nb_ingress_ports; i++) {
        for (j = 0; j < cfg->nb_egress_ports; j++) {
            if (cfg->ingress_ports[i] == cfg->egress_ports[j]) {
                return -1;
            }
        }
    }
    return 0;
}

static int init_port(uint16_t port_id, uint16_t rx_queue_id, uint16_t tx_queue_id,
                     struct rte_mempool *mbuf_pool, uint32_t packet_size) {
    struct rte_eth_conf port_conf;
    struct rte_eth_dev_info dev_info;
    uint16_t nb_rxd = 1024;
    uint16_t nb_txd = 1024;
    uint16_t desired_mtu = 0;
    int rc = 0;

    memset(&port_conf, 0, sizeof(port_conf));
    port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

    rc = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
    if (rc < 0) {
        return rc;
    }

    rc = rte_eth_rx_queue_setup(port_id, rx_queue_id, nb_rxd, rte_eth_dev_socket_id(port_id), NULL,
                                mbuf_pool);
    if (rc < 0) {
        return rc;
    }

    rc = rte_eth_tx_queue_setup(port_id, tx_queue_id, nb_txd, rte_eth_dev_socket_id(port_id), NULL);
    if (rc < 0) {
        return rc;
    }

    if (packet_size > RTE_ETHER_MTU + RTE_ETHER_HDR_LEN) {
        desired_mtu = (uint16_t)(packet_size - RTE_ETHER_HDR_LEN);

        rc = rte_eth_dev_info_get(port_id, &dev_info);
        if (rc < 0) {
            return rc;
        }
        if (desired_mtu > dev_info.max_mtu) {
            return -1;
        }

        rc = rte_eth_dev_set_mtu(port_id, desired_mtu);
        if (rc < 0) {
            return rc;
        }
    }

    rc = rte_eth_dev_start(port_id);
    if (rc < 0) {
        return rc;
    }

    return 0;
}

static int init_egress_vc_rings(switch_ctx_t *ctx) {
    uint16_t egress_idx = 0;
    uint32_t vc_id = 0;
    size_t nb_queues = (size_t)ctx->cfg.nb_egress_ports * ctx->cfg.nb_vc;

    ctx->egress_vc_queues = calloc(nb_queues, sizeof(*ctx->egress_vc_queues));
    if (ctx->egress_vc_queues == NULL) {
        return -1;
    }

    for (egress_idx = 0; egress_idx < ctx->cfg.nb_egress_ports; egress_idx++) {
        for (vc_id = 0; vc_id < ctx->cfg.nb_vc; vc_id++) {
            char ring_name[64];
            size_t queue_idx = switch_egress_vc_state_index(ctx, egress_idx, vc_id);

            snprintf(ring_name, sizeof(ring_name), "switch_egress_%u_vc_%u", egress_idx, vc_id);
            ctx->egress_vc_queues[queue_idx].vc_id = vc_id;
            ctx->egress_vc_queues[queue_idx].ring =
                rte_ring_create(ring_name, ctx->cfg.vc_ring_size, rte_socket_id(), RING_F_SC_DEQ);
            if (ctx->egress_vc_queues[queue_idx].ring == NULL) {
                return -1;
            }
        }
    }

    return 0;
}

static int init_feedback_queues(switch_ctx_t *ctx) {
    uint16_t i = 0;
    uint32_t j = 0;
    unsigned int feedback_ring_flags = RING_F_SC_DEQ | RING_F_EXACT_SZ;
    unsigned int feedback_free_ring_flags = RING_F_SC_DEQ | RING_F_EXACT_SZ;

    ctx->feedback_queues = calloc(ctx->cfg.nb_ingress_ports, sizeof(*ctx->feedback_queues));
    ctx->feedback_free_queues =
        calloc(ctx->cfg.nb_ingress_ports, sizeof(*ctx->feedback_free_queues));
    if (ctx->feedback_queues == NULL || ctx->feedback_free_queues == NULL) {
        return -1;
    }

    ctx->feedback_pool =
        calloc((size_t)ctx->cfg.nb_ingress_ports * ctx->cfg.feedback_ring_size, sizeof(*ctx->feedback_pool));
    if (ctx->feedback_pool == NULL) {
        return -1;
    }

    for (i = 0; i < ctx->cfg.nb_ingress_ports; i++) {
        char ring_name[64];
        char free_ring_name[64];

        snprintf(ring_name, sizeof(ring_name), "switch_feedback_ring_%u", i);
        snprintf(free_ring_name, sizeof(free_ring_name), "switch_feedback_free_ring_%u", i);
        ctx->feedback_queues[i] = rte_ring_create(ring_name, ctx->cfg.feedback_ring_size,
                                                  rte_socket_id(), feedback_ring_flags);
        ctx->feedback_free_queues[i] = rte_ring_create(free_ring_name, ctx->cfg.feedback_ring_size,
                                                       rte_socket_id(), feedback_free_ring_flags);
        if (ctx->feedback_queues[i] == NULL || ctx->feedback_free_queues[i] == NULL) {
            return -1;
        }

        for (j = 0; j < ctx->cfg.feedback_ring_size; j++) {
            switch_feedback_msg_t *msg =
                &ctx->feedback_pool[(size_t)i * ctx->cfg.feedback_ring_size + j];
            if (rte_ring_mp_enqueue(ctx->feedback_free_queues[i], msg) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int init_ingress_vc_states(switch_ctx_t *ctx) {
    uint16_t ingress_idx = 0;
    uint32_t vc_id = 0;
    size_t nb_states = (size_t)ctx->cfg.nb_ingress_ports * ctx->cfg.nb_vc;

    ctx->ingress_port_states = calloc(ctx->cfg.nb_ingress_ports, sizeof(*ctx->ingress_port_states));
    ctx->ingress_vc_states = calloc(nb_states, sizeof(*ctx->ingress_vc_states));
    if (ctx->ingress_port_states == NULL || ctx->ingress_vc_states == NULL) {
        return -1;
    }

    for (ingress_idx = 0; ingress_idx < ctx->cfg.nb_ingress_ports; ingress_idx++) {
        ctx->ingress_port_states[ingress_idx].occupancy = 0U;
        ctx->ingress_port_states[ingress_idx].total_received = 0U;
        ctx->ingress_port_states[ingress_idx].total_drained = 0U;
        for (vc_id = 0; vc_id < ctx->cfg.nb_vc; vc_id++) {
            size_t state_idx = switch_ingress_vc_state_index(ctx, ingress_idx, vc_id);

            ctx->ingress_vc_states[state_idx].occupancy = 0U;
            ctx->ingress_vc_states[state_idx].capacity = ctx->cfg.vc_capacity_pkts;
            ctx->ingress_vc_states[state_idx].total_received = 0U;
            ctx->ingress_vc_states[state_idx].total_drained = 0U;
            ctx->ingress_vc_states[state_idx].state = 0U;
            ctx->ingress_vc_states[state_idx].pending_feedback_flags = 0U;
        }
    }

    return 0;
}

static int init_egress_vc_states(switch_ctx_t *ctx) {
    uint16_t egress_idx = 0;
    uint32_t vc_id = 0;
    size_t nb_states = (size_t)ctx->cfg.nb_egress_ports * ctx->cfg.nb_vc;

    ctx->egress_port_states = calloc(ctx->cfg.nb_egress_ports, sizeof(*ctx->egress_port_states));
    ctx->egress_vc_states = calloc(nb_states, sizeof(*ctx->egress_vc_states));
    if (ctx->egress_port_states == NULL || ctx->egress_vc_states == NULL) {
        return -1;
    }

    for (egress_idx = 0; egress_idx < ctx->cfg.nb_egress_ports; egress_idx++) {
        ctx->egress_port_states[egress_idx].fccl = ctx->cfg.initial_fccl;
        ctx->egress_port_states[egress_idx].fctbs = 0U;
        for (vc_id = 0; vc_id < ctx->cfg.nb_vc; vc_id++) {
            size_t state_idx = switch_egress_vc_state_index(ctx, egress_idx, vc_id);

            ctx->egress_vc_states[state_idx].fccl = ctx->cfg.initial_fccl;
            ctx->egress_vc_states[state_idx].fctbs = 0U;
            ctx->egress_vc_states[state_idx].tx_pkts = 0U;
            ctx->egress_vc_states[state_idx].vc_dr = 0U;
            ctx->egress_vc_states[state_idx].vc_bklg = 0U;
            ctx->egress_vc_states[state_idx].threshold = ctx->cfg.initial_threshold;
            ctx->egress_vc_states[state_idx].state = 0U;
        }
    }

    return 0;
}

static int init_vc_stats(switch_ctx_t *ctx) {
    size_t nb_stats = (size_t)ctx->cfg.nb_ingress_ports * ctx->cfg.nb_vc;

    ctx->vc_stats = calloc(nb_stats, sizeof(*ctx->vc_stats));
    if (ctx->vc_stats == NULL) {
        return -1;
    }

    return 0;
}

static int init_feedback_stats(switch_ctx_t *ctx) {
    ctx->feedback_stats = calloc(ctx->cfg.nb_ingress_ports, sizeof(*ctx->feedback_stats));
    if (ctx->feedback_stats == NULL) {
        return -1;
    }

    return 0;
}

static uint32_t cbfc_calc_deq_limit(const switch_ctx_t *ctx, uint16_t egress_idx, uint32_t vc_id,
                                    uint32_t burst_size) {
    const switch_egress_vc_state_t *state =
        &ctx->egress_vc_states[switch_egress_vc_state_index(ctx, egress_idx, vc_id)];
    uint64_t fccl = __atomic_load_n(&state->fccl, __ATOMIC_ACQUIRE);
    uint64_t fctbs = __atomic_load_n(&state->fctbs, __ATOMIC_RELAXED);
    uint64_t credit = (fccl > fctbs) ? (fccl - fctbs) : 0U;

    if (credit == 0U) {
        return 0U;
    }
    return (credit < burst_size) ? (uint32_t)credit : burst_size;
}

static void cbfc_on_feedback_rx(switch_ctx_t *ctx, uint16_t egress_idx, uint32_t vc_id, uint64_t fccl,
                                uint64_t vc_dr, uint64_t vc_bklg, uint32_t flags) {
    size_t state_idx = switch_egress_vc_state_index(ctx, egress_idx, vc_id);

    (void)vc_dr;
    (void)vc_bklg;
    (void)flags;
    __atomic_store_n(&ctx->egress_vc_states[state_idx].fccl, fccl, __ATOMIC_RELEASE);
}

static void cbfc_on_tx_prepare(switch_ctx_t *ctx, uint16_t egress_idx, uint32_t vc_id, fc_data_header_t *fc_hdr) {
    (void)ctx;
    (void)egress_idx;
    (void)vc_id;
    (void)fc_hdr;
}

static void cbfc_on_tx_success(switch_ctx_t *ctx, uint16_t egress_idx, uint32_t vc_id, uint32_t pkt_count,
                               bool ta_sent) {
    size_t egress_state_idx = switch_egress_vc_state_index(ctx, egress_idx, vc_id);
    switch_egress_vc_state_t *egress_state = &ctx->egress_vc_states[egress_state_idx];

    (void)ta_sent;
    __atomic_fetch_add(&egress_state->fctbs, pkt_count, __ATOMIC_RELAXED);
}

void switch_cbfc_ops_init(switch_ctx_t *ctx) {
    static const switch_fc_ops_t cbfc_ops = {
        .calc_deq_limit = cbfc_calc_deq_limit,
        .on_feedback_rx = cbfc_on_feedback_rx,
        .on_tx_prepare = cbfc_on_tx_prepare,
        .on_tx_success = cbfc_on_tx_success,
    };

    ctx->fc_ops = &cbfc_ops;
}

static uint32_t infiniflow_calc_deq_limit(const switch_ctx_t *ctx, uint16_t egress_idx, uint32_t vc_id,
                                          uint32_t burst_size) {
    const switch_egress_port_state_t *port_state = &ctx->egress_port_states[egress_idx];
    const switch_egress_vc_state_t *vc_state =
        &ctx->egress_vc_states[switch_egress_vc_state_index(ctx, egress_idx, vc_id)];
    uint64_t port_fccl = __atomic_load_n(&port_state->fccl, __ATOMIC_ACQUIRE);
    uint64_t port_fctbs = __atomic_load_n(&port_state->fctbs, __ATOMIC_RELAXED);
    uint64_t threshold = __atomic_load_n(&vc_state->threshold, __ATOMIC_ACQUIRE);
    uint64_t tx_pkts = __atomic_load_n(&vc_state->tx_pkts, __ATOMIC_RELAXED);
    uint64_t vc_dr = __atomic_load_n(&vc_state->vc_dr, __ATOMIC_RELAXED);
    uint64_t credit_pool = (port_fccl > port_fctbs) ? (port_fccl - port_fctbs) : 0U;
    uint64_t inflight = (tx_pkts > vc_dr) ? (tx_pkts - vc_dr) : 0U;
    uint64_t vc_credit = (threshold > inflight) ? (threshold - inflight) : 0U;
    uint32_t limit = burst_size;

    if (credit_pool == 0U || vc_credit == 0U) {
        return 0U;
    }
    if (credit_pool < (uint64_t)limit) {
        limit = (uint32_t)credit_pool;
    }
    if (vc_credit < (uint64_t)limit) {
        limit = (uint32_t)vc_credit;
    }
    return limit;
}

static void infiniflow_on_feedback_rx(switch_ctx_t *ctx, uint16_t egress_idx, uint32_t vc_id, uint64_t fccl,
                                      uint64_t vc_dr, uint64_t vc_bklg, uint32_t flags) {
    switch_egress_port_state_t *port_state = &ctx->egress_port_states[egress_idx];
    switch_egress_vc_state_t *vc_state =
        &ctx->egress_vc_states[switch_egress_vc_state_index(ctx, egress_idx, vc_id)];
    uint64_t tx_pkts = 0;
    uint64_t inflight = 0;
    uint64_t threshold = 0;
    uint64_t port_fctbs = 0;
    uint64_t limit = 1U;

    __atomic_store_n(&port_state->fccl, fccl, __ATOMIC_RELEASE);
    __atomic_store_n(&vc_state->vc_dr, vc_dr, __ATOMIC_RELEASE);
    __atomic_store_n(&vc_state->vc_bklg, vc_bklg, __ATOMIC_RELEASE);

    if ((flags & INFINIFLOW_FEEDBACK_FLAG_TA) != 0U &&
        __atomic_load_n(&vc_state->state, __ATOMIC_ACQUIRE) == 2U) {
        __atomic_store_n(&vc_state->state, 0U, __ATOMIC_RELEASE);
    }

    if (__atomic_load_n(&vc_state->state, __ATOMIC_ACQUIRE) != 0U) {
        return;
    }

    tx_pkts = __atomic_load_n(&vc_state->tx_pkts, __ATOMIC_RELAXED);
    threshold = __atomic_load_n(&vc_state->threshold, __ATOMIC_RELAXED);
    inflight = (tx_pkts > vc_dr) ? (tx_pkts - vc_dr) : 0U;
    port_fctbs = __atomic_load_n(&port_state->fctbs, __ATOMIC_RELAXED);

    if (vc_bklg >= ctx->cfg.qmax) {
        uint64_t reduction = vc_bklg - ctx->cfg.qmin;
        uint64_t new_threshold = (inflight > reduction) ? (inflight - reduction) : 1U;

        __atomic_store_n(&vc_state->threshold, new_threshold, __ATOMIC_RELEASE);
        __atomic_store_n(&vc_state->state, 1U, __ATOMIC_RELEASE);
    } else if (vc_bklg < ctx->cfg.qmin) {
        uint64_t headroom = (fccl > port_fctbs) ? (fccl - port_fctbs) : 1U;
        uint64_t increase = ctx->cfg.qmin - vc_bklg;
        uint64_t new_threshold = threshold + increase;

        limit = (headroom > 0U) ? headroom : 1U;
        if (new_threshold > limit) {
            new_threshold = limit;
        }
        if (new_threshold == 0U) {
            new_threshold = 1U;
        }
        __atomic_store_n(&vc_state->threshold, new_threshold, __ATOMIC_RELEASE);
        __atomic_store_n(&vc_state->state, 1U, __ATOMIC_RELEASE);
    }
}

static void infiniflow_on_tx_prepare(switch_ctx_t *ctx, uint16_t egress_idx, uint32_t vc_id,
                                     fc_data_header_t *fc_hdr) {
    switch_egress_vc_state_t *vc_state =
        &ctx->egress_vc_states[switch_egress_vc_state_index(ctx, egress_idx, vc_id)];
    uint32_t flags = fc_be32_to_cpu(fc_hdr->flags);

    if (__atomic_load_n(&vc_state->state, __ATOMIC_ACQUIRE) == 1U) {
        flags |= FC_DATA_FLAG_TA;
    }
    fc_hdr->flags = fc_cpu_to_be32(flags);
}

static void infiniflow_on_tx_success(switch_ctx_t *ctx, uint16_t egress_idx, uint32_t vc_id,
                                     uint32_t pkt_count, bool ta_sent) {
    switch_egress_port_state_t *port_state = &ctx->egress_port_states[egress_idx];
    switch_egress_vc_state_t *vc_state =
        &ctx->egress_vc_states[switch_egress_vc_state_index(ctx, egress_idx, vc_id)];

    __atomic_fetch_add(&port_state->fctbs, pkt_count, __ATOMIC_RELAXED);
    __atomic_fetch_add(&vc_state->tx_pkts, pkt_count, __ATOMIC_RELAXED);
    if (ta_sent) {
        __atomic_store_n(&vc_state->state, 2U, __ATOMIC_RELEASE);
    }
}

void switch_infiniflow_ops_init(switch_ctx_t *ctx) {
    static const switch_fc_ops_t infiniflow_ops = {
        .calc_deq_limit = infiniflow_calc_deq_limit,
        .on_feedback_rx = infiniflow_on_feedback_rx,
        .on_tx_prepare = infiniflow_on_tx_prepare,
        .on_tx_success = infiniflow_on_tx_success,
    };

    ctx->fc_ops = &infiniflow_ops;
}

static int scheduler_loop(void *arg) {
    ingress_worker_arg_t *worker = (ingress_worker_arg_t *)arg;

    while (!g_force_quit) {
        switch_scheduler_run_tick(worker->ctx, worker->ingress_idx);
    }

    return 0;
}

static int feedback_gen_loop(void *arg) {
    ingress_worker_arg_t *worker = (ingress_worker_arg_t *)arg;

    while (!g_force_quit) {
        switch_feedback_gen_run_tick(worker->ctx, worker->ingress_idx);
    }

    return 0;
}

static int forward_loop(void *arg) {
    egress_worker_arg_t *worker = (egress_worker_arg_t *)arg;

    while (!g_force_quit) {
        switch_forward_run_tick(worker->ctx, worker->egress_idx);
    }
    while (switch_forward_run_tick(worker->ctx, worker->egress_idx) > 0U) {
    }

    return 0;
}

static int feedback_handler_loop(void *arg) {
    switch_ctx_t *ctx = (switch_ctx_t *)arg;

    while (!g_force_quit) {
        switch_feedback_handler_run_tick(ctx);
    }

    return 0;
}

static void cleanup_switch(switch_ctx_t *ctx) {
    uint32_t i = 0;
    uint16_t ingress_idx = 0;
    struct rte_mbuf *mbuf = NULL;
    void *ptr = NULL;

    if (ctx->egress_vc_queues != NULL) {
        for (i = 0; i < (uint32_t)ctx->cfg.nb_egress_ports * ctx->cfg.nb_vc; i++) {
            if (ctx->egress_vc_queues[i].ring == NULL) {
                continue;
            }
            while (rte_ring_sc_dequeue(ctx->egress_vc_queues[i].ring, (void **)&mbuf) == 0) {
                rte_pktmbuf_free(mbuf);
            }
            rte_ring_free(ctx->egress_vc_queues[i].ring);
        }
    }

    if (ctx->feedback_queues != NULL) {
        for (ingress_idx = 0; ingress_idx < ctx->cfg.nb_ingress_ports; ingress_idx++) {
            if (ctx->feedback_queues[ingress_idx] != NULL) {
                while (rte_ring_sc_dequeue(ctx->feedback_queues[ingress_idx], &ptr) == 0) {
                }
                rte_ring_free(ctx->feedback_queues[ingress_idx]);
            }
            if (ctx->feedback_free_queues != NULL && ctx->feedback_free_queues[ingress_idx] != NULL) {
                while (rte_ring_sc_dequeue(ctx->feedback_free_queues[ingress_idx], &ptr) == 0) {
                }
                rte_ring_free(ctx->feedback_free_queues[ingress_idx]);
            }
        }
    }

    for (ingress_idx = 0; ingress_idx < ctx->cfg.nb_ingress_ports; ingress_idx++) {
        if (rte_eth_dev_is_valid_port(ctx->cfg.ingress_ports[ingress_idx])) {
            rte_eth_dev_stop(ctx->cfg.ingress_ports[ingress_idx]);
            rte_eth_dev_close(ctx->cfg.ingress_ports[ingress_idx]);
        }
    }
    for (ingress_idx = 0; ingress_idx < ctx->cfg.nb_egress_ports; ingress_idx++) {
        if (rte_eth_dev_is_valid_port(ctx->cfg.egress_ports[ingress_idx])) {
            rte_eth_dev_stop(ctx->cfg.egress_ports[ingress_idx]);
            rte_eth_dev_close(ctx->cfg.egress_ports[ingress_idx]);
        }
    }

    switch_route_table_reset(ctx);
    free(ctx->feedback_pool);
    free(ctx->feedback_stats);
    free(ctx->vc_stats);
    free(ctx->egress_vc_states);
    free(ctx->egress_port_states);
    free(ctx->ingress_vc_states);
    free(ctx->ingress_port_states);
    free(ctx->feedback_queues);
    free(ctx->feedback_free_queues);
    free(ctx->egress_vc_queues);
}

int main(int argc, char **argv) {
    switch_ctx_t ctx;
    ingress_worker_arg_t *ingress_worker_args = NULL;
    egress_worker_arg_t *egress_worker_args = NULL;
    unsigned int *scheduler_lcores = NULL;
    unsigned int *generator_lcores = NULL;
    unsigned int *egress_lcores = NULL;
    unsigned int handler_lcore = 0;
    uint32_t next_slot = 0;
    uint16_t ingress_idx = 0;
    uint16_t egress_idx = 0;
    unsigned int lcore_id = 0;
    uint32_t data_room_size = 0;
    int eal_argc = 0;
    int rc = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg.nb_vc = DEFAULT_NB_VC;
    ctx.cfg.vc_ring_size = DEFAULT_VC_RING_SIZE;
    ctx.cfg.feedback_ring_size = DEFAULT_FEEDBACK_RING_SIZE;
    ctx.cfg.packet_size = DEFAULT_PKT_SIZE;
    ctx.cfg.mempool_size = DEFAULT_MEMPOOL_SIZE;
    ctx.cfg.tx_burst_size = DEFAULT_TX_BURST;
    ctx.cfg.rx_burst_size = DEFAULT_RX_BURST;
    ctx.cfg.initial_fccl = DEFAULT_INITIAL_FCCL;
    ctx.cfg.vc_capacity_pkts = DEFAULT_VC_CAPACITY;
    ctx.cfg.qmin = DEFAULT_INFINIFLOW_QMIN;
    ctx.cfg.qmax = DEFAULT_INFINIFLOW_QMAX;
    ctx.cfg.initial_threshold = DEFAULT_INFINIFLOW_INITIAL_THRESHOLD;
    ctx.cfg.port_buffer_pkts = DEFAULT_INFINIFLOW_PORT_BUFFER_PKTS;
    ctx.cfg.fc_mode = FC_MODE_CBFC;
    ctx.cfg.ingress_rx_queue_id = 0;
    ctx.cfg.ingress_tx_queue_id = 0;
    ctx.cfg.egress_rx_queue_id = 0;
    ctx.cfg.egress_tx_queue_id = 0;
    if (parse_ingress_ports(DEFAULT_INGRESS_PORTS, &ctx.cfg) != 0) {
        rte_exit(EXIT_FAILURE, "default ingress ports parse failed\n");
    }
    if (parse_egress_ports(DEFAULT_EGRESS_PORTS, &ctx.cfg) != 0) {
        rte_exit(EXIT_FAILURE, "default egress ports parse failed\n");
    }

    eal_argc = rte_eal_init(argc, argv);
    if (eal_argc < 0) {
        rte_exit(EXIT_FAILURE, "EAL init failed\n");
    }
    sync_user1_log_level_with_global();

    argc -= eal_argc;
    argv += eal_argc;
    optind = 1;

    if (parse_app_args(argc, argv, &ctx.cfg) != 0) {
        usage("switch");
        rte_exit(EXIT_FAILURE, "Invalid app arguments\n");
    }
    if (validate_roles(&ctx.cfg) != 0) {
        rte_exit(EXIT_FAILURE, "egress ports cannot overlap ingress ports\n");
    }
    if (switch_route_table_load(&ctx) != 0) {
        rte_exit(EXIT_FAILURE, "route csv load failed\n");
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (ctx.cfg.packet_size < (RTE_ETHER_HDR_LEN + FC_DATA_HEADER_SIZE)) {
        rte_exit(EXIT_FAILURE, "Packet size too small for Ethernet+FC headers\n");
    }
    data_room_size = RTE_MAX(ctx.cfg.packet_size, (uint32_t)RTE_ETHER_MAX_LEN) + RTE_PKTMBUF_HEADROOM;
    if (data_room_size > UINT16_MAX) {
        rte_exit(EXIT_FAILURE, "Packet size too large for mbuf data room\n");
    }

    ctx.mbuf_pool = rte_pktmbuf_pool_create("switch_mbuf_pool", ctx.cfg.mempool_size, MBUF_CACHE_SIZE, 0,
                                            (uint16_t)data_room_size, rte_socket_id());
    if (ctx.mbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "mempool create failed\n");
    }

    for (ingress_idx = 0; ingress_idx < ctx.cfg.nb_ingress_ports; ingress_idx++) {
        rc = init_port(ctx.cfg.ingress_ports[ingress_idx], ctx.cfg.ingress_rx_queue_id,
                       ctx.cfg.ingress_tx_queue_id, ctx.mbuf_pool, ctx.cfg.packet_size);
        if (rc != 0) {
            rte_exit(EXIT_FAILURE, "ingress port init failed\n");
        }
        RTE_LOG(INFO, USER1, "switch: ingress port %" PRIu16 " started\n",
                ctx.cfg.ingress_ports[ingress_idx]);
    }
    for (ingress_idx = 0; ingress_idx < ctx.cfg.nb_egress_ports; ingress_idx++) {
        uint16_t egress_port = ctx.cfg.egress_ports[ingress_idx];

        rc = init_port(egress_port, ctx.cfg.egress_rx_queue_id, ctx.cfg.egress_tx_queue_id,
                       ctx.mbuf_pool, ctx.cfg.packet_size);
        if (rc != 0) {
            rte_exit(EXIT_FAILURE, "egress port init failed\n");
        }
        rte_eth_macaddr_get(egress_port, &ctx.egress_macs[ingress_idx]);
        RTE_LOG(INFO, USER1, "switch: egress port %" PRIu16 " started\n", egress_port);
    }

    if (init_egress_vc_rings(&ctx) != 0 || init_feedback_queues(&ctx) != 0 ||
        init_ingress_vc_states(&ctx) != 0 || init_egress_vc_states(&ctx) != 0 ||
        init_vc_stats(&ctx) != 0 || init_feedback_stats(&ctx) != 0) {
        rte_exit(EXIT_FAILURE, "switch queue/state init failed\n");
    }

    if (ctx.cfg.fc_mode == FC_MODE_CBFC) {
        switch_cbfc_ops_init(&ctx);
    } else if (ctx.cfg.fc_mode == FC_MODE_INFINIFLOW) {
        switch_infiniflow_ops_init(&ctx);
    }

    ingress_worker_args = calloc(ctx.cfg.nb_ingress_ports, sizeof(*ingress_worker_args));
    egress_worker_args = calloc(ctx.cfg.nb_egress_ports, sizeof(*egress_worker_args));
    scheduler_lcores = calloc(ctx.cfg.nb_ingress_ports, sizeof(*scheduler_lcores));
    generator_lcores = calloc(ctx.cfg.nb_ingress_ports, sizeof(*generator_lcores));
    egress_lcores = calloc(ctx.cfg.nb_egress_ports, sizeof(*egress_lcores));
    if (ingress_worker_args == NULL || egress_worker_args == NULL || scheduler_lcores == NULL ||
        generator_lcores == NULL || egress_lcores == NULL) {
        rte_exit(EXIT_FAILURE, "worker allocation failed\n");
    }

    for (ingress_idx = 0; ingress_idx < ctx.cfg.nb_ingress_ports; ingress_idx++) {
        ingress_worker_args[ingress_idx].ctx = &ctx;
        ingress_worker_args[ingress_idx].ingress_idx = ingress_idx;
    }
    for (egress_idx = 0; egress_idx < ctx.cfg.nb_egress_ports; egress_idx++) {
        egress_worker_args[egress_idx].ctx = &ctx;
        egress_worker_args[egress_idx].egress_idx = egress_idx;
    }

    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (next_slot < ctx.cfg.nb_ingress_ports) {
            scheduler_lcores[next_slot++] = lcore_id;
            continue;
        }
        if (next_slot < (uint32_t)ctx.cfg.nb_ingress_ports * 2U) {
            generator_lcores[next_slot++ - ctx.cfg.nb_ingress_ports] = lcore_id;
            continue;
        }
        if (next_slot <
            (uint32_t)ctx.cfg.nb_ingress_ports * 2U + (uint32_t)ctx.cfg.nb_egress_ports) {
            egress_lcores[next_slot++ - (uint32_t)ctx.cfg.nb_ingress_ports * 2U] = lcore_id;
            continue;
        }
        if (handler_lcore == 0U) {
            handler_lcore = lcore_id;
            break;
        }
    }

    for (ingress_idx = 0; ingress_idx < ctx.cfg.nb_ingress_ports; ingress_idx++) {
        if (scheduler_lcores[ingress_idx] == 0U || generator_lcores[ingress_idx] == 0U) {
            rte_exit(EXIT_FAILURE, "Need more worker lcores for scheduler/generator cores\n");
        }
    }
    for (egress_idx = 0; egress_idx < ctx.cfg.nb_egress_ports; egress_idx++) {
        if (egress_lcores[egress_idx] == 0U) {
            rte_exit(EXIT_FAILURE, "Need worker lcores for egress scheduler cores\n");
        }
    }
    if (handler_lcore == 0U) {
        rte_exit(EXIT_FAILURE, "Need worker lcore for feedback handler core\n");
    }

    for (ingress_idx = 0; ingress_idx < ctx.cfg.nb_ingress_ports; ingress_idx++) {
        rte_eal_remote_launch(scheduler_loop, &ingress_worker_args[ingress_idx],
                              scheduler_lcores[ingress_idx]);
        rte_eal_remote_launch(feedback_gen_loop, &ingress_worker_args[ingress_idx],
                              generator_lcores[ingress_idx]);
    }
    for (egress_idx = 0; egress_idx < ctx.cfg.nb_egress_ports; egress_idx++) {
        rte_eal_remote_launch(forward_loop, &egress_worker_args[egress_idx], egress_lcores[egress_idx]);
    }
    rte_eal_remote_launch(feedback_handler_loop, &ctx, handler_lcore);

    for (ingress_idx = 0; ingress_idx < ctx.cfg.nb_ingress_ports; ingress_idx++) {
        rte_eal_wait_lcore(scheduler_lcores[ingress_idx]);
        rte_eal_wait_lcore(generator_lcores[ingress_idx]);
    }
    for (egress_idx = 0; egress_idx < ctx.cfg.nb_egress_ports; egress_idx++) {
        rte_eal_wait_lcore(egress_lcores[egress_idx]);
    }
    rte_eal_wait_lcore(handler_lcore);

    printf("Switch completed.\n");

    for (ingress_idx = 0; ingress_idx < ctx.cfg.nb_ingress_ports; ingress_idx++) {
        uint16_t port_id = ctx.cfg.ingress_ports[ingress_idx];
        uint32_t vc_id = 0;
        const switch_feedback_stats_t *fb_stats = &ctx.feedback_stats[ingress_idx];

        for (vc_id = 0; vc_id < ctx.cfg.nb_vc; vc_id++) {
            size_t stats_idx = (size_t)ingress_idx * ctx.cfg.nb_vc + vc_id;
            const switch_vc_stats_t *stats = &ctx.vc_stats[stats_idx];

            printf(
                "switch ingress port %" PRIu16 " vc=%" PRIu32
                ": rx=%" PRIu64 " tx-ok=%" PRIu64 " drop-capacity=%" PRIu64 " drop-other=%" PRIu64 "\n",
                port_id, vc_id, stats->rx_pkts, stats->tx_ok_pkts, stats->drop_capacity_pkts,
                stats->drop_other_pkts);
        }

        printf("switch ingress port %" PRIu16
               " feedback: enqueue-ok=%" PRIu64 " drop-no-free=%" PRIu64
               " drop-queue-full=%" PRIu64 " tx-ok=%" PRIu64 " tx-retry=%" PRIu64
               " queued=%u free=%u\n",
               port_id,
               __atomic_load_n(&fb_stats->enqueue_ok_pkts, __ATOMIC_RELAXED),
               __atomic_load_n(&fb_stats->drop_no_free_pkts, __ATOMIC_RELAXED),
               __atomic_load_n(&fb_stats->drop_queue_full_pkts, __ATOMIC_RELAXED),
               __atomic_load_n(&fb_stats->tx_ok_pkts, __ATOMIC_RELAXED),
               __atomic_load_n(&fb_stats->tx_retry_pkts, __ATOMIC_RELAXED),
               rte_ring_count(ctx.feedback_queues[ingress_idx]),
               rte_ring_count(ctx.feedback_free_queues[ingress_idx]));
    }

    cleanup_switch(&ctx);
    free(ingress_worker_args);
    free(egress_worker_args);
    free(scheduler_lcores);
    free(generator_lcores);
    free(egress_lcores);
    return 0;
}
