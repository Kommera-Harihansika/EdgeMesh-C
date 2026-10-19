#include "edgemesh/app.h"
#include "edgemesh/net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int probe(unsigned index, double *elapsed)
{
    int fd, flags, rc, error = 0; socklen_t error_length = sizeof(error);
    struct sockaddr_in address; int64_t start = em_now_ms(), deadline = start + EM_HEALTH_TIMEOUT_MS;
    fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return 0;
#ifdef SO_NOSIGPIPE
    { int yes = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)); }
#endif
    memset(&address, 0, sizeof(address)); address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    pthread_mutex_lock(&em_state_mutex); address.sin_port = htons(em_backends[index].port); pthread_mutex_unlock(&em_state_mutex);
    flags = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    rc = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (rc && errno != EINPROGRESS) { close(fd); return 0; }
    if (rc) {
        struct pollfd pfd = { fd, POLLOUT, 0 };
        int left = (int)(deadline - em_now_ms());
        if (left <= 0 || poll(&pfd, 1, left) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_length) || error) { close(fd); return 0; }
    }
    fcntl(fd, F_SETFL, flags);
    em_set_send_timeout(fd, EM_HEALTH_TIMEOUT_MS);
    const char request[] = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    if (em_write_all(fd, request, sizeof(request) - 1, NULL)) { close(fd); return 0; }
    struct pollfd pfd = { fd, POLLIN, 0 };
    int left = (int)(deadline - em_now_ms());
    if (left <= 0 || poll(&pfd, 1, left) <= 0) { close(fd); return 0; }
    char response[128]; ssize_t n = recv(fd, response, sizeof(response) - 1, 0); close(fd);
    if (n < 12) return 0;
    response[n] = '\0'; *elapsed = (double)(em_now_ms() - start);
    return !strncmp(response, "HTTP/1.1 200", 12) || !strncmp(response, "HTTP/1.0 200", 12);
}

void *em_health_main(void *unused)
{
    (void)unused;
    while (!em_stopping) {
        int64_t cycle_start = em_now_ms();
        for (unsigned i = 0; i < EM_BACKEND_COUNT; ++i) {
            double latency = 0; int ok = probe(i, &latency); time_t now = time(NULL);
            pthread_mutex_lock(&em_state_mutex);
            em_backend *b = &em_backends[i]; int old_state = b->state; b->last_check = (int64_t)now;
            if (ok) {
                b->failures = 0; if (b->successes < 2) ++b->successes; b->last_response_ms = latency;
                if (b->state == 0 || (b->state == 2 && b->successes >= 2)) b->state = 1;
            } else {
                b->successes = 0; if (b->failures < 3) ++b->failures;
                if (b->failures >= 3) b->state = 2;
            }
            int new_state = b->state; char id[sizeof(b->id)]; snprintf(id, sizeof(id), "%s", b->id);
            pthread_mutex_unlock(&em_state_mutex);
            if (old_state != new_state) {
                const char *states[] = { "unknown", "healthy", "unhealthy" };
                fprintf(stderr, "{\"event\":\"health_transition\",\"backend\":\"%s\",\"from\":\"%s\",\"to\":\"%s\"}\n", id, states[old_state], states[new_state]);
            }
        }
        int64_t remaining = EM_HEALTH_INTERVAL_MS - (em_now_ms() - cycle_start);
        if (remaining < 0) remaining = 0;
        struct timespec wait = { (time_t)(remaining / 1000), (long)(remaining % 1000) * 1000000 };
        while (!em_stopping && nanosleep(&wait, &wait) && errno == EINTR) { }
    }
    return NULL;
}
