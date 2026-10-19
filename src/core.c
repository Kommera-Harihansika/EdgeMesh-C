#include "edgemesh/core.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

static int token_char(unsigned char c)
{
    return c != 0 && (isalnum(c) || strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static int parse_line(const char *s, size_t n, em_request *out)
{
    const char *a = memchr(s, ' ', n);
    const char *b;
    size_t method_len, target_len;
    if (!a || a == s) return 400;
    method_len = (size_t)(a - s);
    for (size_t i = 0; i < method_len; ++i)
        if (!token_char((unsigned char)s[i])) return 400;
    if (method_len >= sizeof(out->method)) return 405;
    memcpy(out->method, s, method_len);
    out->method[method_len] = '\0';
    b = memchr(a + 1, ' ', n - method_len - 1);
    if (!b || b == a + 1) return 400;
    target_len = (size_t)(b - (a + 1));
    if (target_len > EM_MAX_TARGET) return 414;
    if (a[1] != '/') return 400; /* origin-form only */
    for (size_t i = 0; i < target_len; ++i)
        if ((unsigned char)a[1 + i] < 33 || (unsigned char)a[1 + i] == 127) return 400;
    if ((size_t)(s + n - (b + 1)) != 8 || memcmp(b + 1, "HTTP/1.1", 8)) return 400;
    memcpy(out->target, a + 1, target_len);
    out->target[target_len] = '\0';
    return 0;
}

int em_parse_request(const char *data, size_t length, em_request *out)
{
    const char *end, *line_end, *p;
    int status;
    int host_seen = 0, content_length_seen = 0;
    if (!data || !out || length == 0 || length > EM_MAX_HEADERS) return length > EM_MAX_HEADERS ? 431 : 400;
    memset(out, 0, sizeof(*out));
    end = data + length;
    line_end = NULL;
    for (p = data; p + 1 < end; ++p) {
        if (p[0] == '\r' && p[1] == '\n') { line_end = p; break; }
        if (p[0] == '\n' || (p[0] == '\r' && p[1] != '\n')) return 400;
    }
    if (!line_end) return 400;
    status = parse_line(data, (size_t)(line_end - data), out);
    if (status) return status;
    if (strcmp(out->method, "GET") != 0) return 405;
    p = line_end + 2;
    while (p + 1 < end) {
        const char *e = NULL, *colon;
        size_t n;
        for (const char *q = p; q + 1 < end; ++q) {
            if (q[0] == '\r' && q[1] == '\n') { e = q; break; }
            if (q[0] == '\n' || (q[0] == '\r' && q[1] != '\n')) return 400;
        }
        if (!e) return 400;
        if (e == p) return (e + 2 == end && host_seen == 1) ? 0 : 400;
        if (*p == ' ' || *p == '\t') return 400;
        colon = memchr(p, ':', (size_t)(e - p));
        if (!colon || colon == p) return 400;
        for (const char *q = p; q < colon; ++q)
            if (!token_char((unsigned char)*q)) return 400;
        n = (size_t)(e - (colon + 1));
        const char *v = colon + 1;
        while (n && (*v == ' ' || *v == '\t')) { ++v; --n; }
        while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) --n;
        for (size_t i = 0; i < n; ++i)
            if (((unsigned char)v[i] < 32 && v[i] != '\t') || (unsigned char)v[i] == 127) return 400;
        if ((size_t)(colon - p) == 4 && !strncasecmp(p, "Host", 4)) {
            if (host_seen++ || n == 0 || n >= sizeof(out->host)) return 400;
            memcpy(out->host, v, n); out->host[n] = '\0';
        } else if (((size_t)(colon - p) == 14 && !strncasecmp(p, "Content-Length", 14)) ||
                   ((size_t)(colon - p) == 17 && !strncasecmp(p, "Transfer-Encoding", 17))) {
            if ((size_t)(colon - p) == 14) {
                if (content_length_seen++ || n != 1 || *v != '0') return 400;
            } else return 400;
        }
        p = e + 2;
    }
    return 400;
}

int em_select_backend_excluding(em_backend backends[EM_BACKEND_COUNT], em_policy policy,
                                unsigned *round_robin_cursor, int excluded)
{
    int selected = -1;
    unsigned i;
    if (!backends || !round_robin_cursor) return -1;
    if (policy == EM_POLICY_ROUND_ROBIN) {
        for (i = 0; i < EM_BACKEND_COUNT; ++i) {
            unsigned idx = (*round_robin_cursor + i) % EM_BACKEND_COUNT;
            if ((int)idx != excluded && backends[idx].state == 1) { selected = (int)idx; break; }
        }
        if (selected >= 0) *round_robin_cursor = ((unsigned)selected + 1) % EM_BACKEND_COUNT;
    } else {
        unsigned min = (unsigned)-1;
        for (i = 0; i < EM_BACKEND_COUNT; ++i) {
            unsigned idx = (*round_robin_cursor + i) % EM_BACKEND_COUNT;
            if ((int)idx != excluded && backends[idx].state == 1 && backends[idx].active < min) {
                min = backends[idx].active; selected = (int)idx;
            }
        }
        if (selected >= 0) *round_robin_cursor = ((unsigned)selected + 1) % EM_BACKEND_COUNT;
    }
    if (selected >= 0) { ++backends[selected].active; ++backends[selected].requests; }
    return selected;
}

int em_select_backend(em_backend backends[EM_BACKEND_COUNT], em_policy policy,
                      unsigned *round_robin_cursor)
{
    return em_select_backend_excluding(backends, policy, round_robin_cursor, -1);
}

void em_release_backend(em_backend *backend)
{
    if (backend && backend->active) --backend->active;
}

int em_retry_allowed(unsigned attempts_already_made, int eligible_upstream_failure,
                     size_t response_bytes_written)
{
    return attempts_already_made == 0 && eligible_upstream_failure && response_bytes_written == 0;
}

int em_send_error_allowed(size_t response_bytes_written)
{
    return response_bytes_written == 0;
}
