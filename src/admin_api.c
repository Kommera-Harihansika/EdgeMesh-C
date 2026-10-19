#include "edgemesh/app.h"
#include "edgemesh/net.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define ADMIN_BUFFER (EM_MAX_HEADERS + 4096 + 1)

typedef struct { int fd; } admin_arg;

static void close_admin_slot(int fd)
{
    close(fd);
    pthread_mutex_lock(&em_state_mutex);
    if (em_admin_connections) --em_admin_connections;
    pthread_mutex_unlock(&em_state_mutex);
}

static int start_admin_handler(pthread_t *thread, void *(*entry)(void *), void *arg)
{
    pthread_attr_t attr; int rc = pthread_attr_init(&attr);
    if (rc) return rc;
    rc = pthread_attr_setstacksize(&attr, EM_HANDLER_STACK_SIZE);
    if (!rc) rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (!rc) rc = pthread_create(thread, &attr, entry, arg);
    pthread_attr_destroy(&attr);
    return rc;
}

static char *find_header_end(char *data, size_t length)
{
    for (size_t i = 0; i + 3 < length; ++i)
        if (!memcmp(data + i, "\r\n\r\n", 4)) return data + i;
    return NULL;
}

static int read_more(int fd, char *data, size_t *used, size_t needed)
{
    int64_t deadline = em_now_ms() + EM_CLIENT_HEADER_MS;
    while (*used < needed) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int left = (int)(deadline - em_now_ms());
        if (left <= 0 || poll(&pfd, 1, left) <= 0) return -1;
        if (!(pfd.revents & POLLIN) && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) return -1;
        ssize_t n = recv(fd, data + *used, needed - *used, 0);
        if (n <= 0) return -1;
        *used += (size_t)n;
    }
    return 0;
}

static int parse_admin(int fd, char *buffer, size_t length, char method[8], char target[256], char body[4097])
{
    char *end = find_header_end(buffer, length), *line_end, *p;
    size_t header_length, body_length = 0, used = length;
    int length_seen = 0;
    char version[16], extra;
    if (!end) return 431;
    header_length = (size_t)(end + 4 - buffer);
    if (header_length > EM_MAX_HEADERS) return 431;
    buffer[length] = '\0';
    line_end = strstr(buffer, "\r\n");
    if (!line_end) return 400;
    *line_end = '\0';
    if (sscanf(buffer, "%7s %255s %15s %c", method, target, version, &extra) != 3 || strcmp(version, "HTTP/1.1")) return 400;
    *line_end = '\r';
    p = line_end + 2;
    while (p < end) {
        char *e = strstr(p, "\r\n"), *colon;
        if (!e || e == p) break;
        colon = memchr(p, ':', (size_t)(e - p));
        if (!colon) return 400;
        if ((size_t)(colon - p) == 14 && !strncasecmp(p, "Content-Length", 14)) {
            char *tail; unsigned long n = strtoul(colon + 1, &tail, 10);
            while (*tail == ' ' || *tail == '\t') ++tail;
            if (tail != e || n > 4096 || length_seen++) return n > 4096 ? 413 : 400;
            body_length = (size_t)n;
        }
        p = e + 2;
    }
    size_t body_at = header_length;
    if (used < body_at + body_length) {
        if (read_more(fd, buffer, &used, body_at + body_length)) return 400;
    }
    if (used != body_at + body_length) return 400;
    if (body_length) { memcpy(body, buffer + body_at, body_length); body[body_length] = '\0'; }
    else body[0] = '\0';
    return 0;
}

static int allowed_origin(const char *origin)
{
    return origin && (!strcmp(origin, "http://localhost:5173") || !strcmp(origin, "http://127.0.0.1:5173"));
}

static void send_admin(int fd, int status, const char *body, const char *origin, int options)
{
    char headers[1024]; size_t written; const char *phrase = status == 200 ? "OK" : status == 204 ? "No Content" : status == 400 ? "Bad Request" : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : status == 413 ? "Payload Too Large" : status == 431 ? "Request Header Fields Too Large" : "Internal Server Error";
    int n = snprintf(headers, sizeof(headers), "HTTP/1.1 %d %s\r\nConnection: close\r\n%s%s%s%s%sContent-Type: application/json; charset=utf-8\r\nContent-Length: %lu\r\n\r\n",
        status, phrase,
        allowed_origin(origin) ? "Access-Control-Allow-Origin: " : "",
        allowed_origin(origin) ? origin : "",
        allowed_origin(origin) ? "\r\nVary: Origin\r\n" : "",
        options ? "Access-Control-Allow-Methods: GET, PUT, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\n" : "",
        "", (unsigned long)(options ? 0 : strlen(body)));
    if (n > 0 && !em_write_all(fd, headers, (size_t)n, &written) && !options && body[0])
        em_write_all(fd, body, strlen(body), &written);
}

static int parse_policy(const char *json, em_policy *out)
{
    const char *p = json;
    while (isspace((unsigned char)*p)) ++p;
    if (*p++ != '{') return -1;
    while (isspace((unsigned char)*p)) ++p;
    if (*p++ != '"' || strncmp(p, "policy", 6)) return -1;
    p += 6; if (*p++ != '"') return -1;
    while (isspace((unsigned char)*p)) ++p;
    if (*p++ != ':') return -1;
    while (isspace((unsigned char)*p)) ++p;
    if (*p++ != '"') return -1;
    if (!strncmp(p, "round_robin", 11) && p[11] == '"') { *out = EM_POLICY_ROUND_ROBIN; p += 11; }
    else if (!strncmp(p, "least_connections", 17) && p[17] == '"') { *out = EM_POLICY_LEAST_CONNECTIONS; p += 17; }
    else return -1;
    ++p; while (isspace((unsigned char)*p)) ++p;
    if (*p++ != '}') return -1;
    while (isspace((unsigned char)*p)) ++p;
    return *p ? -1 : 0;
}

static void send_json_error(int fd, int status, const char *code, const char *message, const char *origin)
{
    char body[512]; snprintf(body, sizeof(body), "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}", code, message);
    fprintf(stderr, "{\"event\":\"admin_error\",\"status\":%d,\"code\":\"%s\"}\n", status, code);
    send_admin(fd, status, body, origin, 0);
}

static void *handle_admin(void *value)
{
    admin_arg *arg = value; int fd = arg->fd; free(arg);
    char buffer[ADMIN_BUFFER], method[8] = {0}, target[256] = {0}, body[4097], origin[128] = {0};
    ssize_t n; size_t length; char *end;
    em_set_send_timeout(fd, 2000);
    n = em_read_headers(fd, buffer, sizeof(buffer) - 1, EM_CLIENT_HEADER_MS);
    if (n < 0) { send_json_error(fd, n == -2 ? 431 : 400, "BAD_REQUEST", "invalid or oversized request", NULL); close_admin_slot(fd); return NULL; }
    length = (size_t)n;
    buffer[length] = '\0';
    end = find_header_end(buffer, length);
    if (end) {
        size_t hlen = (size_t)(end + 4 - buffer);
        if (hlen > EM_MAX_HEADERS) { send_json_error(fd, 431, "HEADERS_TOO_LARGE", "request headers exceed 16 KiB", NULL); close_admin_slot(fd); return NULL; }
        for (char *p = strstr(buffer, "\r\n"); p && p < end; ) {
            p += 2; char *e = strstr(p, "\r\n"); if (!e || e == p) break;
            char *colon = memchr(p, ':', (size_t)(e - p));
            if (colon && (size_t)(colon - p) == 6 && !strncasecmp(p, "Origin", 6)) {
                char *v = colon + 1; while (v < e && (*v == ' ' || *v == '\t')) ++v;
                size_t z = (size_t)(e - v); if (z < sizeof(origin)) { memcpy(origin, v, z); origin[z] = '\0'; }
                break;
            }
            p = e;
        }
    }
    int status = parse_admin(fd, buffer, length, method, target, body);
    if (status) { send_json_error(fd, status, status == 413 ? "BODY_TOO_LARGE" : "BAD_REQUEST", "invalid admin request", origin); close_admin_slot(fd); return NULL; }
    int known_route = !strcmp(target, "/api/v1/health") || !strcmp(target, "/api/v1/overview") ||
        !strcmp(target, "/api/v1/backends") || !strcmp(target, "/api/v1/policy") ||
        !strcmp(target, "/api/v1/benchmarks/latest") ||
        (!strncmp(target, "/api/v1/metrics", 15) && (target[15] == '\0' || target[15] == '?'));
    if (!known_route) { send_json_error(fd, 404, "NOT_FOUND", "route not found", origin); close_admin_slot(fd); return NULL; }
    if (strcmp(method, "OPTIONS") && strcmp(method, "GET") && strcmp(method, "PUT")) { send_json_error(fd, 405, "METHOD_NOT_ALLOWED", "method is not supported", origin); close_admin_slot(fd); return NULL; }
    if (!strcmp(method, "OPTIONS")) { send_admin(fd, 204, "", origin, 1); close_admin_slot(fd); return NULL; }
    int method_matches = (!strcmp(target, "/api/v1/policy") ? !strcmp(method, "PUT") : !strcmp(method, "GET"));
    if (known_route && !method_matches) { send_json_error(fd, 405, "METHOD_NOT_ALLOWED", "method is not supported for this route", origin); close_admin_slot(fd); return NULL; }
    char response[131072]; response[0] = '\0';
    if (!strcmp(target, "/api/v1/health") && !strcmp(method, "GET")) {
        snprintf(response, sizeof(response), "{\"status\":\"ok\",\"service\":\"edgemesh-c\",\"version\":\"0.1.0\",\"uptime_seconds\":%ld}", (long)(time(NULL) - em_started_at));
    } else if (!strcmp(target, "/api/v1/overview") && !strcmp(method, "GET")) em_format_overview(response, sizeof(response));
    else if (!strcmp(target, "/api/v1/backends") && !strcmp(method, "GET")) em_format_backends(response, sizeof(response));
    else if (!strcmp(target, "/api/v1/policy") && !strcmp(method, "PUT")) {
        em_policy policy;
        if (parse_policy(body, &policy)) { send_json_error(fd, 400, "INVALID_POLICY", "policy must be round_robin or least_connections", origin); close_admin_slot(fd); return NULL; }
        pthread_mutex_lock(&em_state_mutex); em_current_policy = policy; pthread_mutex_unlock(&em_state_mutex);
        snprintf(response, sizeof(response), "{\"policy\":\"%s\",\"updated\":true}", policy == EM_POLICY_ROUND_ROBIN ? "round_robin" : "least_connections");
    } else if (!strncmp(target, "/api/v1/metrics", 15) && (target[15] == '\0' || target[15] == '?') && !strcmp(method, "GET")) {
        unsigned window = 60;
        const char *q = strchr(target, '?');
        if (q) {
            if (strncmp(q, "?window_seconds=", 16)) { send_json_error(fd, 400, "INVALID_WINDOW", "window_seconds must be between 10 and 300", origin); close_admin_slot(fd); return NULL; }
            char *tail; unsigned long v = strtoul(q + 16, &tail, 10);
            if (tail == q + 16 || *tail || v < 10 || v > 300) { send_json_error(fd, 400, "INVALID_WINDOW", "window_seconds must be between 10 and 300", origin); close_admin_slot(fd); return NULL; }
            window = (unsigned)v;
        }
        em_format_metrics(response, sizeof(response), window);
    } else if (!strcmp(target, "/api/v1/benchmarks/latest") && !strcmp(method, "GET")) {
        snprintf(response, sizeof(response), "{\"available\":false,\"run\":null}");
    } else {
        send_json_error(fd, 404, "NOT_FOUND", "route not found", origin); close_admin_slot(fd); return NULL;
    }
    send_admin(fd, 200, response, origin, 0);
    close_admin_slot(fd); return NULL;
}

void *em_admin_listener_main(void *unused)
{
    (void)unused;
    while (!em_stopping) {
        struct pollfd listener = { em_admin_listener, POLLIN, 0 };
        int ready = poll(&listener, 1, 500);
        if (ready < 0) { if (errno == EINTR) continue; if (em_stopping) break; continue; }
        if (!ready || !(listener.revents & POLLIN)) continue;
        int fd = accept(em_admin_listener, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; if (em_stopping) break; continue; }
        pthread_mutex_lock(&em_state_mutex);
        int full = em_admin_connections >= EM_MAX_ADMIN_CONNECTIONS;
        if (!full) ++em_admin_connections;
        pthread_mutex_unlock(&em_state_mutex);
        if (full) {
            em_record_rejection(1);
            static const char body[] = "{\"error\":{\"code\":\"CAPACITY_REACHED\",\"message\":\"admin connection limit reached\"}}";
            char response[256]; size_t ignored;
            int n = snprintf(response, sizeof(response), "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n%s", (unsigned long)(sizeof(body) - 1), body);
            fprintf(stderr, "{\"event\":\"capacity_rejection\",\"listener\":\"admin\"}\n");
            if (n > 0) em_write_all(fd, response, (size_t)n, &ignored);
            close(fd); continue;
        }
        admin_arg *arg = malloc(sizeof(*arg)); pthread_t thread;
        if (!arg) { pthread_mutex_lock(&em_state_mutex); --em_admin_connections; pthread_mutex_unlock(&em_state_mutex); close(fd); continue; }
        arg->fd = fd;
        if (start_admin_handler(&thread, handle_admin, arg)) { free(arg); close(fd); pthread_mutex_lock(&em_state_mutex); --em_admin_connections; pthread_mutex_unlock(&em_state_mutex); continue; }
    }
    return NULL;
}
