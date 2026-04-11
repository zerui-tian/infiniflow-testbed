#include "switch/switch_ctx.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static char *trim(char *s) {
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

static int parse_u32_strict(const char *s, uint32_t *out) {
    char *end = NULL;
    unsigned long value = 0;

    errno = 0;
    value = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *trim(end) != '\0' || value > UINT32_MAX) {
        return -1;
    }

    *out = (uint32_t)value;
    return 0;
}

static int parse_u16_strict(const char *s, uint16_t *out) {
    uint32_t value = 0;

    if (parse_u32_strict(s, &value) != 0 || value > UINT16_MAX) {
        return -1;
    }

    *out = (uint16_t)value;
    return 0;
}

static int validate_route_header(char *line) {
    const char *expected[] = {"fid", "port"};
    char *saveptr = NULL;
    char *token = NULL;
    size_t i = 0;

    token = strtok_r(line, ",", &saveptr);
    while (token != NULL && i < (sizeof(expected) / sizeof(expected[0]))) {
        if (strcmp(trim(token), expected[i]) != 0) {
            return -1;
        }
        i++;
        token = strtok_r(NULL, ",", &saveptr);
    }

    if (i != (sizeof(expected) / sizeof(expected[0])) || token != NULL) {
        return -1;
    }

    return 0;
}

uint16_t switch_egress_index_from_port(const switch_ctx_t *ctx, uint16_t port_id) {
    uint16_t i = 0;

    for (i = 0; i < ctx->cfg.nb_egress_ports; i++) {
        if (ctx->cfg.egress_ports[i] == port_id) {
            return i;
        }
    }

    return UINT16_MAX;
}

uint16_t switch_ingress_index_from_port(const switch_ctx_t *ctx, uint16_t port_id) {
    uint16_t i = 0;

    for (i = 0; i < ctx->cfg.nb_ingress_ports; i++) {
        if (ctx->cfg.ingress_ports[i] == port_id) {
            return i;
        }
    }

    return UINT16_MAX;
}

void switch_route_table_reset(switch_ctx_t *ctx) {
    free(ctx->route_table.ports);
    ctx->route_table.ports = NULL;
    ctx->route_table.nb_entries = 0;
}

uint16_t switch_flow_map_lookup(const switch_ctx_t *ctx, uint32_t flow_id) {
    if (flow_id >= ctx->route_table.nb_entries || ctx->route_table.ports == NULL) {
        return UINT16_MAX;
    }

    return ctx->route_table.ports[flow_id];
}

int switch_route_table_load(switch_ctx_t *ctx) {
    FILE *fp = NULL;
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len = 0;
    uint16_t *ports = NULL;
    uint32_t nb_entries = 0;
    int rc = -1;

    if (ctx->cfg.route_csv_path[0] == '\0') {
        return -1;
    }

    fp = fopen(ctx->cfg.route_csv_path, "r");
    if (fp == NULL) {
        return -1;
    }

    line_len = getline(&line, &line_cap, fp);
    if (line_len <= 0 || validate_route_header(line) != 0) {
        goto out;
    }

    while ((line_len = getline(&line, &line_cap, fp)) > 0) {
        char *copy = NULL;
        char *saveptr = NULL;
        char *fid_col = NULL;
        char *port_col = NULL;
        uint32_t fid = 0;
        uint16_t port = 0;
        uint16_t *new_ports = NULL;
        uint32_t old_entries = 0;

        if (trim(line)[0] == '\0') {
            continue;
        }

        copy = strdup(line);
        if (copy == NULL) {
            goto out;
        }

        fid_col = strtok_r(copy, ",", &saveptr);
        port_col = strtok_r(NULL, ",", &saveptr);
        if (fid_col == NULL || port_col == NULL || strtok_r(NULL, ",", &saveptr) != NULL) {
            free(copy);
            goto out;
        }

        fid_col = trim(fid_col);
        port_col = trim(port_col);
        if (parse_u32_strict(fid_col, &fid) != 0 || parse_u16_strict(port_col, &port) != 0) {
            free(copy);
            goto out;
        }
        if (switch_egress_index_from_port(ctx, port) == UINT16_MAX) {
            free(copy);
            goto out;
        }

        if (fid >= nb_entries) {
            old_entries = nb_entries;
            new_ports = realloc(ports, ((size_t)fid + 1U) * sizeof(*ports));
            if (new_ports == NULL) {
                free(copy);
                goto out;
            }
            ports = new_ports;
            nb_entries = fid + 1U;
            while (old_entries < nb_entries) {
                ports[old_entries++] = UINT16_MAX;
            }
        }

        if (ports[fid] != UINT16_MAX) {
            free(copy);
            goto out;
        }

        ports[fid] = port;
        free(copy);
    }

    if (nb_entries == 0U) {
        goto out;
    }

    switch_route_table_reset(ctx);
    ctx->route_table.ports = ports;
    ctx->route_table.nb_entries = nb_entries;
    ports = NULL;
    rc = 0;

out:
    free(ports);
    free(line);
    if (fp != NULL) {
        fclose(fp);
    }
    return rc;
}
