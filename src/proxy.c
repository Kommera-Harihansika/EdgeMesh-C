#include "edgemesh/app.h"
#include "edgemesh/net.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct { int fd; } client_arg;

static void close_proxy_slot(int fd)
{
    close(fd);
    pthread_mutex_lock(&em_state_mutex);
    if (em_proxy_connections) --em_proxy_connections;
    pthread_mutex_unlock(&em_state_mutex);
}

static int start_proxy_handler(pthread_t *thread, void *(*entry)(void *), void *arg)
{
    pthread_attr_t attr; int rc = pthread_attr_init(&attr);
    if (rc) return rc;
    rc = pthread_attr_setstacksize(&attr, EM_HANDLER_STACK_SIZE);
    if (!rc) rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (!rc) rc = pthread_create(thread, &attr, entry, arg);
    pthread_attr_destroy(&attr);
    return rc;
}

static int64_t monotonic_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static const char *reason_phrase(int status)
{
    switch (status) {
    case 400: return "Bad Request";
    case 405: return "Method Not Allowed";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Internal Server Error";
    }
}

void em_send_http_error(int fd, int status, const char *message)
{
    char response[512];
    const char *text = message ? message : reason_phrase(status);
    int n = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %lu\r\nConnection: close\r\n\r\n%s\n",
        status, reason_phrase(status), (unsigned long)strlen(text) + 1, text);
    if (n > 0) { size_t ignored; em_write_all(fd, response, (size_t)n, &ignored); }
}

static int connect_backend(unsigned index, int timeout_ms, int64_t deadline)
{
    int fd, flags, rc, wait_ms;
    struct sockaddr_in address;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
#ifdef SO_NOSIGPIPE
    { int yes = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)); }
#endif
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    pthread_mutex_lock(&em_state_mutex);
    address.sin_port = htons(em_backends[index].port);
    pthread_mutex_unlock(&em_state_mutex);
    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    rc = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (rc && errno != EINPROGRESS) { close(fd); return -1; }
    if (rc) {
        struct pollfd pfd = { fd, POLLOUT, 0 };
        int64_t end = monotonic_ms() + timeout_ms;
        if (end > deadline) end = deadline;
        wait_ms = (int)(end - monotonic_ms());
        if (wait_ms <= 0 || poll(&pfd, 1, wait_ms) <= 0) { close(fd); errno = ETIMEDOUT; return -2; }
        { int error = 0; socklen_t length = sizeof(error); getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length); if (error) { close(fd); errno = error; return -1; } }
    }
    fcntl(fd, F_SETFL, flags);
    em_set_send_timeout(fd, timeout_ms);
    return fd;
}

static int read_upstream(int fd, char response[EM_MAX_RESPONSE + 1], size_t *length,
                         int64_t deadline, int *timed_out)
{
    size_t used = 0;
    int64_t read_deadline = monotonic_ms() + EM_UPSTREAM_READ_MS;
    if (read_deadline > deadline) read_deadline = deadline;
    *timed_out = 0;
    for (;;) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int64_t left = read_deadline - monotonic_ms();
        if (left <= 0) { *timed_out = 1; return -1; }
        int rc = poll(&pfd, 1, (int)left);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) { *timed_out = 1; return -1; }
        ssize_t n = recv(fd, response + used, EM_MAX_RESPONSE + 1 - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (n == 0) { *length = used; return used ? 0 : -1; }
        used += (size_t)n;
        if (used > EM_MAX_RESPONSE) { errno = EMSGSIZE; return -1; }
    }
}

static int write_client_response(int fd, const char *data, size_t length,
                                 int64_t deadline, size_t *written)
{
    size_t offset = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (written) *written = 0;
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
#ifdef SO_NOSIGPIPE
    { int yes = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)); }
#endif
    while (offset < length) {
        struct pollfd pfd = { fd, POLLOUT, 0 };
        int64_t left = deadline - monotonic_ms();
        if (left <= 0) break;
        int rc = poll(&pfd, 1, (int)left);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, data + offset, length - offset, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, data + offset, length - offset, 0);
#endif
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) break;
        offset += (size_t)n;
    }
    if (written) *written = offset;
    return offset == length ? 0 : -1;
}

static int one_attempt(int client_fd, const em_request *request, unsigned backend,
                       int64_t deadline, int *error_status, size_t *client_bytes_written)
{
    int upstream, timed_out = 0;
    char response[EM_MAX_RESPONSE + 1];
    char request_text[EM_MAX_TARGET + 512];
    size_t response_length = 0, written = 0;
    *client_bytes_written = 0;
    int n;
    upstream = connect_backend(backend, EM_UPSTREAM_CONNECT_MS, deadline);
    if (upstream < 0) { *error_status = errno == ETIMEDOUT ? 504 : 502; return -1; }
    n = snprintf(request_text, sizeof(request_text),
        "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nConnection: close\r\n\r\n",
        request->target, em_backends[backend].port);
    if (n <= 0 || (size_t)n >= sizeof(request_text) || em_write_all(upstream, request_text, (size_t)n, NULL)) {
        close(upstream); *error_status = 502; return -1;
    }
    if (read_upstream(upstream, response, &response_length, deadline, &timed_out) || em_validate_response(response, response_length)) {
        close(upstream); *error_status = timed_out ? 504 : 502; return -1;
    }
    close(upstream);
    if (write_client_response(client_fd, response, response_length, deadline, &written)) {
        em_add_response_bytes(written);
        *client_bytes_written = written;
        *error_status = 502;
        return -2; /* A client write began: retry is forbidden. */
    }
    em_add_response_bytes(written);
    *client_bytes_written = written;
    pthread_mutex_lock(&em_state_mutex);
    ++em_backends[backend].responses;
    pthread_mutex_unlock(&em_state_mutex);
    return 0;
}

static void *handle_proxy(void *value)
{
    client_arg *arg = value;
    int fd = arg->fd, parse_status, last_error = 503, selected = -1, success = 0;
    char input[EM_MAX_HEADERS];
    ssize_t got;
    em_request request;
    int64_t request_started, deadline;
    free(arg);
    em_set_send_timeout(fd, 2000);
    got = em_read_headers(fd, input, sizeof(input), EM_CLIENT_HEADER_MS);
    if (got < 0) {
        parse_status = got == -2 ? 431 : 400;
        fprintf(stderr, "{\"event\":\"client_rejection\",\"status\":%d}\n", parse_status);
        em_record_rejection(0); em_send_http_error(fd, parse_status, NULL); close_proxy_slot(fd); return NULL;
    }
    parse_status = em_parse_request(input, (size_t)got, &request);
    if (parse_status) {
        fprintf(stderr, "{\"event\":\"client_rejection\",\"status\":%d}\n", parse_status);
        em_record_rejection(0); em_send_http_error(fd, parse_status, NULL); close_proxy_slot(fd); return NULL;
    }
    request_started = monotonic_ms(); deadline = request_started + EM_REQUEST_DEADLINE_MS;
    em_begin_request();
    size_t response_bytes_written = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        em_policy policy;
        pthread_mutex_lock(&em_state_mutex);
        policy = em_current_policy;
        selected = em_select_backend_excluding(em_backends, policy, &em_selection_cursor, attempt ? selected : -1);
        pthread_mutex_unlock(&em_state_mutex);
        if (selected < 0) {
            if (attempt == 0) last_error = 503;
            break;
        }
        size_t attempt_bytes = 0;
        int attempt_result = one_attempt(fd, &request, (unsigned)selected, deadline, &last_error, &attempt_bytes);
        response_bytes_written += attempt_bytes;
        pthread_mutex_lock(&em_state_mutex);
        char backend_id[sizeof(em_backends[selected].id)]; snprintf(backend_id, sizeof(backend_id), "%s", em_backends[selected].id);
        em_release_backend(&em_backends[selected]);
        if (attempt_result == -1) ++em_backends[selected].failures_total;
        pthread_mutex_unlock(&em_state_mutex);
        if (attempt_result == -1) fprintf(stderr, "{\"event\":\"upstream_attempt_failure\",\"backend\":\"%s\",\"status\":%d}\n", backend_id, last_error);
        if (attempt_result == 0) { success = 1; break; }
        if (attempt_result == -2) { fprintf(stderr, "{\"event\":\"client_write_failure\",\"bytes_written\":%lu}\n", (unsigned long)attempt_bytes); break; }
        if (!em_retry_allowed((unsigned)attempt, attempt_result == -1, response_bytes_written)) break;
        if (monotonic_ms() >= deadline) { last_error = 504; break; }
    }
    if (!success && em_send_error_allowed(response_bytes_written)) {
        fprintf(stderr, "{\"event\":\"proxy_failure\",\"status\":%d}\n", last_error);
        em_send_http_error(fd, last_error, NULL);
    }
    em_finish_request(success, 0, (double)(monotonic_ms() - request_started));
    close_proxy_slot(fd);
    return NULL;
}

void *em_proxy_listener_main(void *unused)
{
    (void)unused;
    while (!em_stopping) {
        struct pollfd listener = { em_proxy_listener, POLLIN, 0 };
        int ready = poll(&listener, 1, 500);
        if (ready < 0) { if (errno == EINTR) continue; if (em_stopping) break; continue; }
        if (!ready || !(listener.revents & POLLIN)) continue;
        int fd = accept(em_proxy_listener, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; if (em_stopping) break; continue; }
        pthread_mutex_lock(&em_state_mutex);
        int full = em_proxy_connections >= EM_MAX_PROXY_CONNECTIONS;
        if (!full) ++em_proxy_connections;
        pthread_mutex_unlock(&em_state_mutex);
        if (full) { fprintf(stderr, "{\"event\":\"capacity_rejection\",\"listener\":\"proxy\"}\n"); em_record_rejection(1); em_send_http_error(fd, 503, "proxy connection limit reached"); close(fd); continue; }
        client_arg *arg = malloc(sizeof(*arg)); pthread_t thread;
        if (!arg) { pthread_mutex_lock(&em_state_mutex); --em_proxy_connections; pthread_mutex_unlock(&em_state_mutex); close(fd); continue; }
        arg->fd = fd;
        if (start_proxy_handler(&thread, handle_proxy, arg)) { free(arg); close(fd); pthread_mutex_lock(&em_state_mutex); --em_proxy_connections; pthread_mutex_unlock(&em_state_mutex); continue; }
    }
    return NULL;
}
