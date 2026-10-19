#include "edgemesh/net.h"
#include "edgemesh/app.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int has_header_end(const char *data, size_t length)
{
    for (size_t i = 0; i + 3 < length; ++i)
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') return 1;
    return 0;
}

int em_make_listener(unsigned short port)
{
    int fd, yes = 1;
    struct sockaddr_in address;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) || listen(fd, 64)) {
        close(fd); return -1;
    }
    return fd;
}

ssize_t em_read_headers(int fd, char *buffer, size_t capacity, int timeout_ms)
{
    size_t used = 0;
    int64_t deadline = em_now_ms() + timeout_ms;
    while (used < capacity) {
        struct pollfd pfd;
        int remaining = (int)(deadline - em_now_ms());
        int rc;
        if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
        pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
        do { rc = poll(&pfd, 1, remaining); } while (rc < 0 && errno == EINTR);
        if (rc <= 0) { errno = ETIMEDOUT; return -1; }
        if (!(pfd.revents & POLLIN) && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) { errno = ECONNRESET; return -1; }
        ssize_t n = recv(fd, buffer + used, capacity - used, 0);
        if (n <= 0) return -1;
        used += (size_t)n;
        if (used >= 4 && has_header_end(buffer, used)) return (ssize_t)used;
    }
    errno = EMSGSIZE;
    return -2;
}

int em_write_all(int fd, const char *data, size_t length, size_t *written)
{
    size_t offset = 0;
    if (written) *written = 0;
    while (offset < length) {
        ssize_t n = send(fd, data + offset, length - offset, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (written) *written = offset; return -1; }
        offset += (size_t)n;
    }
    if (written) *written = offset;
    return 0;
}

void em_set_send_timeout(int fd, int timeout_ms)
{
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
