#ifndef EDGEMESH_CORE_H
#define EDGEMESH_CORE_H

#include <stddef.h>
#include <stdint.h>

#define EM_MAX_TARGET 4096
#define EM_MAX_HEADERS 16384
#define EM_MAX_RESPONSE 65536
#define EM_BACKEND_COUNT 3

typedef enum {
    EM_POLICY_ROUND_ROBIN = 0,
    EM_POLICY_LEAST_CONNECTIONS = 1
} em_policy;

typedef struct {
    char method[32];
    char target[EM_MAX_TARGET + 1];
    char host[256];
} em_request;

typedef struct {
    char id[16];
    char address[32];
    unsigned short port;
    int state; /* 0 unknown, 1 healthy, 2 unhealthy */
    unsigned failures;
    unsigned successes;
    unsigned active;
    uint64_t requests;
    uint64_t failures_total;
    uint64_t responses;
    int64_t last_check;
    double last_response_ms;
} em_backend;

/* Returns an HTTP status to use on parse failure, or zero on success. */
int em_parse_request(const char *data, size_t length, em_request *out);

/* Selects a healthy backend and reserves one in-flight attempt; -1 if none. */
int em_select_backend(em_backend backends[EM_BACKEND_COUNT], em_policy policy,
                      unsigned *round_robin_cursor);
int em_select_backend_excluding(em_backend backends[EM_BACKEND_COUNT], em_policy policy,
                                unsigned *round_robin_cursor, int excluded);
void em_release_backend(em_backend *backend);
int em_retry_allowed(unsigned attempts_already_made, int eligible_upstream_failure,
                     size_t response_bytes_written);
int em_send_error_allowed(size_t response_bytes_written);
int em_validate_response(const char *data, size_t length);

#endif
