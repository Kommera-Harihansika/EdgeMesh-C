#include "edgemesh/app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static em_bucket *bucket_at(int64_t second)
{
    em_bucket *b = &em_global_metrics.buckets[(uint64_t)second % EM_METRIC_BUCKETS];
    if (b->second != second) {
        memset(b, 0, sizeof(*b)); b->second = second;
        b->cumulative_requests = em_global_metrics.requests;
        b->cumulative_responses = em_global_metrics.responses;
        b->cumulative_failures = em_global_metrics.failures;
        b->cumulative_client_rejections = em_global_metrics.client_rejections;
        b->cumulative_capacity_rejections = em_global_metrics.capacity_rejections;
    }
    return b;
}

static void snapshot_totals(em_bucket *b)
{
    b->cumulative_requests = em_global_metrics.requests;
    b->cumulative_responses = em_global_metrics.responses;
    b->cumulative_failures = em_global_metrics.failures;
    b->cumulative_client_rejections = em_global_metrics.client_rejections;
    b->cumulative_capacity_rejections = em_global_metrics.capacity_rejections;
}

void em_record_rejection(int capacity)
{
    int64_t second = (int64_t)time(NULL);
    pthread_mutex_lock(&em_global_metrics.mutex);
    em_bucket *b = bucket_at(second);
    if (capacity) { ++em_global_metrics.capacity_rejections; ++b->capacity_rejections; }
    else { ++em_global_metrics.client_rejections; ++b->client_rejections; }
    snapshot_totals(b);
    pthread_mutex_unlock(&em_global_metrics.mutex);
}

void em_begin_request(void)
{
    int64_t second = (int64_t)time(NULL);
    pthread_mutex_lock(&em_global_metrics.mutex);
    ++em_global_metrics.requests; ++em_global_metrics.active;
    em_bucket *b = bucket_at(second); ++b->requests; snapshot_totals(b);
    pthread_mutex_unlock(&em_global_metrics.mutex);
}

void em_set_active(int delta)
{
    pthread_mutex_lock(&em_global_metrics.mutex);
    if (delta > 0) em_global_metrics.active += (unsigned)delta;
    else if (em_global_metrics.active) --em_global_metrics.active;
    pthread_mutex_unlock(&em_global_metrics.mutex);
}

void em_add_response_bytes(uint64_t bytes)
{
    int64_t second = (int64_t)time(NULL);
    pthread_mutex_lock(&em_global_metrics.mutex);
    em_global_metrics.bytes += bytes;
    em_bucket *b = bucket_at(second); b->bytes += bytes; snapshot_totals(b);
    pthread_mutex_unlock(&em_global_metrics.mutex);
}

void em_finish_request(int success, uint64_t bytes, double latency_ms)
{
    int64_t second = (int64_t)time(NULL);
    (void)bytes;
    pthread_mutex_lock(&em_global_metrics.mutex);
    em_bucket *b = bucket_at(second);
    if (success) {
        ++em_global_metrics.responses; ++b->responses;
        em_global_metrics.latency[em_global_metrics.latency_next].second = second;
        em_global_metrics.latency[em_global_metrics.latency_next].ms = latency_ms;
        em_global_metrics.latency_next = (em_global_metrics.latency_next + 1) % EM_LATENCY_SAMPLES;
        if (em_global_metrics.latency_count < EM_LATENCY_SAMPLES) ++em_global_metrics.latency_count;
    } else { ++em_global_metrics.failures; ++b->failures; }
    if (em_global_metrics.active) --em_global_metrics.active;
    snapshot_totals(b);
    pthread_mutex_unlock(&em_global_metrics.mutex);
}

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int quantiles(int64_t from, int64_t to, double *p50, double *p95, double *p99)
{
    double values[EM_LATENCY_SAMPLES]; unsigned n = 0;
    for (unsigned i = 0; i < em_global_metrics.latency_count; ++i) {
        em_latency_sample s = em_global_metrics.latency[i];
        if (s.second >= from && s.second <= to) values[n++] = s.ms;
    }
    if (!n) return 0;
    qsort(values, n, sizeof(values[0]), compare_double);
    *p50 = values[(n - 1) * 50 / 100];
    *p95 = values[(n - 1) * 95 / 100];
    *p99 = values[(n - 1) * 99 / 100];
    return 1;
}

static void utc_string(int64_t second, char out[32])
{
    time_t t = (time_t)second; struct tm tm_value;
    if (!gmtime_r(&t, &tm_value)) { strcpy(out, "1970-01-01T00:00:00Z"); return; }
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm_value);
}

static void number_or_null(char out[32], int have, double value)
{
    if (have) snprintf(out, 32, "%.3f", value); else strcpy(out, "null");
}

void em_format_overview(char *out, size_t capacity)
{
    unsigned healthy = 0, unhealthy = 0, unknown = 0, active;
    uint64_t requests, responses, failures, client_rejects, capacity_rejects, bytes = 0, window_requests = 0;
    int64_t now = (int64_t)time(NULL); double p50 = 0, p95 = 0, p99 = 0;
    char q50[32], q95[32], q99[32], stamp[32];
    pthread_mutex_lock(&em_state_mutex);
    for (unsigned i = 0; i < EM_BACKEND_COUNT; ++i) {
        if (em_backends[i].state == 1) ++healthy;
        else if (em_backends[i].state == 2) ++unhealthy;
        else ++unknown;
    }
    em_policy policy = em_current_policy;
    pthread_mutex_unlock(&em_state_mutex);
    pthread_mutex_lock(&em_global_metrics.mutex);
    requests = em_global_metrics.requests; responses = em_global_metrics.responses;
    failures = em_global_metrics.failures; active = em_global_metrics.active;
    client_rejects = em_global_metrics.client_rejections;
    capacity_rejects = em_global_metrics.capacity_rejections;
    for (int64_t s = now - 59; s <= now; ++s) {
        em_bucket *b = &em_global_metrics.buckets[(uint64_t)s % EM_METRIC_BUCKETS];
        if (b->second == s) { bytes += b->bytes; window_requests += b->requests; }
    }
    int have = quantiles(now - 59, now, &p50, &p95, &p99);
    pthread_mutex_unlock(&em_global_metrics.mutex);
    double denominator = (double)(now - (int64_t)em_started_at + 1);
    if (denominator < 1.0) denominator = 1.0;
    if (denominator > 60.0) denominator = 60.0;
    number_or_null(q50, have, p50); number_or_null(q95, have, p95); number_or_null(q99, have, p99); utc_string(now, stamp);
    snprintf(out, capacity,
        "{\"proxy_running\":true,\"policy\":\"%s\",\"uptime_seconds\":%ld,"
        "\"backends\":{\"total\":3,\"healthy\":%u,\"unhealthy\":%u,\"unknown\":%u},"
        "\"metrics\":{\"requests_total\":%llu,\"responses_forwarded_total\":%llu,"
        "\"proxy_failures_total\":%llu,\"client_rejections_total\":%llu,\"capacity_rejections_total\":%llu,"
        "\"requests_per_second\":%.3f,\"response_bytes_per_second\":%.3f,\"active_requests\":%u,"
        "\"latency_p50_ms\":%s,\"latency_p95_ms\":%s,\"latency_p99_ms\":%s},"
        "\"sample_window_seconds\":60,\"generated_at\":\"%s\"}",
        policy == EM_POLICY_ROUND_ROBIN ? "round_robin" : "least_connections",
        (long)(time(NULL) - em_started_at), healthy, unhealthy, unknown,
        (unsigned long long)requests, (unsigned long long)responses, (unsigned long long)failures,
        (unsigned long long)client_rejects, (unsigned long long)capacity_rejects,
        (double)window_requests / denominator, (double)bytes / denominator, active, q50, q95, q99, stamp);
}

void em_format_backends(char *out, size_t capacity)
{
    size_t used = 0; int64_t now = (int64_t)time(NULL); char stamp[32]; utc_string(now, stamp);
    used += (size_t)snprintf(out + used, capacity - used, "{\"backends\":[");
    pthread_mutex_lock(&em_state_mutex);
    for (unsigned i = 0; i < EM_BACKEND_COUNT; ++i) {
        em_backend b = em_backends[i]; char check[32], response[32];
        if (i) used += (size_t)snprintf(out + used, capacity - used, ",");
        if (b.last_check) { utc_string(b.last_check, check); snprintf(response, sizeof(response), "%.3f", b.last_response_ms); }
        else { strcpy(check, "null"); strcpy(response, "null"); }
        char check_json[40];
        if (b.last_check) snprintf(check_json, sizeof(check_json), "\"%s\"", check);
        else strcpy(check_json, "null");
        used += (size_t)snprintf(out + used, capacity - used,
            "{\"id\":\"%s\",\"address\":\"%s:%u\",\"state\":\"%s\","
            "\"active_connections\":%u,\"requests_total\":%llu,\"responses_forwarded_total\":%llu,"
            "\"proxy_failures_total\":%llu,\"last_check_at\":%s,\"last_response_ms\":%s}",
            b.id, b.address, b.port, b.state == 1 ? "healthy" : b.state == 2 ? "unhealthy" : "unknown",
            b.active, (unsigned long long)b.requests, (unsigned long long)b.responses,
            (unsigned long long)b.failures_total, check_json, response);
    }
    pthread_mutex_unlock(&em_state_mutex);
    snprintf(out + used, capacity - used, "],\"generated_at\":\"%s\"}", stamp);
}

void em_format_metrics(char *out, size_t capacity, unsigned window_seconds)
{
    size_t used = 0; int64_t now = (int64_t)time(NULL); unsigned emitted = 0;
    pthread_mutex_lock(&em_global_metrics.mutex);
    used += (size_t)snprintf(out + used, capacity - used, "{\"window_seconds\":%u,\"points\":[", window_seconds);
    for (int64_t s = now - (int64_t)window_seconds + 1; s <= now; ++s) {
        em_bucket b = em_global_metrics.buckets[(uint64_t)s % EM_METRIC_BUCKETS];
        if (b.second != s) continue;
        double p50 = 0, p95 = 0, p99 = 0; int have = quantiles(s, s, &p50, &p95, &p99);
        char ts[32], q50[32], q95[32], q99[32]; utc_string(s, ts);
        number_or_null(q50, have, p50); number_or_null(q95, have, p95); number_or_null(q99, have, p99);
        if (emitted++) used += (size_t)snprintf(out + used, capacity - used, ",");
        used += (size_t)snprintf(out + used, capacity - used,
            "{\"timestamp\":\"%s\",\"requests_total\":%llu,\"responses_forwarded_total\":%llu,"
            "\"proxy_failures_total\":%llu,\"client_rejections_total\":%llu,\"capacity_rejections_total\":%llu,"
            "\"requests_per_second\":%llu,\"response_bytes_per_second\":%llu,"
            "\"latency_p50_ms\":%s,\"latency_p95_ms\":%s,\"latency_p99_ms\":%s}",
            ts, (unsigned long long)b.cumulative_requests, (unsigned long long)b.cumulative_responses,
            (unsigned long long)b.cumulative_failures, (unsigned long long)b.cumulative_client_rejections,
            (unsigned long long)b.cumulative_capacity_rejections, (unsigned long long)b.requests,
            (unsigned long long)b.bytes, q50, q95, q99);
    }
    pthread_mutex_unlock(&em_global_metrics.mutex);
    char stamp[32]; utc_string(now, stamp);
    snprintf(out + used, capacity - used, "],\"generated_at\":\"%s\"}", stamp);
}
