#include "edgemesh/core.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse(const char *text, em_request *request)
{
    return em_parse_request(text, strlen(text), request);
}

int main(void)
{
    em_request request;
    const char *valid = "GET /demo?q=1 HTTP/1.1\r\nHost: localhost\r\nAccept: application/json\r\n\r\n";
    assert(parse(valid, &request) == 0);
    assert(!strcmp(request.method, "GET"));
    assert(!strcmp(request.target, "/demo?q=1"));
    assert(!strcmp(request.host, "localhost"));
    assert(parse("GET / HTTP/1.1\r\n\r\n", &request) == 400);
    assert(parse("GET / HTTP/1.0\r\nHost: x\r\n\r\n", &request) == 400);
    assert(parse("POST / HTTP/1.1\r\nHost: x\r\n\r\n", &request) == 405);
    assert(parse("GET http://example.test/ HTTP/1.1\r\nHost: x\r\n\r\n", &request) == 400);
    assert(parse("GET / HTTP/1.1\nHost: x\n\n", &request) == 400);
    assert(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\n", &request) == 400);
    assert(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n", &request) == 400);

    char long_target[EM_MAX_TARGET + 128];
    size_t prefix = (size_t)sprintf(long_target, "GET /");
    memset(long_target + prefix, 'a', EM_MAX_TARGET + 1);
    prefix += EM_MAX_TARGET + 1;
    prefix += (size_t)sprintf(long_target + prefix, " HTTP/1.1\r\nHost: x\r\n\r\n");
    assert(em_parse_request(long_target, prefix, &request) == 414);
    char large_headers[EM_MAX_HEADERS + 1]; memset(large_headers, 'x', sizeof(large_headers));
    assert(em_parse_request(large_headers, sizeof(large_headers), &request) == 431);

    em_backend backends[EM_BACKEND_COUNT] = {0}; unsigned cursor = 0;
    backends[0].state = 0; backends[1].state = 1; backends[2].state = 1;
    assert(em_select_backend(backends, EM_POLICY_ROUND_ROBIN, &cursor) == 1);
    assert(em_select_backend(backends, EM_POLICY_ROUND_ROBIN, &cursor) == 2);
    assert(em_select_backend(backends, EM_POLICY_ROUND_ROBIN, &cursor) == 1);
    assert(backends[1].active == 2 && backends[1].requests == 2);
    em_release_backend(&backends[1]); em_release_backend(&backends[1]);
    backends[1].active = 2; backends[2].active = 0; cursor = 0;
    assert(em_select_backend(backends, EM_POLICY_LEAST_CONNECTIONS, &cursor) == 2);
    assert(em_select_backend_excluding(backends, EM_POLICY_ROUND_ROBIN, &cursor, 2) == 1);
    backends[2].state = 0;
    assert(em_select_backend_excluding(backends, EM_POLICY_ROUND_ROBIN, &cursor, 1) == -1);

    assert(em_retry_allowed(0, 1, 0));
    assert(!em_retry_allowed(1, 1, 0));
    assert(!em_retry_allowed(2, 1, 0));
    assert(!em_retry_allowed(0, 0, 0));
    assert(!em_retry_allowed(0, 1, 1));
    assert(em_send_error_allowed(0));
    assert(!em_send_error_allowed(1));

    const char complete[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    const char incomplete[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nok";
    const char chunked[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
    assert(em_validate_response(complete, sizeof(complete) - 1) == 0);
    assert(em_validate_response(incomplete, sizeof(incomplete) - 1) != 0);
    assert(em_validate_response(chunked, sizeof(chunked) - 1) != 0);
    char *max_response = malloc(EM_MAX_RESPONSE + 1); assert(max_response);
    size_t body_length = 65000; char head[128]; int head_length;
    do {
        head_length = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n", (unsigned long)body_length);
        body_length = EM_MAX_RESPONSE - (size_t)head_length;
    } while ((size_t)snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n", (unsigned long)body_length) != (size_t)head_length);
    memcpy(max_response, head, (size_t)head_length);
    memset(max_response + head_length, 'x', body_length);
    assert(em_validate_response(max_response, EM_MAX_RESPONSE) == 0);
    assert(em_validate_response(max_response, EM_MAX_RESPONSE + 1) != 0);
    free(max_response);
    puts("core tests passed");
    return 0;
}
