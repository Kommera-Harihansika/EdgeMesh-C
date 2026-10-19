#ifndef EDGEMESH_APP_H
#define EDGEMESH_APP_H

#include "edgemesh/core.h"
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>

#define EM_PROXY_PORT 8080
#define EM_ADMIN_PORT 9090
#define EM_MAX_PROXY_CONNECTIONS 32
#define EM_MAX_ADMIN_CONNECTIONS 8
#define EM_HANDLER_STACK_SIZE (512 * 1024)
#define EM_UPSTREAM_CONNECT_MS 1000
#define EM_UPSTREAM_READ_MS 2000
#define EM_CLIENT_HEADER_MS 2000
#define EM_REQUEST_DEADLINE_MS 5000
#define EM_HEALTH_INTERVAL_MS 1000
#define EM_HEALTH_TIMEOUT_MS 250
#define EM_METRIC_BUCKETS 300
#define EM_LATENCY_SAMPLES 300

typedef struct {
    int64_t second;
    uint64_t requests;
    uint64_t responses;
    uint64_t failures;
    uint64_t bytes;
    uint64_t client_rejections;
    uint64_t capacity_rejections;
    uint64_t cumulative_requests;
    uint64_t cumulative_responses;
    uint64_t cumulative_failures;
    uint64_t cumulative_client_rejections;
    uint64_t cumulative_capacity_rejections;
} em_bucket;

typedef struct { int64_t second; double ms; } em_latency_sample;

typedef struct {
    pthread_mutex_t mutex;
    uint64_t requests;
    uint64_t responses;
    uint64_t failures;
    uint64_t client_rejections;
    uint64_t capacity_rejections;
    uint64_t bytes;
    unsigned active;
    em_bucket buckets[EM_METRIC_BUCKETS];
    em_latency_sample latency[EM_LATENCY_SAMPLES];
    unsigned latency_count;
    unsigned latency_next;
} em_metrics;

extern pthread_mutex_t em_state_mutex;
extern em_backend em_backends[EM_BACKEND_COUNT];
extern em_policy em_current_policy;
extern unsigned em_selection_cursor;
extern em_metrics em_global_metrics;
extern volatile sig_atomic_t em_stopping;
extern time_t em_started_at;
extern int em_proxy_listener;
extern int em_admin_listener;
extern unsigned em_proxy_connections;
extern unsigned em_admin_connections;

int64_t em_now_ms(void);
void em_record_rejection(int capacity);
void em_begin_request(void);
void em_finish_request(int success, uint64_t bytes, double latency_ms);
void em_add_response_bytes(uint64_t bytes);
void em_set_active(int delta);
void em_format_overview(char *out, size_t capacity);
void em_format_backends(char *out, size_t capacity);
void em_format_metrics(char *out, size_t capacity, unsigned window_seconds);
void *em_proxy_listener_main(void *unused);
void *em_admin_listener_main(void *unused);
void *em_health_main(void *unused);
void em_send_http_error(int fd, int status, const char *message);

#endif
