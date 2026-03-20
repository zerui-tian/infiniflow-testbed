#include "sender/sender_ctx.h"

#include <inttypes.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

#define DEFAULT_NB_VC 8U
#define DEFAULT_RING_SIZE 1024U
#define DEFAULT_PKT_SIZE 8000U
#define DEFAULT_MEMPOOL_SIZE 32768U
#define DEFAULT_TX_BURST 64U
#define DEFAULT_TICK_US 1000U
#define MBUF_CACHE_SIZE 256U

static volatile sig_atomic_t g_force_quit = 0;

static void handle_signal(int signum) {
    (void)signum;
    g_force_quit = 1;
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

static void usage(const char *prog) {
    printf("Usage: %s [EAL args] -- --csv <path> [options]\n", prog);
    printf("Options:\n");
    printf("  --csv <path>         Flow CSV file path (required)\n");
    printf("  --port <id>          NIC port id (default: 0)\n");
    printf("  --vcs <num>          Number of VC rings (default: 8)\n");
    printf("  --ring-size <num>    Ring size per VC (default: 1024)\n");
    printf("  --pkt-size <bytes>   Packet size in bytes (default: 8000)\n");
    printf("  --mempool <num>      Mempool object count (default: 32768)\n");
    printf("  --tx-burst <num>     TX burst size (default: 64)\n");
    printf("  --tick-us <num>      Producer tick interval us (default: 1000)\n");
}

static int parse_app_args(int argc, char **argv, sender_config_t *cfg) {
    static const struct option long_opts[] = {
        {"csv", required_argument, 0, 'c'},
        {"port", required_argument, 0, 'p'},
        {"vcs", required_argument, 0, 'v'},
        {"ring-size", required_argument, 0, 'r'},
        {"pkt-size", required_argument, 0, 's'},
        {"mempool", required_argument, 0, 'm'},
        {"tx-burst", required_argument, 0, 'b'},
        {"tick-us", required_argument, 0, 't'},
        {0, 0, 0, 0},
    };

    int opt = 0;

    while ((opt = getopt_long(argc, argv, "c:p:v:r:s:m:b:t:", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'c':
                cfg->csv_path = optarg;
                break;
            case 'p':
                if (parse_u16(optarg, &cfg->port_id) != 0) {
                    return -1;
                }
                break;
            case 'v':
                if (parse_u32(optarg, &cfg->nb_vc) != 0) {
                    return -1;
                }
                break;
            case 'r':
                if (parse_u32(optarg, &cfg->ring_size) != 0) {
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
            case 't':
                if (parse_u32(optarg, &cfg->tick_us) != 0) {
                    return -1;
                }
                break;
            default:
                return -1;
        }
    }

    if (cfg->csv_path == NULL || cfg->nb_vc == 0U || cfg->ring_size == 0U || cfg->packet_size == 0U ||
        cfg->tx_burst_size == 0U || cfg->tick_us == 0U) {
        return -1;
    }

    return 0;
}

static int init_port(uint16_t port_id) {
    struct rte_eth_conf port_conf;
    const uint16_t nb_rx_queue = 0;
    const uint16_t nb_tx_queue = 1;
    int rc = 0;

    memset(&port_conf, 0, sizeof(port_conf));
    port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

    rc = rte_eth_dev_configure(port_id, nb_rx_queue, nb_tx_queue, &port_conf);
    if (rc < 0) {
        return rc;
    }

    rc = rte_eth_tx_queue_setup(port_id, 0, 1024, rte_eth_dev_socket_id(port_id), NULL);
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

        snprintf(ring_name, sizeof(ring_name), "vc_ring_%u", i);
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

static int producer_loop(void *arg) {
    sender_ctx_t *ctx = (sender_ctx_t *)arg;

    while (!g_force_quit) {
        double now_sec = sender_now_sec(ctx);

        activity_manager_activate_ready(ctx, now_sec);
        scheduler_run_tick(ctx);
        activity_manager_compact(ctx);

        if (activity_manager_all_done(ctx)) {
            g_force_quit = 1;
            break;
        }

        usleep(ctx->cfg.tick_us);
    }

    return 0;
}

static int forward_loop(void *arg) {
    sender_ctx_t *ctx = (sender_ctx_t *)arg;

    while (!g_force_quit) {
        forward_run_tick(ctx);
    }

    /* Final drain to avoid leftovers in ring when producer exits. */
    while (forward_run_tick(ctx) > 0U) {
    }

    return 0;
}

static void cleanup_sender(sender_ctx_t *ctx) {
    uint32_t i = 0;

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

    free(ctx->vc_queues);
    free(ctx->active_ids);
    free(ctx->pending_order);
    free(ctx->flows);
}

int main(int argc, char **argv) {
    sender_ctx_t ctx;
    uint32_t max_vc_csv = 0;
    int eal_argc = 0;
    unsigned int producer_lcore = 0;
    unsigned int forward_lcore = 0;
    unsigned int lcore_id = 0;
    int rc = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg.port_id = 0;
    ctx.cfg.tx_queue_id = 0;
    ctx.cfg.nb_vc = DEFAULT_NB_VC;
    ctx.cfg.ring_size = DEFAULT_RING_SIZE;
    ctx.cfg.packet_size = DEFAULT_PKT_SIZE;
    ctx.cfg.mempool_size = DEFAULT_MEMPOOL_SIZE;
    ctx.cfg.tx_burst_size = DEFAULT_TX_BURST;
    ctx.cfg.tick_us = DEFAULT_TICK_US;

    eal_argc = rte_eal_init(argc, argv);
    if (eal_argc < 0) {
        rte_exit(EXIT_FAILURE, "EAL init failed\n");
    }

    argc -= eal_argc;
    argv += eal_argc;
    optind = 1;

    if (parse_app_args(argc, argv, &ctx.cfg) != 0) {
        usage("sender");
        rte_exit(EXIT_FAILURE, "Invalid app arguments\n");
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    rc = csv_loader_load(ctx.cfg.csv_path, &ctx.flows, &ctx.nb_flows, &max_vc_csv);
    if (rc != 0 || ctx.nb_flows == 0U) {
        rte_exit(EXIT_FAILURE, "CSV load failed or no flow found\n");
    }

    if (max_vc_csv >= ctx.cfg.nb_vc) {
        rte_exit(EXIT_FAILURE, "CSV vc id exceeds configured --vcs\n");
    }

    if (ctx.cfg.packet_size > RTE_MBUF_DEFAULT_BUF_SIZE - RTE_PKTMBUF_HEADROOM) {
        rte_exit(EXIT_FAILURE, "Packet size too large for default mbuf data room\n");
    }

    ctx.hz = rte_get_timer_hz();
    ctx.start_cycles = rte_get_timer_cycles();

    ctx.mbuf_pool =
        rte_pktmbuf_pool_create("sender_mbuf_pool", ctx.cfg.mempool_size, MBUF_CACHE_SIZE, 0,
                                RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (ctx.mbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "mempool create failed\n");
    }

    if (init_port(ctx.cfg.port_id) != 0) {
        rte_exit(EXIT_FAILURE, "port init failed\n");
    }

    if (init_vc_rings(&ctx) != 0) {
        rte_exit(EXIT_FAILURE, "ring init failed\n");
    }

    if (activity_manager_init(&ctx) != 0) {
        rte_exit(EXIT_FAILURE, "activity manager init failed\n");
    }

    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (producer_lcore == 0U) {
            producer_lcore = lcore_id;
        } else if (forward_lcore == 0U) {
            forward_lcore = lcore_id;
            break;
        }
    }

    if (producer_lcore == 0U || forward_lcore == 0U) {
        rte_exit(EXIT_FAILURE, "Need at least 2 worker lcores for producer/forward\n");
    }

    rte_eal_remote_launch(producer_loop, &ctx, producer_lcore);
    rte_eal_remote_launch(forward_loop, &ctx, forward_lcore);

    rte_eal_wait_lcore(producer_lcore);
    rte_eal_wait_lcore(forward_lcore);

    printf("Sender completed. enqueued=%" PRIu64 ", tx=%" PRIu64 "\n", ctx.total_pkts_enqueued,
           ctx.total_pkts_tx);

    cleanup_sender(&ctx);
    return 0;
}
