#include "receiver/receiver_ctx.h"
#include "core/fc_header.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_byteorder.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>

#define DEFAULT_MEMPOOL_SIZE 32768U
#define DEFAULT_RX_BURST 64U
#define DEFAULT_OUTPUT_PATH "receiver_flow_stats.csv"
#define MBUF_CACHE_SIZE 256U
#define FLOW_TABLE_INIT_CAP 1024U
#define MAX_RX_BURST 256U

static volatile sig_atomic_t g_force_quit = 0;

static void handle_signal(int signum) {
    (void)signum;
    g_force_quit = 1;
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

static void usage(const char *prog) {
    printf("Usage: %s [EAL args] -- [options]\n", prog);
    printf("Options:\n");
    printf("  --port <id>          NIC port id (default: 0)\n");
    printf("  --rx-burst <num>     RX burst size (default: 64)\n");
    printf("  --mempool <num>      Mempool object count (default: 32768)\n");
    printf("  --output <path>      Output CSV file path (default: receiver_flow_stats.csv)\n");
}

static int parse_app_args(int argc, char **argv, receiver_config_t *cfg) {
    static const struct option long_opts[] = {
        {"port", required_argument, 0, 'p'},
        {"rx-burst", required_argument, 0, 'b'},
        {"mempool", required_argument, 0, 'm'},
        {"output", required_argument, 0, 'o'},
        {0, 0, 0, 0},
    };

    int opt = 0;

    while ((opt = getopt_long(argc, argv, "p:b:m:o:", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'p':
                if (parse_u16(optarg, &cfg->port_id) != 0) {
                    return -1;
                }
                break;
            case 'b':
                if (parse_u32(optarg, &cfg->rx_burst_size) != 0) {
                    return -1;
                }
                break;
            case 'm':
                if (parse_u32(optarg, &cfg->mempool_size) != 0) {
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

    if (cfg->rx_burst_size == 0U || cfg->mempool_size == 0U || cfg->output_path == NULL) {
        return -1;
    }

    return 0;
}

static int init_port(receiver_ctx_t *ctx) {
    struct rte_eth_conf port_conf;
    uint16_t nb_rxd = 1024;
    int rc = 0;

    memset(&port_conf, 0, sizeof(port_conf));

    rc = rte_eth_dev_configure(ctx->cfg.port_id, 1, 0, &port_conf);
    if (rc < 0) {
        return rc;
    }

    rc = rte_eth_rx_queue_setup(ctx->cfg.port_id, ctx->cfg.rx_queue_id, nb_rxd,
                                rte_eth_dev_socket_id(ctx->cfg.port_id), NULL, ctx->mbuf_pool);
    if (rc < 0) {
        return rc;
    }

    rc = rte_eth_dev_start(ctx->cfg.port_id);
    if (rc < 0) {
        return rc;
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
    const struct rte_ipv4_hdr *ipv4_hdr = NULL;
    const fc_header_t *fc_hdr = NULL;
    uint32_t ipv4_hdr_len = 0;
    uint32_t min_len = 0;
    uint32_t flow_id = 0;
    uint64_t ts_cycles = 0;

    if (mbuf->pkt_len < sizeof(struct rte_ether_hdr)) {
        return 0;
    }

    eth_hdr = rte_pktmbuf_mtod(mbuf, const struct rte_ether_hdr *);
    if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        return 0;
    }

    min_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(fc_header_t);
    if (mbuf->pkt_len < min_len) {
        return 0;
    }

    ipv4_hdr = (const struct rte_ipv4_hdr *)((const char *)eth_hdr + sizeof(*eth_hdr));
    ipv4_hdr_len = (uint32_t)(ipv4_hdr->version_ihl & 0x0FU) * 4U;
    if (ipv4_hdr_len < sizeof(struct rte_ipv4_hdr)) {
        return 0;
    }

    if (ipv4_hdr->next_proto_id != FC_IPPROTO) {
        return 0;
    }

    if (mbuf->pkt_len < sizeof(struct rte_ether_hdr) + ipv4_hdr_len + sizeof(fc_header_t)) {
        return 0;
    }

    fc_hdr = (const fc_header_t *)((const char *)ipv4_hdr + ipv4_hdr_len);
    flow_id = rte_be_to_cpu_32(fc_hdr->flow_id);
    ts_cycles = rte_get_timer_cycles();

    return update_flow_stats(ctx, flow_id, ts_cycles, mbuf->pkt_len);
}

static int flush_flow_stats_to_csv(const receiver_ctx_t *ctx, uint64_t end_cycles) {
    FILE *fp = NULL;
    uint32_t i = 0;
    double total_runtime_sec = 0.0;

    fp = fopen(ctx->cfg.output_path, "w");
    if (fp == NULL) {
        return -1;
    }

    total_runtime_sec = (double)(end_cycles - ctx->start_cycles) / (double)ctx->hz;
    if (total_runtime_sec <= 0.0) {
        total_runtime_sec = 1.0 / (double)ctx->hz;
    }

    fprintf(fp, "flow_id,timestamp_count,pps,bps,timestamps_sec\n");

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

        fprintf(fp, "%" PRIu32 ",%" PRIu32 ",%.6f,%.6f,\"", record->flow_id, record->ts_count,
                pps, bps);
        for (j = 0; j < record->ts_count; j++) {
            double ts_sec = (double)(record->timestamps[j] - ctx->start_cycles) / (double)ctx->hz;
            fprintf(fp, "%.9f", ts_sec);
            if (j + 1U < record->ts_count) {
                fputc(';', fp);
            }
        }
        fprintf(fp, "\"\n");
    }

    fclose(fp);
    return 0;
}

static void cleanup_receiver(receiver_ctx_t *ctx) {
    uint32_t i = 0;

    if (rte_eth_dev_is_valid_port(ctx->cfg.port_id)) {
        rte_eth_dev_stop(ctx->cfg.port_id);
        rte_eth_dev_close(ctx->cfg.port_id);
    }

    if (ctx->records != NULL) {
        for (i = 0; i < ctx->records_cap; i++) {
            free(ctx->records[i].timestamps);
        }
        free(ctx->records);
    }
}

int main(int argc, char **argv) {
    receiver_ctx_t ctx;
    struct rte_mbuf *rx_pkts[MAX_RX_BURST];
    uint16_t burst = 0;
    int eal_argc = 0;
    int rc = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg.port_id = 0;
    ctx.cfg.rx_queue_id = 0;
    ctx.cfg.rx_burst_size = DEFAULT_RX_BURST;
    ctx.cfg.mempool_size = DEFAULT_MEMPOOL_SIZE;
    ctx.cfg.output_path = DEFAULT_OUTPUT_PATH;

    eal_argc = rte_eal_init(argc, argv);
    if (eal_argc < 0) {
        rte_exit(EXIT_FAILURE, "EAL init failed\n");
    }

    argc -= eal_argc;
    argv += eal_argc;
    optind = 1;

    if (parse_app_args(argc, argv, &ctx.cfg) != 0) {
        usage("receiver");
        rte_exit(EXIT_FAILURE, "Invalid app arguments\n");
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    ctx.hz = rte_get_timer_hz();
    ctx.start_cycles = rte_get_timer_cycles();

    ctx.mbuf_pool = rte_pktmbuf_pool_create("receiver_mbuf_pool", ctx.cfg.mempool_size,
                                            MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                            rte_socket_id());
    if (ctx.mbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "mempool create failed\n");
    }

    if (init_port(&ctx) != 0) {
        rte_exit(EXIT_FAILURE, "port init failed\n");
    }

    if (flow_table_init(&ctx) != 0) {
        rte_exit(EXIT_FAILURE, "flow table init failed\n");
    }

    burst = (uint16_t)ctx.cfg.rx_burst_size;
    if (burst > MAX_RX_BURST) {
        burst = MAX_RX_BURST;
    }

    while (!g_force_quit) {
        uint16_t nb_rx = rte_eth_rx_burst(ctx.cfg.port_id, ctx.cfg.rx_queue_id, rx_pkts, burst);
        uint16_t i = 0;

        if (nb_rx == 0U) {
            continue;
        }

        for (i = 0; i < nb_rx; i++) {
            if (process_one_packet(&ctx, rx_pkts[i]) != 0) {
                g_force_quit = 1;
                break;
            }
        }

        for (i = 0; i < nb_rx; i++) {
            rte_pktmbuf_free(rx_pkts[i]);
        }
    }

    rc = flush_flow_stats_to_csv(&ctx, rte_get_timer_cycles());
    if (rc != 0) {
        fprintf(stderr, "failed to write output CSV: %s\n", ctx.cfg.output_path);
    }

    cleanup_receiver(&ctx);
    return rc == 0 ? 0 : 1;
}
