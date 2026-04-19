#include "receiver/receiver_ctx.h"
#include "core/fc_header.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>

#define DEFAULT_PORTS "0"
#define DEFAULT_MEMPOOL_SIZE 32768U
#define DEFAULT_RX_BURST 64U
#define DEFAULT_TX_BURST 64U
#define DEFAULT_NB_VC 8U
#define DEFAULT_CBFC_TOTAL_BUFFER_PKTS 8192U
#define DEFAULT_INFINIFLOW_QMIN 1U
#define DEFAULT_INFINIFLOW_QMAX 1U
#define DEFAULT_INFINIFLOW_INITIAL_THRESHOLD 1U
#define DEFAULT_INFINIFLOW_PORT_BUFFER_PKTS 1U
#define DEFAULT_PKT_SIZE 8000U
#define DEFAULT_OUTPUT_PATH "receiver_flow_stats.csv"
#define DEFAULT_FEEDBACK_RING_SIZE 1024U
#define MBUF_CACHE_SIZE 256U
#define FLOW_TABLE_INIT_CAP 1024U
#define MAX_RX_BURST 256U

typedef struct receiver_app_config_s {
    uint16_t *port_ids;
    uint16_t nb_ports;
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
} receiver_app_config_t;

typedef struct receiver_app_s {
    receiver_app_config_t cfg;
    receiver_ctx_t *port_ctxs;
    uint64_t start_cycles;
    uint64_t hz;
} receiver_app_t;

typedef struct receiver_worker_arg_s {
    receiver_app_t *app;
    uint16_t port_idx;
} receiver_worker_arg_t;

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

static void free_app_config(receiver_app_config_t *cfg) {
    free(cfg->port_ids);
    cfg->port_ids = NULL;
    cfg->nb_ports = 0;
}

static void usage(const char *prog) {
    printf("Usage: %s [EAL args] -- [options]\n", prog);
    printf("Options:\n");
    printf("  --ports <a,b>        NIC port ids (default: %s)\n", DEFAULT_PORTS);
    printf("  --vcs <num>          Number of virtual channels (default: 8)\n");
    printf("  --rx-burst <num>     RX burst size (default: 64)\n");
    printf("  --tx-burst <num>     TX burst size for feedback (default: 64)\n");
    printf("  --pkt-size <bytes>   Expected packet size in bytes (default: 8000)\n");
    printf("  --mempool <num>      Per-port mempool object count (default: 32768)\n");
    printf("  --fc-mode <mode>     Flow control mode: none|cbfc|infiniflow (default: none)\n");
    printf("  --cbfc-buffer-pkts <num>  Per-port CBFC buffer packets shared by all VCs (default: 8192)\n");
    printf("  --feedback-ring-size <num>  Per-port CBFC feedback ring depth (default: 1024)\n");
    printf("  --qmin <num>         InfiniFlow qmin (default: 1)\n");
    printf("  --qmax <num>         InfiniFlow qmax (default: 1)\n");
    printf("  --initial-threshold <num>  InfiniFlow initial threshold (default: 1)\n");
    printf("  --port-buffer-pkts <num>   InfiniFlow shared port buffer packets (default: 1)\n");
    printf("  --output <path>      Output CSV file path (default: receiver_flow_stats.csv)\n");
}

static int parse_app_args(int argc, char **argv, receiver_app_config_t *cfg) {
    static const struct option long_opts[] = {
        {"ports", required_argument, 0, 'P'},
        {"vcs", required_argument, 0, 'v'},
        {"rx-burst", required_argument, 0, 'b'},
        {"tx-burst", required_argument, 0, 't'},
        {"pkt-size", required_argument, 0, 's'},
        {"mempool", required_argument, 0, 'm'},
        {"fc-mode", required_argument, 0, 'f'},
        {"cbfc-buffer-pkts", required_argument, 0, 'c'},
        {"feedback-ring-size", required_argument, 0, 'q'},
        {"qmin", required_argument, 0, 'n'},
        {"qmax", required_argument, 0, 'N'},
        {"initial-threshold", required_argument, 0, 'T'},
        {"port-buffer-pkts", required_argument, 0, 'B'},
        {"output", required_argument, 0, 'o'},
        {0, 0, 0, 0},
    };
    int opt = 0;

    while ((opt = getopt_long(argc, argv, "P:v:b:t:s:m:f:c:q:n:N:T:B:o:", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'P': {
                uint16_t *port_ids = NULL;
                uint16_t nb_ports = 0;

                if (parse_port_list(optarg, &port_ids, &nb_ports) != 0) {
                    return -1;
                }
                free(cfg->port_ids);
                cfg->port_ids = port_ids;
                cfg->nb_ports = nb_ports;
                break;
            }
            case 'v':
                if (parse_u32(optarg, &cfg->nb_vc) != 0) {
                    return -1;
                }
                break;
            case 'b':
                if (parse_u32(optarg, &cfg->rx_burst_size) != 0) {
                    return -1;
                }
                break;
            case 't':
                if (parse_u32(optarg, &cfg->tx_burst_size) != 0) {
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
            case 'f':
                if (parse_fc_mode(optarg, &cfg->fc_mode) != 0) {
                    return -1;
                }
                break;
            case 'c':
                if (parse_u64(optarg, &cfg->cbfc_total_buffer_pkts) != 0) {
                    return -1;
                }
                break;
            case 'q':
                if (parse_u32(optarg, &cfg->feedback_ring_size) != 0) {
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
            case 'o':
                cfg->output_path = optarg;
                break;
            default:
                return -1;
        }
    }

    if (cfg->nb_ports == 0U || cfg->nb_vc == 0U || cfg->rx_burst_size == 0U ||
        cfg->tx_burst_size == 0U || cfg->packet_size == 0U || cfg->mempool_size == 0U ||
        cfg->feedback_ring_size == 0U || cfg->output_path == NULL) {
        return -1;
    }

    return 0;
}

static int validate_app_config(const receiver_app_config_t *cfg) {
    uint16_t i = 0;

    if (cfg->packet_size < (RTE_ETHER_HDR_LEN + FC_DATA_HEADER_SIZE)) {
        return -1;
    }

    for (i = 0; i < cfg->nb_ports; i++) {
        if (!rte_eth_dev_is_valid_port(cfg->port_ids[i])) {
            return -1;
        }
    }

    return 0;
}

static int init_port(receiver_ctx_t *ctx) {
    struct rte_eth_conf port_conf;
    struct rte_eth_dev_info dev_info;
    uint16_t nb_rxd = 1024;
    uint16_t nb_txd = 1024;
    uint16_t desired_mtu = 0;
    int socket_id = rte_eth_dev_socket_id(ctx->cfg.port_id);
    int rc = 0;

    if (socket_id < 0) {
        socket_id = rte_socket_id();
    }

    memset(&port_conf, 0, sizeof(port_conf));

    rc = rte_eth_dev_configure(ctx->cfg.port_id, 1, 1, &port_conf);
    if (rc < 0) {
        RTE_LOG(ERR, USER1,
                "receiver port=%" PRIu16 " configure failed: rc=%d err=%d(%s)\n",
                ctx->cfg.port_id, rc, rte_errno, rte_strerror(rte_errno));
        return rc;
    }

    rc = rte_eth_rx_queue_setup(ctx->cfg.port_id, ctx->cfg.rx_queue_id, nb_rxd, socket_id, NULL,
                                ctx->mbuf_pool);
    if (rc < 0) {
        RTE_LOG(ERR, USER1,
                "receiver port=%" PRIu16 " rx queue setup failed: queue=%" PRIu16
                " desc=%" PRIu16 " socket=%d rc=%d err=%d(%s)\n",
                ctx->cfg.port_id, ctx->cfg.rx_queue_id, nb_rxd, socket_id, rc, rte_errno,
                rte_strerror(rte_errno));
        return rc;
    }

    rc = rte_eth_tx_queue_setup(ctx->cfg.port_id, ctx->cfg.tx_queue_id, nb_txd, socket_id, NULL);
    if (rc < 0) {
        RTE_LOG(ERR, USER1,
                "receiver port=%" PRIu16 " tx queue setup failed: queue=%" PRIu16
                " desc=%" PRIu16 " socket=%d rc=%d err=%d(%s)\n",
                ctx->cfg.port_id, ctx->cfg.tx_queue_id, nb_txd, socket_id, rc, rte_errno,
                rte_strerror(rte_errno));
        return rc;
    }

    if (ctx->cfg.packet_size > RTE_ETHER_MTU + RTE_ETHER_HDR_LEN) {
        desired_mtu = (uint16_t)(ctx->cfg.packet_size - RTE_ETHER_HDR_LEN);

        rc = rte_eth_dev_info_get(ctx->cfg.port_id, &dev_info);
        if (rc < 0) {
            RTE_LOG(ERR, USER1,
                    "receiver port=%" PRIu16 " dev info get failed: rc=%d err=%d(%s)\n",
                    ctx->cfg.port_id, rc, rte_errno, rte_strerror(rte_errno));
            return rc;
        }
        if (desired_mtu > dev_info.max_mtu) {
            RTE_LOG(ERR, USER1,
                    "receiver port=%" PRIu16 " mtu too large: desired=%" PRIu16
                    " max=%" PRIu32 " packet_size=%" PRIu32 "\n",
                    ctx->cfg.port_id, desired_mtu, dev_info.max_mtu, ctx->cfg.packet_size);
            return -1;
        }

        rc = rte_eth_dev_set_mtu(ctx->cfg.port_id, desired_mtu);
        if (rc < 0) {
            RTE_LOG(ERR, USER1,
                    "receiver port=%" PRIu16 " set mtu failed: mtu=%" PRIu16
                    " rc=%d err=%d(%s)\n",
                    ctx->cfg.port_id, desired_mtu, rc, rte_errno, rte_strerror(rte_errno));
            return rc;
        }
    }

    rc = rte_eth_dev_start(ctx->cfg.port_id);
    if (rc < 0) {
        RTE_LOG(ERR, USER1,
                "receiver port=%" PRIu16 " start failed: rc=%d err=%d(%s)\n",
                ctx->cfg.port_id, rc, rte_errno, rte_strerror(rte_errno));
        return rc;
    }

    rte_eth_macaddr_get(ctx->cfg.port_id, &ctx->port_mac);
    return 0;
}

static int init_vc_cbfc_states(receiver_ctx_t *ctx) {
    uint32_t i = 0;
    uint64_t base = 0;
    uint64_t rem = 0;

    if (ctx->cfg.fc_mode == FC_MODE_CBFC) {
        ctx->vc_cbfc_states = calloc(ctx->cfg.nb_vc, sizeof(*ctx->vc_cbfc_states));
        if (ctx->vc_cbfc_states == NULL) {
            return -1;
        }

        base = ctx->cfg.cbfc_total_buffer_pkts / (uint64_t)ctx->cfg.nb_vc;
        rem = ctx->cfg.cbfc_total_buffer_pkts % (uint64_t)ctx->cfg.nb_vc;
        for (i = 0; i < ctx->cfg.nb_vc; i++) {
            ctx->vc_cbfc_states[i].buffer_cap = base + ((uint64_t)i < rem ? 1U : 0U);
            ctx->vc_cbfc_states[i].received = 0U;
        }
    } else if (ctx->cfg.fc_mode == FC_MODE_INFINIFLOW) {
        ctx->vc_infiniflow_states = calloc(ctx->cfg.nb_vc, sizeof(*ctx->vc_infiniflow_states));
        if (ctx->vc_infiniflow_states == NULL) {
            return -1;
        }
        for (i = 0; i < ctx->cfg.nb_vc; i++) {
            rte_spinlock_init(&ctx->vc_infiniflow_states[i].pending_feedback_lock);
        }
        ctx->port_infiniflow_state.total_received = 0U;
        ctx->port_infiniflow_state.total_drained = 0U;
    }

    return 0;
}

static uint32_t flow_hash(uint32_t flow_id) {
    return (flow_id * 2654435761U);
}

static int flow_table_rehash(receiver_ctx_t *ctx, uint32_t new_cap) {
    flow_record_t *new_records = NULL;
    uint32_t i = 0;

    new_records = calloc(new_cap, sizeof(*new_records));
    if (new_records == NULL) {
        return -1;
    }

    for (i = 0; i < ctx->records_cap; i++) {
        if (!ctx->records[i].used) {
            continue;
        }

        uint32_t idx = flow_hash(ctx->records[i].flow_id) & (new_cap - 1U);
        while (new_records[idx].used) {
            idx = (idx + 1U) & (new_cap - 1U);
        }
        new_records[idx] = ctx->records[i];
    }

    free(ctx->records);
    ctx->records = new_records;
    ctx->records_cap = new_cap;
    return 0;
}

static int flow_table_init(receiver_ctx_t *ctx) {
    ctx->records = calloc(FLOW_TABLE_INIT_CAP, sizeof(*ctx->records));
    if (ctx->records == NULL) {
        return -1;
    }
    ctx->records_cap = FLOW_TABLE_INIT_CAP;
    ctx->records_used = 0;
    return 0;
}

static flow_record_t *flow_table_get_or_create(receiver_ctx_t *ctx, uint32_t flow_id) {
    uint32_t idx = 0;
    uint32_t mask = 0;

    if (ctx->records_used * 10U >= ctx->records_cap * 7U) {
        if (flow_table_rehash(ctx, ctx->records_cap * 2U) != 0) {
            return NULL;
        }
    }

    mask = ctx->records_cap - 1U;
    idx = flow_hash(flow_id) & mask;
    while (ctx->records[idx].used && ctx->records[idx].flow_id != flow_id) {
        idx = (idx + 1U) & mask;
    }

    if (!ctx->records[idx].used) {
        ctx->records[idx].used = true;
        ctx->records[idx].flow_id = flow_id;
        ctx->records_used++;
    }

    return &ctx->records[idx];
}

static int flow_record_append_timestamp(flow_record_t *record, uint64_t ts_cycles) {
    if (record->ts_count == record->ts_cap) {
        uint32_t new_cap = (record->ts_cap == 0U) ? 256U : (record->ts_cap * 2U);
        uint64_t *new_buf = realloc(record->timestamps, new_cap * sizeof(*new_buf));
        if (new_buf == NULL) {
            return -1;
        }
        record->timestamps = new_buf;
        record->ts_cap = new_cap;
    }

    record->timestamps[record->ts_count++] = ts_cycles;
    return 0;
}

static int update_flow_stats(receiver_ctx_t *ctx, uint32_t flow_id, uint64_t ts_cycles,
                             uint32_t pkt_len) {
    flow_record_t *record = flow_table_get_or_create(ctx, flow_id);
    if (record == NULL) {
        return -1;
    }

    if (record->pkt_count == 0U) {
        record->first_ts_cycles = ts_cycles;
    }
    record->last_ts_cycles = ts_cycles;
    record->pkt_count++;
    record->byte_count += pkt_len;

    return flow_record_append_timestamp(record, ts_cycles);
}

static int process_one_packet(receiver_ctx_t *ctx, struct rte_mbuf *mbuf) {
    const struct rte_ether_hdr *eth_hdr = NULL;
    const fc_data_header_t *fc_hdr = NULL;
    uint32_t min_len = 0;
    uint32_t flow_id = 0;
    uint32_t vc_id = 0;
    uint64_t ts_cycles = 0;
    int rc = 0;

    if (mbuf->pkt_len < sizeof(struct rte_ether_hdr)) {
        return 0;
    }

    eth_hdr = rte_pktmbuf_mtod(mbuf, const struct rte_ether_hdr *);
    if (eth_hdr->ether_type != rte_cpu_to_be_16(FC_ETHER_TYPE)) {
        return 0;
    }

    min_len = sizeof(struct rte_ether_hdr) + FC_DATA_HEADER_SIZE;
    if (mbuf->pkt_len < min_len) {
        return 0;
    }

    fc_hdr = (const fc_data_header_t *)((const char *)eth_hdr + sizeof(*eth_hdr));
    flow_id = rte_be_to_cpu_32(fc_hdr->flow_id);
    vc_id = rte_be_to_cpu_32(fc_hdr->vc_id);
    ts_cycles = rte_get_timer_cycles();

    rc = update_flow_stats(ctx, flow_id, ts_cycles, mbuf->pkt_len);
    if (rc != 0) {
        return rc;
    }
    ctx->rx_fc_data_pkts++;

    if (ctx->cfg.fc_mode == FC_MODE_CBFC) {
        receiver_vc_cbfc_state_t *vc_state = NULL;
        uint64_t fccl = 0;
        uint64_t old_received = 0;
        uint64_t new_received = 0;

        if (vc_id >= ctx->cfg.nb_vc) {
            return 0;
        }
        vc_state = &ctx->vc_cbfc_states[vc_id];
        old_received = vc_state->received;
        vc_state->received = old_received + 1U;
        new_received = vc_state->received;
        fccl = vc_state->buffer_cap + vc_state->received;
        RTE_LOG(DEBUG, USER1,
                "[CBFC][receiver][rx] port=%" PRIu16 " vc=%" PRIu32 " flow=%" PRIu32
                " buffer_cap=%" PRIu64 " old_received=%" PRIu64
                " new_received=%" PRIu64 " fccl=%" PRIu64 "\n",
                ctx->cfg.port_id, vc_id, flow_id, vc_state->buffer_cap, old_received, new_received,
                fccl);
        receiver_feedback_try_enqueue(ctx, &eth_hdr->src_addr, vc_id, fccl, 0U, 0U, 0U);
    } else if (ctx->cfg.fc_mode == FC_MODE_INFINIFLOW) {
        receiver_vc_infiniflow_state_t *vc_state = NULL;
        uint32_t flags = fc_be32_to_cpu(fc_hdr->flags);
        uint64_t fccl = 0;

        if (vc_id >= ctx->cfg.nb_vc) {
            return 0;
        }

        vc_state = &ctx->vc_infiniflow_states[vc_id];
        vc_state->received++;
        vc_state->drained++;
        vc_state->backlog = 0U;
        ctx->port_infiniflow_state.total_received++;
        ctx->port_infiniflow_state.total_drained++;

        if ((flags & FC_DATA_FLAG_TA) != 0U) {
            receiver_feedback_note_ta(ctx, vc_id);
        }

        fccl = ctx->port_infiniflow_state.total_received + ctx->cfg.port_buffer_pkts;
        receiver_feedback_try_enqueue(ctx, &eth_hdr->src_addr, vc_id, fccl, vc_state->drained,
                                      vc_state->backlog, 0U);
    }

    return 0;
}

static int append_flow_stats_csv(FILE *fp, const receiver_ctx_t *ctx, uint64_t end_cycles) {
    uint32_t i = 0;
    double total_runtime_sec = (double)(end_cycles - ctx->start_cycles) / (double)ctx->hz;

    if (total_runtime_sec <= 0.0) {
        total_runtime_sec = 1.0 / (double)ctx->hz;
    }

    for (i = 0; i < ctx->records_cap; i++) {
        const flow_record_t *record = &ctx->records[i];
        double duration_sec = 0.0;
        double pps = 0.0;
        double bps = 0.0;
        uint32_t j = 0;

        if (!record->used || record->pkt_count == 0U) {
            continue;
        }

        if (record->pkt_count > 1U && record->last_ts_cycles > record->first_ts_cycles) {
            duration_sec =
                (double)(record->last_ts_cycles - record->first_ts_cycles) / (double)ctx->hz;
        } else {
            duration_sec = total_runtime_sec;
        }

        if (duration_sec <= 0.0) {
            duration_sec = 1.0 / (double)ctx->hz;
        }

        pps = (double)record->pkt_count / duration_sec;
        bps = ((double)record->byte_count * 8.0) / duration_sec;

        fprintf(fp, "%" PRIu16 ",%" PRIu32 ",%" PRIu32 ",%.6f,%.6f,\"", ctx->cfg.port_id,
                record->flow_id, record->ts_count, pps, bps);
        for (j = 0; j < record->ts_count; j++) {
            double ts_sec = (double)(record->timestamps[j] - ctx->start_cycles) / (double)ctx->hz;
            fprintf(fp, "%.9f", ts_sec);
            if (j + 1U < record->ts_count) {
                fputc(';', fp);
            }
        }
        fprintf(fp, "\"\n");
    }

    return 0;
}

static int flush_all_flow_stats_to_csv(const receiver_app_t *app, uint64_t end_cycles) {
    FILE *fp = NULL;
    uint16_t i = 0;

    fp = fopen(app->cfg.output_path, "w");
    if (fp == NULL) {
        return -1;
    }

    fprintf(fp, "port_id,flow_id,timestamp_count,pps,bps,timestamps_sec\n");
    for (i = 0; i < app->cfg.nb_ports; i++) {
        if (append_flow_stats_csv(fp, &app->port_ctxs[i], end_cycles) != 0) {
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);
    return 0;
}

static void receiver_run_rx_loop(receiver_ctx_t *ctx, uint16_t burst) {
    struct rte_mbuf *rx_pkts[MAX_RX_BURST];

    while (!g_force_quit) {
        uint16_t nb_rx = rte_eth_rx_burst(ctx->cfg.port_id, ctx->cfg.rx_queue_id, rx_pkts, burst);
        uint16_t i = 0;

        if (nb_rx == 0U) {
            continue;
        }

        for (i = 0; i < nb_rx; i++) {
            if (process_one_packet(ctx, rx_pkts[i]) != 0) {
                g_force_quit = 1;
                break;
            }
        }

        for (i = 0; i < nb_rx; i++) {
            rte_pktmbuf_free(rx_pkts[i]);
        }
    }
}

static int receiver_rx_loop(void *arg) {
    receiver_worker_arg_t *worker = (receiver_worker_arg_t *)arg;
    receiver_ctx_t *ctx = &worker->app->port_ctxs[worker->port_idx];
    uint16_t burst = (uint16_t)ctx->cfg.rx_burst_size;

    if (burst > MAX_RX_BURST) {
        burst = MAX_RX_BURST;
    }

    receiver_run_rx_loop(ctx, burst);
    return 0;
}

static int receiver_feedback_loop(void *arg) {
    receiver_worker_arg_t *worker = (receiver_worker_arg_t *)arg;
    receiver_ctx_t *ctx = &worker->app->port_ctxs[worker->port_idx];

    while (!g_force_quit) {
        receiver_feedback_tx_run_tick(ctx);
    }
    while (receiver_feedback_tx_run_tick(ctx) > 0U) {
    }

    return 0;
}

static void cleanup_receiver(receiver_ctx_t *ctx) {
    uint32_t i = 0;

    receiver_feedback_cleanup(ctx);

    if (ctx->cfg.port_id != UINT16_MAX && rte_eth_dev_is_valid_port(ctx->cfg.port_id)) {
        rte_eth_dev_stop(ctx->cfg.port_id);
        rte_eth_dev_close(ctx->cfg.port_id);
    }

    if (ctx->records != NULL) {
        for (i = 0; i < ctx->records_cap; i++) {
            free(ctx->records[i].timestamps);
        }
        free(ctx->records);
        ctx->records = NULL;
    }

    free(ctx->vc_cbfc_states);
    ctx->vc_cbfc_states = NULL;
    free(ctx->vc_infiniflow_states);
    ctx->vc_infiniflow_states = NULL;

    if (ctx->mbuf_pool != NULL) {
        rte_mempool_free(ctx->mbuf_pool);
        ctx->mbuf_pool = NULL;
    }
}

static int init_receiver_port_ctx(receiver_ctx_t *ctx, const receiver_app_t *app, uint16_t port_idx) {
    uint32_t data_room_size = 0;
    int socket_id = 0;

    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg.port_id = app->cfg.port_ids[port_idx];
    ctx->cfg.rx_queue_id = 0;
    ctx->cfg.tx_queue_id = 0;
    ctx->cfg.rx_burst_size = app->cfg.rx_burst_size;
    ctx->cfg.tx_burst_size = app->cfg.tx_burst_size;
    ctx->cfg.nb_vc = app->cfg.nb_vc;
    ctx->cfg.packet_size = app->cfg.packet_size;
    ctx->cfg.mempool_size = app->cfg.mempool_size;
    ctx->cfg.feedback_ring_size = app->cfg.feedback_ring_size;
    ctx->cfg.cbfc_total_buffer_pkts = app->cfg.cbfc_total_buffer_pkts;
    ctx->cfg.qmin = app->cfg.qmin;
    ctx->cfg.qmax = app->cfg.qmax;
    ctx->cfg.initial_threshold = app->cfg.initial_threshold;
    ctx->cfg.port_buffer_pkts = app->cfg.port_buffer_pkts;
    ctx->cfg.fc_mode = app->cfg.fc_mode;
    ctx->hz = app->hz;
    ctx->start_cycles = app->start_cycles;

    snprintf(ctx->mempool_name, sizeof(ctx->mempool_name), "rxmp_p%u", ctx->cfg.port_id);
    snprintf(ctx->feedback_ring_name, sizeof(ctx->feedback_ring_name), "rxfb_p%u",
             ctx->cfg.port_id);
    snprintf(ctx->feedback_free_ring_name, sizeof(ctx->feedback_free_ring_name), "rxff_p%u",
             ctx->cfg.port_id);

    data_room_size =
        RTE_MAX(ctx->cfg.packet_size, (uint32_t)RTE_ETHER_MAX_LEN) + RTE_PKTMBUF_HEADROOM;
    if (data_room_size > UINT16_MAX) {
        RTE_LOG(ERR, USER1,
                "receiver port=%" PRIu16 " data room too large: packet_size=%" PRIu32
                " data_room=%" PRIu32 "\n",
                ctx->cfg.port_id, ctx->cfg.packet_size, data_room_size);
        return -1;
    }

    socket_id = rte_eth_dev_socket_id(ctx->cfg.port_id);
    if (socket_id < 0) {
        socket_id = rte_socket_id();
    }

    ctx->mbuf_pool =
        rte_pktmbuf_pool_create(ctx->mempool_name, ctx->cfg.mempool_size, MBUF_CACHE_SIZE, 0,
                                (uint16_t)data_room_size, socket_id);
    if (ctx->mbuf_pool == NULL) {
        RTE_LOG(ERR, USER1,
                "receiver port=%" PRIu16 " mempool create failed: name=%s count=%" PRIu32
                " data_room=%" PRIu32 " socket=%d err=%d(%s)\n",
                ctx->cfg.port_id, ctx->mempool_name, ctx->cfg.mempool_size, data_room_size,
                socket_id, rte_errno, rte_strerror(rte_errno));
        return -1;
    }

    if (init_port(ctx) != 0) {
        RTE_LOG(ERR, USER1, "receiver port=%" PRIu16 " init_port failed\n", ctx->cfg.port_id);
        return -1;
    }
    if (flow_table_init(ctx) != 0) {
        RTE_LOG(ERR, USER1, "receiver port=%" PRIu16 " flow table init failed\n",
                ctx->cfg.port_id);
        return -1;
    }
    if (init_vc_cbfc_states(ctx) != 0) {
        RTE_LOG(ERR, USER1, "receiver port=%" PRIu16 " vc cbfc state init failed\n",
                ctx->cfg.port_id);
        return -1;
    }
    if (ctx->cfg.fc_mode != FC_MODE_NONE && receiver_feedback_init(ctx) != 0) {
        RTE_LOG(ERR, USER1,
                "receiver port=%" PRIu16 " feedback init failed: ring_size=%" PRIu32 "\n",
                ctx->cfg.port_id, ctx->cfg.feedback_ring_size);
        return -1;
    }

    RTE_LOG(INFO, USER1,
            "receiver port=%" PRIu16 " init ok: mempool=%s count=%" PRIu32
            " packet_size=%" PRIu32 " fc_mode=%s\n",
            ctx->cfg.port_id, ctx->mempool_name, ctx->cfg.mempool_size, ctx->cfg.packet_size,
            ctx->cfg.fc_mode == FC_MODE_CBFC
                ? "cbfc"
                : (ctx->cfg.fc_mode == FC_MODE_INFINIFLOW ? "infiniflow" : "none"));

    return 0;
}

int main(int argc, char **argv) {
    receiver_app_t app;
    receiver_worker_arg_t *worker_args = NULL;
    unsigned int *rx_lcores = NULL;
    unsigned int *feedback_lcores = NULL;
    unsigned int worker_ids[RTE_MAX_LCORE];
    uint16_t nb_workers = 0;
    uint16_t required_workers = 0;
    uint16_t worker_cursor = 0;
    uint16_t launched_rx = 0;
    uint16_t launched_feedback = 0;
    uint16_t i = 0;
    unsigned int lcore_id = 0;
    uint64_t total_rx_pkts = 0;
    uint64_t total_feedback_tx_pkts = 0;
    uint64_t total_feedback_drops = 0;
    int eal_argc = 0;
    int ret = EXIT_FAILURE;

    memset(&app, 0, sizeof(app));
    if (parse_port_list(DEFAULT_PORTS, &app.cfg.port_ids, &app.cfg.nb_ports) != 0) {
        rte_exit(EXIT_FAILURE, "default ports parse failed\n");
    }
    app.cfg.nb_vc = DEFAULT_NB_VC;
    app.cfg.rx_burst_size = DEFAULT_RX_BURST;
    app.cfg.tx_burst_size = DEFAULT_TX_BURST;
    app.cfg.packet_size = DEFAULT_PKT_SIZE;
    app.cfg.mempool_size = DEFAULT_MEMPOOL_SIZE;
    app.cfg.feedback_ring_size = DEFAULT_FEEDBACK_RING_SIZE;
    app.cfg.cbfc_total_buffer_pkts = DEFAULT_CBFC_TOTAL_BUFFER_PKTS;
    app.cfg.qmin = DEFAULT_INFINIFLOW_QMIN;
    app.cfg.qmax = DEFAULT_INFINIFLOW_QMAX;
    app.cfg.initial_threshold = DEFAULT_INFINIFLOW_INITIAL_THRESHOLD;
    app.cfg.port_buffer_pkts = DEFAULT_INFINIFLOW_PORT_BUFFER_PKTS;
    app.cfg.fc_mode = FC_MODE_NONE;
    app.cfg.output_path = DEFAULT_OUTPUT_PATH;

    eal_argc = rte_eal_init(argc, argv);
    if (eal_argc < 0) {
        rte_exit(EXIT_FAILURE, "EAL init failed\n");
    }
    sync_user1_log_level_with_global();

    argc -= eal_argc;
    argv += eal_argc;
    optind = 1;

    if (parse_app_args(argc, argv, &app.cfg) != 0) {
        usage("receiver");
        rte_exit(EXIT_FAILURE, "Invalid app arguments\n");
    }
    if (validate_app_config(&app.cfg) != 0) {
        rte_exit(EXIT_FAILURE, "Invalid receiver port list or packet size\n");
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    app.hz = rte_get_timer_hz();
    app.start_cycles = rte_get_timer_cycles();
    app.port_ctxs = calloc(app.cfg.nb_ports, sizeof(*app.port_ctxs));
    worker_args = calloc(app.cfg.nb_ports, sizeof(*worker_args));
    rx_lcores = calloc(app.cfg.nb_ports, sizeof(*rx_lcores));
    if (app.port_ctxs == NULL || worker_args == NULL || rx_lcores == NULL) {
        fprintf(stderr, "receiver multi-port allocation failed\n");
        goto out;
    }
    if (app.cfg.fc_mode != FC_MODE_NONE) {
        feedback_lcores = calloc(app.cfg.nb_ports, sizeof(*feedback_lcores));
        if (feedback_lcores == NULL) {
            fprintf(stderr, "receiver feedback lcore allocation failed\n");
            goto out;
        }
    }
    for (i = 0; i < app.cfg.nb_ports; i++) {
        app.port_ctxs[i].cfg.port_id = UINT16_MAX;
    }

    for (i = 0; i < app.cfg.nb_ports; i++) {
        if (init_receiver_port_ctx(&app.port_ctxs[i], &app, i) != 0) {
            fprintf(stderr, "init failed for receiver port=%u\n", app.cfg.port_ids[i]);
            goto out;
        }
        worker_args[i].app = &app;
        worker_args[i].port_idx = i;
    }

    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (nb_workers < RTE_DIM(worker_ids)) {
            worker_ids[nb_workers++] = lcore_id;
        }
    }

    required_workers = app.cfg.nb_ports;
    if (app.cfg.fc_mode != FC_MODE_NONE) {
        required_workers = (uint16_t)(required_workers + app.cfg.nb_ports);
    }
    if (nb_workers < required_workers) {
        fprintf(stderr, "Need at least %u worker lcores for %u receiver ports in %s mode\n",
                required_workers, app.cfg.nb_ports,
                app.cfg.fc_mode == FC_MODE_CBFC
                    ? "cbfc"
                    : (app.cfg.fc_mode == FC_MODE_INFINIFLOW ? "infiniflow" : "normal"));
        goto out;
    }

    for (i = 0; i < app.cfg.nb_ports; i++) {
        rx_lcores[i] = worker_ids[worker_cursor++];
    }
    if (app.cfg.fc_mode != FC_MODE_NONE) {
        for (i = 0; i < app.cfg.nb_ports; i++) {
            feedback_lcores[i] = worker_ids[worker_cursor++];
        }
    }

    for (i = 0; i < app.cfg.nb_ports; i++) {
        if (rte_eal_remote_launch(receiver_rx_loop, &worker_args[i], rx_lcores[i]) != 0) {
            fprintf(stderr, "failed to launch receiver RX loop on lcore %u\n", rx_lcores[i]);
            goto out;
        }
        launched_rx++;
    }
    if (app.cfg.fc_mode != FC_MODE_NONE) {
        for (i = 0; i < app.cfg.nb_ports; i++) {
            if (rte_eal_remote_launch(receiver_feedback_loop, &worker_args[i], feedback_lcores[i]) !=
                0) {
                fprintf(stderr, "failed to launch receiver feedback loop on lcore %u\n",
                        feedback_lcores[i]);
                goto out;
            }
            launched_feedback++;
        }
    }

    for (i = 0; i < launched_rx; i++) {
        rte_eal_wait_lcore(rx_lcores[i]);
    }
    launched_rx = 0;
    if (app.cfg.fc_mode != FC_MODE_NONE) {
        for (i = 0; i < launched_feedback; i++) {
            rte_eal_wait_lcore(feedback_lcores[i]);
        }
        launched_feedback = 0;
    }

    if (flush_all_flow_stats_to_csv(&app, rte_get_timer_cycles()) != 0) {
        fprintf(stderr, "failed to write output CSV: %s\n", app.cfg.output_path);
        goto out;
    }

    for (i = 0; i < app.cfg.nb_ports; i++) {
        total_rx_pkts += app.port_ctxs[i].rx_fc_data_pkts;
        total_feedback_tx_pkts += app.port_ctxs[i].tx_cbfc_feedback_pkts;
        total_feedback_drops += app.port_ctxs[i].feedback_enqueue_drop;
        RTE_LOG(INFO, USER1,
                "receiver port=%" PRIu16 " exit: received FC data packets=%" PRIu64
                ", sent feedback packets=%" PRIu64 ", feedback enqueue drops=%" PRIu64 "\n",
                app.port_ctxs[i].cfg.port_id, app.port_ctxs[i].rx_fc_data_pkts,
                app.port_ctxs[i].tx_cbfc_feedback_pkts, app.port_ctxs[i].feedback_enqueue_drop);
    }
    RTE_LOG(INFO, USER1,
            "receiver total exit: ports=%" PRIu16 ", received FC data packets=%" PRIu64
            ", sent feedback packets=%" PRIu64 ", feedback enqueue drops=%" PRIu64 "\n",
            app.cfg.nb_ports, total_rx_pkts, total_feedback_tx_pkts, total_feedback_drops);
    ret = EXIT_SUCCESS;

out:
    g_force_quit = 1;
    for (i = 0; i < launched_rx; i++) {
        rte_eal_wait_lcore(rx_lcores[i]);
    }
    for (i = 0; i < launched_feedback; i++) {
        rte_eal_wait_lcore(feedback_lcores[i]);
    }
    if (app.port_ctxs != NULL) {
        for (i = 0; i < app.cfg.nb_ports; i++) {
            cleanup_receiver(&app.port_ctxs[i]);
        }
    }
    free(feedback_lcores);
    free(rx_lcores);
    free(worker_args);
    free(app.port_ctxs);
    free_app_config(&app.cfg);
    return ret;
}
