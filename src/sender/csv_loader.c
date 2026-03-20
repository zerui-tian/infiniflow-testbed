#include "sender/sender_ctx.h"

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

static int parse_u32(const char *s, uint32_t *out) {
    char *end = NULL;
    unsigned long value = 0;

    errno = 0;
    value = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *trim(end) != '\0') {
        return -1;
    }
    if (value > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

static int parse_double(const char *s, double *out) {
    char *end = NULL;
    double value = 0.0;

    errno = 0;
    value = strtod(s, &end);
    if (errno != 0 || end == s || *trim(end) != '\0') {
        return -1;
    }
    *out = value;
    return 0;
}

static int validate_header(char *line) {
    const char *expected[] = {"fid", "vc", "len", "stime", "dest"};
    char *saveptr = NULL;
    char *token = NULL;
    int i = 0;

    token = strtok_r(line, ",", &saveptr);
    while (token != NULL && i < 5) {
        if (strcmp(trim(token), expected[i]) != 0) {
            return -1;
        }
        i++;
        token = strtok_r(NULL, ",", &saveptr);
    }
    if (i != 5 || token != NULL) {
        return -1;
    }
    return 0;
}

int csv_loader_load(const char *path, flow_desc_t **flows_out, uint32_t *nb_flows_out,
                    uint32_t *max_vc_out) {
    FILE *fp = NULL;
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len = 0;
    flow_desc_t *flows = NULL;
    uint32_t cap = 0;
    uint32_t count = 0;
    uint32_t max_vc = 0;
    int rc = -1;

    if (path == NULL || flows_out == NULL || nb_flows_out == NULL || max_vc_out == NULL) {
        return -1;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }

    line_len = getline(&line, &line_cap, fp);
    if (line_len <= 0 || validate_header(line) != 0) {
        goto out;
    }

    while ((line_len = getline(&line, &line_cap, fp)) > 0) {
        char *copy = NULL;
        char *saveptr = NULL;
        char *cols[5] = {0};
        uint32_t fid = 0;
        uint32_t vc = 0;
        uint32_t len = 0;
        uint32_t dest = 0;
        double stime_sec = 0.0;
        int i = 0;

        if (trim(line)[0] == '\0') {
            continue;
        }

        copy = strdup(line);
        if (copy == NULL) {
            goto out;
        }

        cols[0] = strtok_r(copy, ",", &saveptr);
        for (i = 1; i < 5; i++) {
            cols[i] = strtok_r(NULL, ",", &saveptr);
        }
        if (strtok_r(NULL, ",", &saveptr) != NULL) {
            free(copy);
            goto out;
        }
        for (i = 0; i < 5; i++) {
            if (cols[i] == NULL) {
                free(copy);
                goto out;
            }
            cols[i] = trim(cols[i]);
        }

        if (parse_u32(cols[0], &fid) != 0 || parse_u32(cols[1], &vc) != 0 ||
            parse_u32(cols[2], &len) != 0 || parse_double(cols[3], &stime_sec) != 0 ||
            parse_u32(cols[4], &dest) != 0) {
            free(copy);
            goto out;
        }
        if (len == 0 || stime_sec < 0.0) {
            free(copy);
            goto out;
        }

        if (count == cap) {
            uint32_t new_cap = (cap == 0) ? 64U : cap * 2U;
            flow_desc_t *new_flows = realloc(flows, new_cap * sizeof(*flows));
            if (new_flows == NULL) {
                free(copy);
                goto out;
            }
            flows = new_flows;
            cap = new_cap;
        }

        flows[count].fid = fid;
        flows[count].vc = vc;
        flows[count].len = len;
        flows[count].stime_sec = stime_sec;
        flows[count].dest = dest;
        flows[count].sent_count = 0;
        flows[count].state = FLOW_STATE_PENDING;
        if (vc > max_vc) {
            max_vc = vc;
        }
        count++;
        free(copy);
    }

    *flows_out = flows;
    *nb_flows_out = count;
    *max_vc_out = max_vc;
    flows = NULL;
    rc = 0;

out:
    free(flows);
    free(line);
    if (fp != NULL) {
        fclose(fp);
    }
    return rc;
}
