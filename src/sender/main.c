#include "sender/sender_ctx.h"
#include "core/fc_header.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_pause.h>
#include <rte_ring.h>

#define DEFAULT_NB_VC 8U
#define DEFAULT_RING_SIZE 1024U
#define DEFAULT_PKT_SIZE 8000U
#define DEFAULT_MEMPOOL_SIZE 32768U
#define DEFAULT_TX_BURST 64U
#define DEFAULT_RX_BURST 64U
#define DEFAULT_TICK_US 1000U
#define DEFAULT_INITIAL_FCCL 1024U
#define DEFAULT_INFINIFLOW_QMIN 1U
#define DEFAULT_INFINIFLOW_QMAX 1U
#define DEFAULT_INFINIFLOW_INITIAL_THRESHOLD 1U
#define MBUF_CACHE_SIZE 256U

typedef struct sender_app_config_s {
    uint16_t *port_ids;
    uint16_t nb_ports;
    char **csv_paths;

    uint32_t nb_vc;
    uint32_t ring_size;
    uint32_t packet_size;
    uint32_t mempool_size;
    uint32_t tx_burst_size;
    uint32_t rx_burst_size;
    uint32_t tick_us;
    uint64_t initial_fccl;
    uint64_t qmin;
    uint64_t qmax;
    uint64_t initial_threshold;
    fc_mode_t fc_mode;
} sender_app_config_t;

typedef struct sender_app_ctx_s {
    sender_app_config_t cfg;
    sender_ctx_t *port_ctxs;
    uint8_t *producer_done;
    uint64_t global_start_cycles;
    uint32_t start_flag;
} sender_app_ctx_t;

typedef struct sender_worker_arg_s {
    sender_app_ctx_t *app;
    uint16_t port_idx;
} sender_worker_arg_t;

static volatile sig_atomic_t g_force_quit = 0;

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

double sender_now_sec(const sender_ctx_t *ctx) {
    uint64_t now = rte_get_timer_cycles();
    uint64_t delta = now - ctx->start_cycles;
    return (double)delta / (double)ctx->hz;
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

static char *trim_spaces(char *s) {
    char *end = NULL;

    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '\0') {
        return s;
    }
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return s;
}

static int parse_port_list(const char *spec, uint16_t **ports_out, uint16_t *nb_ports_out) {
    char *dup = NULL;
    char *saveptr = NULL;
    char *token = NULL;
    uint16_t *ports = NULL;
    uint16_t cap = 0;
    uint16_t count = 0;
    int rc = -1;

    if (spec == NULL || ports_out == NULL || nb_ports_out == NULL) {
        return -1;
    }

    dup = strdup(spec);
    if (dup == NULL) {
        return -1;
    }

    token = strtok_r(dup, ",", &saveptr);
    while (token != NULL) {
        uint16_t port_id = 0;
        uint16_t i = 0;
        char *trimmed = trim_spaces(token);

        if (parse_u16(trimmed, &port_id) != 0) {
            goto out;
        }
        for (i = 0; i < count; i++) {
            if (ports[i] == port_id) {
                goto out;
            }
        }
        if (count == cap) {
            uint16_t new_cap = (cap == 0U) ? 4U : (uint16_t)(cap * 2U);
            uint16_t *new_ports = realloc(ports, new_cap * sizeof(*ports));
            if (new_ports == NULL) {
                goto out;
            }
            ports = new_ports;
            cap = new_cap;
        }
        ports[count++] = port_id;
        token = strtok_r(NULL, ",", &saveptr);
    }

    if (count == 0U) {
        goto out;
    }

    *ports_out = ports;
    *nb_ports_out = count;
    ports = NULL;
    rc = 0;

out:
    free(ports);
    free(dup);
    return rc;
}

static int parse_csv_list(const char *spec, char ***csvs_out, uint16_t *nb_csvs_out) {
    char *dup = NULL;
    char *saveptr = NULL;
    char *token = NULL;
    char **csvs = NULL;
    uint16_t cap = 0;
    uint16_t count = 0;
    uint16_t i = 0;
    int rc = -1;

    if (spec == NULL || csvs_out == NULL || nb_csvs_out == NULL) {
        return -1;
    }

    dup = strdup(spec);
    if (dup == NULL) {
        return -1;
    }

    token = strtok_r(dup, ",", &saveptr);
    while (token != NULL) {
        char *trimmed = trim_spaces(token);

        if (*trimmed == '\0') {
            goto out;
        }
        if (count == cap) {
            uint16_t new_cap = (cap == 0U) ? 4U : (uint16_t)(cap * 2U);
            char **new_csvs = realloc(csvs, new_cap * sizeof(*csvs));
            if (new_csvs == NULL) {
                goto out;
            }
            csvs = new_csvs;
            cap = new_cap;
        }
        csvs[count] = strdup(trimmed);
        if (csvs[count] == NULL) {
            goto out;
        }
        count++;
        token = strtok_r(NULL, ",", &saveptr);
    }

    if (count == 0U) {
        goto out;
    }

    *csvs_out = csvs;
    *nb_csvs_out = count;
    csvs = NULL;
    rc = 0;

out:
    if (csvs != NULL) {
        for (i = 0; i < count; i++) {
            free(csvs[i]);
        }
    }
    free(csvs);
    free(dup);
    return rc;
}

static void free_csv_list(char **csvs, uint16_t nb_csvs) {
    uint16_t i = 0;
    if (csvs == NULL) {
        return;
    }
    for (i = 0; i < nb_csvs; i++) {
        free(csvs[i]);
    }
    free(csvs);
}

static void free_app_config(sender_app_config_t *cfg) {
    if (cfg == NULL) {
        return;
    }
    free(cfg->port_ids);
    free_csv_list(cfg->csv_paths, cfg->nb_ports);
    memset(cfg, 0, sizeof(*cfg));
}

static void usage(const char *prog) {
    printf("Usage: %s [EAL args] -- [options]\n", prog);
    printf("Options:\n");
    printf("  --csv <path>         Single flow CSV path (legacy mode)\n");
    printf("  --port <id>          Single NIC port id (legacy mode)\n");
    printf("  --csvs <p1,p2,...>   Per-port CSV list (required for multi-port)\n");
    printf("  --ports <a,b,...>    NIC port list for sender instances\n");
    printf("  --vcs <num>          Number of VC rings per port (default: 8)\n");
    printf("  --ring-size <num>    Ring size per VC (default: 1024)\n");
    printf("  --pkt-size <bytes>   Packet size in bytes (default: 8000)\n");
    printf("  --mempool <num>      Mempool object count per port (default: 32768)\n");
    printf("  --tx-burst <num>     TX burst size (default: 64)\n");
    printf("  --rx-burst <num>     RX burst size for feedback (default: 64)\n");
    printf("  --tick-us <num>      Producer tick interval us (default: 1000)\n");
    printf("  --fc-mode <mode>     Flow control mode: none|cbfc|infiniflow (default: none)\n");
    printf("  --initial-fccl <n>   Initial CBFC FCCL per VC (default: 1024)\n");
    printf("  --qmin <n>           InfiniFlow qmin (default: 1)\n");
    printf("  --qmax <n>           InfiniFlow qmax (default: 1)\n");
    printf("  --initial-threshold <n> InfiniFlow initial threshold per VC (default: 1)\n");
}

static int parse_app_args(int argc, char **argv, sender_app_config_t *cfg) {
    static const struct option long_opts[] = {
        {"csv", required_argument, 0, 'c'},
        {"port", required_argument, 0, 'p'},
        {"csvs", required_argument, 0, 'C'},
        {"ports", required_argument, 0, 'P'},
        {"vcs", required_argument, 0, 'v'},
        {"ring-size", required_argument, 0, 'r'},
        {"pkt-size", required_argument, 0, 's'},
        {"mempool", required_argument, 0, 'm'},
        {"tx-burst", required_argument, 0, 'b'},
        {"rx-burst", required_argument, 0, 'x'},
        {"tick-us", required_argument, 0, 't'},
        {"fc-mode", required_argument, 0, 'f'},
        {"initial-fccl", required_argument, 0, 'i'},
        {"qmin", required_argument, 0, 'q'},
        {"qmax", required_argument, 0, 'Q'},
        {"initial-threshold", required_argument, 0, 'T'},
        {0, 0, 0, 0},
    };
    uint16_t *port_list = NULL;
    uint16_t nb_port_list = 0;
    char **csv_list = NULL;
    uint16_t nb_csv_list = 0;
    char *single_csv = NULL;
    uint16_t single_port = 0;
    bool has_single_csv = false;
    bool has_single_port = false;
    bool has_csv_list = false;
    bool has_port_list = false;
    int opt = 0;

    if (cfg == NULL) {
        return -1;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->nb_vc = DEFAULT_NB_VC;
    cfg->ring_size = DEFAULT_RING_SIZE;
    cfg->packet_size = DEFAULT_PKT_SIZE;
    cfg->mempool_size = DEFAULT_MEMPOOL_SIZE;
    cfg->tx_burst_size = DEFAULT_TX_BURST;
    cfg->rx_burst_size = DEFAULT_RX_BURST;
    cfg->tick_us = DEFAULT_TICK_US;
    cfg->initial_fccl = DEFAULT_INITIAL_FCCL;
    cfg->qmin = DEFAULT_INFINIFLOW_QMIN;
    cfg->qmax = DEFAULT_INFINIFLOW_QMAX;
    cfg->initial_threshold = DEFAULT_INFINIFLOW_INITIAL_THRESHOLD;
    cfg->fc_mode = FC_MODE_NONE;

    while ((opt = getopt_long(argc, argv, "c:p:C:P:v:r:s:m:b:x:t:f:i:q:Q:T:", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'c':
                free(single_csv);
                single_csv = strdup(optarg);
                if (single_csv == NULL) {
                    return -1;
                }
                has_single_csv = true;
                break;
            case 'p':
                if (parse_u16(optarg, &single_port) != 0) {
                    free(single_csv);
                    return -1;
                }
                has_single_port = true;
                break;
            case 'C':
                if (has_csv_list) {
                    free(single_csv);
                    return -1;
                }
                if (parse_csv_list(optarg, &csv_list, &nb_csv_list) != 0) {
                    free(single_csv);
                    return -1;
                }
                has_csv_list = true;
                break;
            case 'P':
                if (has_port_list) {
                    free(single_csv);
                    return -1;
                }
                if (parse_port_list(optarg, &port_list, &nb_port_list) != 0) {
                    free(single_csv);
                    return -1;
                }
                has_port_list = true;
                break;
            case 'v':
                if (parse_u32(optarg, &cfg->nb_vc) != 0) {
                    free(single_csv);
                    return -1;
                }
                break;
            case 'r':
                if (parse_u32(optarg, &cfg->ring_size) != 0) {
                    free(single_csv);
                    return -1;
                }
                break;
            case 's':
                if (parse_u32(optarg, &cfg->packet_size) != 0) {
                    free(single_csv);
                    return -1;
                }
                break;
            case 'm':
                if (parse_u32(optarg, &cfg->mempool_size) != 0) {
                    free(single_csv);
                    return -1;
                }
                break;
            case 'b':
                if (parse_u32(optarg, &cfg->tx_burst_size) != 0) {
                    free(single_csv);
                    return -1;
                }
                break;
            case 'x':
                if (parse_u32(optarg, &cfg->rx_burst_size) != 0) {
                    free(single_csv);
                    return -1;
                }
                break;
            case 't':
                if (parse_u32(optarg, &cfg->tick_us) != 0) {
                    free(single_csv);
                    return -1;
                }
                break;
            case 'f':
                if (parse_fc_mode(optarg, &cfg->fc_mode) != 0) {
                    free(single_csv);
                    return -1;
                }
                break;
            case 'i':
                if (parse_u64(optarg, &cfg->initial_fccl) != 0) {
                    free(single_csv);
                    return -1;
                }
                break;
            case 'q':
                if (parse_u64(optarg, &cfg->qmin) != 0) {
                    free(single_csv);
                    return -1;
                }
                break;
            case 'Q':
                if (parse_u64(optarg, &cfg->qmax) != 0) {
                    free(single_csv);
                    return -1;
                }
                break;
            case 'T':
                if (parse_u64(optarg, &cfg->initial_threshold) != 0) {
                    free(single_csv);
                    return -1;
                }
                break;
            default:
                free(single_csv);
                return -1;
        }
    }

    if ((has_single_port && has_port_list) || (has_single_csv && has_csv_list)) {
        free(port_list);
        free_csv_list(csv_list, nb_csv_list);
        free(single_csv);
        return -1;
    }

    if (has_port_list && has_csv_list && nb_port_list != nb_csv_list) {
        free(port_list);
        free_csv_list(csv_list, nb_csv_list);
        free(single_csv);
        return -1;
    }

    if (has_port_list) {
        cfg->port_ids = port_list;
        cfg->nb_ports = nb_port_list;
        port_list = NULL;
    } else {
        cfg->nb_ports = has_csv_list ? nb_csv_list : 1U;
        cfg->port_ids = calloc(cfg->nb_ports, sizeof(*cfg->port_ids));
        if (cfg->port_ids == NULL) {
            free(port_list);
            free_csv_list(csv_list, nb_csv_list);
            free(single_csv);
            return -1;
        }
        if (has_single_port || cfg->nb_ports == 1U) {
            cfg->port_ids[0] = has_single_port ? single_port : 0U;
        } else {
            free(port_list);
            free_csv_list(csv_list, nb_csv_list);
            free(single_csv);
            return -1;
        }
    }

    if (has_csv_list) {
        cfg->csv_paths = csv_list;
        csv_list = NULL;
    } else {
        cfg->csv_paths = calloc(cfg->nb_ports, sizeof(*cfg->csv_paths));
        if (cfg->csv_paths == NULL) {
            free(port_list);
            free_csv_list(csv_list, nb_csv_list);
            free(single_csv);
            return -1;
        }
        if (!has_single_csv) {
            free(port_list);
            free_csv_list(csv_list, nb_csv_list);
            free(single_csv);
            return -1;
        }
        cfg->csv_paths[0] = single_csv;
        single_csv = NULL;
    }

    if (cfg->port_ids == NULL || cfg->csv_paths == NULL || cfg->nb_ports == 0U) {
        free(port_list);
        free_csv_list(csv_list, nb_csv_list);
        free(single_csv);
        return -1;
    }

    if (cfg->nb_vc == 0U || cfg->ring_size == 0U || cfg->packet_size == 0U ||
        cfg->tx_burst_size == 0U || cfg->rx_burst_size == 0U || cfg->tick_us == 0U) {
        free(port_list);
        free_csv_list(csv_list, nb_csv_list);
        free(single_csv);
        return -1;
    }

    {
        uint16_t i = 0;
        uint16_t j = 0;
        for (i = 0; i < cfg->nb_ports; i++) {
            if (cfg->csv_paths[i] == NULL || cfg->csv_paths[i][0] == '\0') {
                free(port_list);
                free_csv_list(csv_list, nb_csv_list);
                free(single_csv);
                return -1;
            }
            for (j = (uint16_t)(i + 1U); j < cfg->nb_ports; j++) {
                if (cfg->port_ids[i] == cfg->port_ids[j]) {
                    free(port_list);
                    free_csv_list(csv_list, nb_csv_list);
                    free(single_csv);
                    return -1;
                }
            }
        }
    }

    free(port_list);
    free_csv_list(csv_list, nb_csv_list);
    free(single_csv);
    return 0;
}

static int init_port(sender_ctx_t *ctx) {
    const uint16_t port_id = ctx->cfg.port_id;
    const uint32_t packet_size = ctx->cfg.packet_size;
    struct rte_eth_conf port_conf;
    struct rte_eth_dev_info dev_info;
    uint16_t nb_rxd = 1024;
    const uint16_t nb_tx_queue = 1;
    const uint16_t nb_rx_queue = 1;
    uint16_t desired_mtu = 0;
    int rc = 0;

    memset(&port_conf, 0, sizeof(port_conf));
    port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

    rc = rte_eth_dev_configure(port_id, nb_rx_queue, nb_tx_queue, &port_conf);
    if (rc < 0) {
        return rc;
    }

    rc = rte_eth_rx_queue_setup(port_id, ctx->cfg.rx_queue_id, nb_rxd, rte_eth_dev_socket_id(port_id),
                                NULL, ctx->mbuf_pool);
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

    rc = rte_eth_tx_queue_setup(port_id, ctx->cfg.tx_queue_id, 1024, rte_eth_dev_socket_id(port_id),
                                NULL);
    if (rc < 0) {
        return rc;
    }

    rc = rte_eth_dev_start(port_id);
    if (rc < 0) {
        return rc;
    }

    return 0;
}

static int init_vc_rings(sender_ctx_t *ctx) {
    uint32_t i = 0;

    ctx->vc_queues = calloc(ctx->cfg.nb_vc, sizeof(*ctx->vc_queues));
    if (ctx->vc_queues == NULL) {
        return -1;
    }

    for (i = 0; i < ctx->cfg.nb_vc; i++) {
        char ring_name[64];

        snprintf(ring_name, sizeof(ring_name), "vc_ring_p%u_v%u", ctx->cfg.port_id, i);
        ctx->vc_queues[i].vc_id = i;
        ctx->vc_queues[i].ring =
            rte_ring_create(ring_name, ctx->cfg.ring_size, rte_socket_id(),
                            RING_F_SP_ENQ | RING_F_SC_DEQ);
        if (ctx->vc_queues[i].ring == NULL) {
            return -1;
        }
    }
    return 0;
}

static int init_vc_fc_states(sender_ctx_t *ctx) {
    uint32_t i = 0;

    ctx->vc_fc_states = calloc(ctx->cfg.nb_vc, sizeof(*ctx->vc_fc_states));
    ctx->feedback_rx_pkts = calloc(ctx->cfg.nb_vc, sizeof(*ctx->feedback_rx_pkts));
    if (ctx->vc_fc_states == NULL || ctx->feedback_rx_pkts == NULL) {
        return -1;
    }

    for (i = 0; i < ctx->cfg.nb_vc; i++) {
        ctx->vc_fc_states[i].fccl = ctx->cfg.initial_fccl;
        ctx->vc_fc_states[i].fctbs = 0U;
        ctx->vc_fc_states[i].tx_pkts = 0U;
        ctx->vc_fc_states[i].vc_dr = 0U;
        ctx->vc_fc_states[i].vc_bklg = 0U;
        ctx->vc_fc_states[i].threshold = ctx->cfg.initial_threshold;
        ctx->vc_fc_states[i].state = SENDER_INFINIFLOW_STATE_UN;
    }
    ctx->port_fc_state.fccl = ctx->cfg.initial_fccl;
    ctx->port_fc_state.fctbs = 0U;

    return 0;
}

static void cleanup_sender(sender_ctx_t *ctx) {
    uint32_t i = 0;

    if (ctx == NULL) {
        return;
    }

    if (ctx->vc_queues != NULL) {
        for (i = 0; i < ctx->cfg.nb_vc; i++) {
            if (ctx->vc_queues[i].ring != NULL) {
                struct rte_mbuf *mbuf = NULL;
                while (rte_ring_sc_dequeue(ctx->vc_queues[i].ring, (void **)&mbuf) == 0) {
                    rte_pktmbuf_free(mbuf);
                }
                rte_ring_free(ctx->vc_queues[i].ring);
            }
        }
    }

    if (rte_eth_dev_is_valid_port(ctx->cfg.port_id)) {
        rte_eth_dev_stop(ctx->cfg.port_id);
        rte_eth_dev_close(ctx->cfg.port_id);
    }

    free(ctx->feedback_rx_pkts);
    free(ctx->vc_fc_states);
    free(ctx->vc_queues);
    free(ctx->active_ids);
    free(ctx->pending_order);
    free(ctx->flows);
    memset(ctx, 0, sizeof(*ctx));
}

static int init_sender_port_ctx(sender_ctx_t *ctx, const sender_app_config_t *app_cfg,
                                uint16_t port_idx) {
    uint32_t max_vc_csv = 0;
    uint32_t data_room_size = 0;
    char mempool_name[64];
    int rc = 0;

    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg.csv_path = app_cfg->csv_paths[port_idx];
    ctx->cfg.port_id = app_cfg->port_ids[port_idx];
    ctx->cfg.rx_queue_id = 0;
    ctx->cfg.tx_queue_id = 0;
    ctx->cfg.nb_vc = app_cfg->nb_vc;
    ctx->cfg.ring_size = app_cfg->ring_size;
    ctx->cfg.packet_size = app_cfg->packet_size;
    ctx->cfg.mempool_size = app_cfg->mempool_size;
    ctx->cfg.tx_burst_size = app_cfg->tx_burst_size;
    ctx->cfg.rx_burst_size = app_cfg->rx_burst_size;
    ctx->cfg.tick_us = app_cfg->tick_us;
    ctx->cfg.initial_fccl = app_cfg->initial_fccl;
    ctx->cfg.qmin = app_cfg->qmin;
    ctx->cfg.qmax = app_cfg->qmax;
    ctx->cfg.initial_threshold = app_cfg->initial_threshold;
    ctx->cfg.fc_mode = app_cfg->fc_mode;
    ctx->hz = rte_get_timer_hz();

    rc = csv_loader_load(ctx->cfg.csv_path, &ctx->flows, &ctx->nb_flows, &max_vc_csv);
    if (rc != 0 || ctx->nb_flows == 0U) {
        return -1;
    }
    if (max_vc_csv >= ctx->cfg.nb_vc) {
        return -1;
    }
    if (ctx->cfg.packet_size < (RTE_ETHER_HDR_LEN + FC_DATA_HEADER_SIZE)) {
        return -1;
    }

    data_room_size = RTE_MAX(ctx->cfg.packet_size, (uint32_t)RTE_ETHER_MAX_LEN) + RTE_PKTMBUF_HEADROOM;
    if (data_room_size > UINT16_MAX) {
        return -1;
    }

    snprintf(mempool_name, sizeof(mempool_name), "sender_mbuf_pool_p%u", ctx->cfg.port_id);
    ctx->mbuf_pool =
        rte_pktmbuf_pool_create(mempool_name, ctx->cfg.mempool_size, MBUF_CACHE_SIZE, 0,
                                (uint16_t)data_room_size, rte_socket_id());
    if (ctx->mbuf_pool == NULL) {
        return -1;
    }
    if (init_port(ctx) != 0) {
        return -1;
    }
    if (init_vc_rings(ctx) != 0) {
        return -1;
    }
    if (init_vc_fc_states(ctx) != 0) {
        return -1;
    }
    if (activity_manager_init(ctx) != 0) {
        return -1;
    }
    return 0;
}

static int producer_loop(void *arg) {
    sender_worker_arg_t *warg = (sender_worker_arg_t *)arg;
    sender_app_ctx_t *app = warg->app;
    sender_ctx_t *ctx = &app->port_ctxs[warg->port_idx];

    while (__atomic_load_n(&app->start_flag, __ATOMIC_ACQUIRE) == 0U && !g_force_quit) {
        rte_pause();
    }
    if (g_force_quit) {
        __atomic_store_n(&app->producer_done[warg->port_idx], 1U, __ATOMIC_RELEASE);
        return 0;
    }

    ctx->start_cycles = app->global_start_cycles;
    while (!g_force_quit) {
        double now_sec = sender_now_sec(ctx);

        activity_manager_activate_ready(ctx, now_sec);
        scheduler_run_tick(ctx);
        activity_manager_compact(ctx);

        if (activity_manager_all_done(ctx)) {
            break;
        }
    }

    __atomic_store_n(&app->producer_done[warg->port_idx], 1U, __ATOMIC_RELEASE);
    return 0;
}

static int forward_loop(void *arg) {
    sender_worker_arg_t *warg = (sender_worker_arg_t *)arg;
    sender_app_ctx_t *app = warg->app;
    sender_ctx_t *ctx = &app->port_ctxs[warg->port_idx];

    while (!g_force_quit) {
        uint32_t n = forward_run_tick(ctx);
        if (__atomic_load_n(&app->producer_done[warg->port_idx], __ATOMIC_ACQUIRE) != 0U &&
            activity_manager_all_done(ctx) && n == 0U) {
            break;
        }
    }

    while (forward_run_tick(ctx) > 0U) {
    }

    return 0;
}

static int feedback_rx_loop(void *arg) {
    sender_worker_arg_t *warg = (sender_worker_arg_t *)arg;
    sender_app_ctx_t *app = warg->app;
    sender_ctx_t *ctx = &app->port_ctxs[warg->port_idx];

    while (!g_force_quit) {
        sender_feedback_rx_run_tick(ctx);
        if (__atomic_load_n(&app->producer_done[warg->port_idx], __ATOMIC_ACQUIRE) != 0U &&
            activity_manager_all_done(ctx)) {
            break;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    sender_app_ctx_t app;
    sender_worker_arg_t *worker_args = NULL;
    unsigned int *producer_lcores = NULL;
    unsigned int *forward_lcores = NULL;
    unsigned int *feedback_lcores = NULL;
    unsigned int worker_ids[RTE_MAX_LCORE];
    uint16_t nb_workers = 0;
    uint16_t worker_cursor = 0;
    uint16_t i = 0;
    unsigned int lcore_id = 0;
    uint64_t total_enqueued = 0;
    uint64_t total_tx = 0;
    int eal_argc = 0;
    int ret = EXIT_FAILURE;

    memset(&app, 0, sizeof(app));

    eal_argc = rte_eal_init(argc, argv);
    if (eal_argc < 0) {
        rte_exit(EXIT_FAILURE, "EAL init failed\n");
    }
    sync_user1_log_level_with_global();

    argc -= eal_argc;
    argv += eal_argc;
    optind = 1;

    if (parse_app_args(argc, argv, &app.cfg) != 0) {
        usage("sender");
        rte_exit(EXIT_FAILURE, "Invalid app arguments\n");
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (app.cfg.nb_ports == 0U) {
        fprintf(stderr, "no sender ports configured\n");
        goto out;
    }

    app.port_ctxs = calloc(app.cfg.nb_ports, sizeof(*app.port_ctxs));
    app.producer_done = calloc(app.cfg.nb_ports, sizeof(*app.producer_done));
    worker_args = calloc(app.cfg.nb_ports, sizeof(*worker_args));
    producer_lcores = calloc(app.cfg.nb_ports, sizeof(*producer_lcores));
    forward_lcores = calloc(app.cfg.nb_ports, sizeof(*forward_lcores));
    feedback_lcores = calloc(app.cfg.nb_ports, sizeof(*feedback_lcores));
    if (app.port_ctxs == NULL || app.producer_done == NULL || worker_args == NULL ||
        producer_lcores == NULL || forward_lcores == NULL || feedback_lcores == NULL) {
        fprintf(stderr, "allocation failed for multi-port sender state\n");
        goto out;
    }

    for (i = 0; i < app.cfg.nb_ports; i++) {
        uint16_t port_id = app.cfg.port_ids[i];
        if (!rte_eth_dev_is_valid_port(port_id)) {
            fprintf(stderr, "invalid sender port id=%u\n", port_id);
            goto out;
        }
        if (init_sender_port_ctx(&app.port_ctxs[i], &app.cfg, i) != 0) {
            fprintf(stderr, "init failed for sender port=%u csv=%s\n", port_id, app.cfg.csv_paths[i]);
            goto out;
        }
    }

    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (nb_workers < RTE_DIM(worker_ids)) {
            worker_ids[nb_workers++] = lcore_id;
        }
    }
    if (nb_workers < (uint16_t)(app.cfg.nb_ports * 3U)) {
        fprintf(stderr, "Need at least %u worker lcores for %u ports\n", app.cfg.nb_ports * 3U,
                app.cfg.nb_ports);
        goto out;
    }

    for (i = 0; i < app.cfg.nb_ports; i++) {
        producer_lcores[i] = worker_ids[worker_cursor++];
        forward_lcores[i] = worker_ids[worker_cursor++];
        feedback_lcores[i] = worker_ids[worker_cursor++];
        worker_args[i].app = &app;
        worker_args[i].port_idx = i;
    }

    for (i = 0; i < app.cfg.nb_ports; i++) {
        rte_eal_remote_launch(producer_loop, &worker_args[i], producer_lcores[i]);
        rte_eal_remote_launch(forward_loop, &worker_args[i], forward_lcores[i]);
        rte_eal_remote_launch(feedback_rx_loop, &worker_args[i], feedback_lcores[i]);
    }

    app.global_start_cycles = rte_get_timer_cycles();
    __atomic_store_n(&app.start_flag, 1U, __ATOMIC_RELEASE);

    for (i = 0; i < app.cfg.nb_ports; i++) {
        rte_eal_wait_lcore(producer_lcores[i]);
        rte_eal_wait_lcore(forward_lcores[i]);
        rte_eal_wait_lcore(feedback_lcores[i]);
    }

    for (i = 0; i < app.cfg.nb_ports; i++) {
        uint32_t vc_id = 0;

        printf("Sender port=%u completed. enqueued=%" PRIu64 ", tx=%" PRIu64 ", csv=%s\n",
               app.port_ctxs[i].cfg.port_id, app.port_ctxs[i].total_pkts_enqueued,
               app.port_ctxs[i].total_pkts_tx, app.port_ctxs[i].cfg.csv_path);
        for (vc_id = 0; vc_id < app.port_ctxs[i].cfg.nb_vc; vc_id++) {
            uint64_t fccl = __atomic_load_n(&app.port_ctxs[i].vc_fc_states[vc_id].fccl, __ATOMIC_RELAXED);
            uint64_t fctbs = __atomic_load_n(&app.port_ctxs[i].vc_fc_states[vc_id].fctbs, __ATOMIC_RELAXED);
            uint64_t feedback_rx =
                __atomic_load_n(&app.port_ctxs[i].feedback_rx_pkts[vc_id], __ATOMIC_RELAXED);

            if (app.port_ctxs[i].cfg.fc_mode == FC_MODE_INFINIFLOW) {
                uint64_t tx_pkts =
                    __atomic_load_n(&app.port_ctxs[i].vc_fc_states[vc_id].tx_pkts, __ATOMIC_RELAXED);
                uint64_t vc_dr =
                    __atomic_load_n(&app.port_ctxs[i].vc_fc_states[vc_id].vc_dr, __ATOMIC_RELAXED);
                uint64_t vc_bklg =
                    __atomic_load_n(&app.port_ctxs[i].vc_fc_states[vc_id].vc_bklg, __ATOMIC_RELAXED);
                uint64_t threshold =
                    __atomic_load_n(&app.port_ctxs[i].vc_fc_states[vc_id].threshold, __ATOMIC_RELAXED);
                uint64_t port_fccl =
                    __atomic_load_n(&app.port_ctxs[i].port_fc_state.fccl, __ATOMIC_RELAXED);
                uint64_t port_fctbs =
                    __atomic_load_n(&app.port_ctxs[i].port_fc_state.fctbs, __ATOMIC_RELAXED);
                uint64_t credit_pool = (port_fccl > port_fctbs) ? (port_fccl - port_fctbs) : 0U;
                uint64_t inflight = (tx_pkts > vc_dr) ? (tx_pkts - vc_dr) : 0U;

                printf("  sender port=%u vc=%" PRIu32
                       ": ring=%u feedback-rx=%" PRIu64 " port-credit=%" PRIu64
                       " threshold=%" PRIu64 " inflight=%" PRIu64
                       " vc-dr=%" PRIu64 " vc-bklg=%" PRIu64 " state=%" PRIu32 "\n",
                       app.port_ctxs[i].cfg.port_id, vc_id,
                       rte_ring_count(app.port_ctxs[i].vc_queues[vc_id].ring), feedback_rx, credit_pool,
                       threshold, inflight, vc_dr, vc_bklg,
                       __atomic_load_n(&app.port_ctxs[i].vc_fc_states[vc_id].state, __ATOMIC_RELAXED));
            } else {
                uint64_t credit = (fccl > fctbs) ? (fccl - fctbs) : 0U;

                printf("  sender port=%u vc=%" PRIu32
                       ": ring=%u feedback-rx=%" PRIu64 " fccl=%" PRIu64
                       " fctbs=%" PRIu64 " credit=%" PRIu64 "\n",
                       app.port_ctxs[i].cfg.port_id, vc_id,
                       rte_ring_count(app.port_ctxs[i].vc_queues[vc_id].ring), feedback_rx, fccl, fctbs,
                       credit);
            }
        }
        total_enqueued += app.port_ctxs[i].total_pkts_enqueued;
        total_tx += app.port_ctxs[i].total_pkts_tx;
    }
    printf("Sender total completed. enqueued=%" PRIu64 ", tx=%" PRIu64 "\n", total_enqueued, total_tx);
    ret = EXIT_SUCCESS;

out:
    if (app.port_ctxs != NULL) {
        for (i = 0; i < app.cfg.nb_ports; i++) {
            cleanup_sender(&app.port_ctxs[i]);
        }
    }
    free(feedback_lcores);
    free(forward_lcores);
    free(producer_lcores);
    free(worker_args);
    free(app.producer_done);
    free(app.port_ctxs);
    free_app_config(&app.cfg);
    return ret;
}
