#include "edgemesh/core.h"
#include "edgemesh/net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    unsigned long port; char *tail; const char *identity; char expected[16];
    int listener; signal(SIGPIPE, SIG_IGN);
    if (argc != 3) { fprintf(stderr, "usage: %s <port> <backend-id>\n", argv[0]); return 2; }
    errno = 0; port = strtoul(argv[1], &tail, 10); identity = argv[2];
    if (errno || tail == argv[1] || *tail || port < 9001 || port > 9003) { fprintf(stderr, "invalid backend port\n"); return 2; }
    snprintf(expected, sizeof(expected), "backend-%lu", port - 9000);
    if (strcmp(identity, expected)) { fprintf(stderr, "backend identity must match its configured port\n"); return 2; }
    listener = em_make_listener((unsigned short)port);
    if (listener < 0) { perror("backend bind"); return 1; }
    printf("demo backend %s listening on 127.0.0.1:%lu\n", identity, port); fflush(stdout);
    for (;;) {
        int client = accept(listener, NULL, NULL); if (client < 0) continue;
        char input[EM_MAX_HEADERS]; ssize_t got = em_read_headers(client, input, sizeof(input), 2000);
        if (got > 0) {
            char body[128], response[512]; int b = snprintf(body, sizeof(body), "{\"backend\":\"%s\"}", identity);
            int n = snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", b, body);
            if (n > 0) em_write_all(client, response, (size_t)n, NULL);
        }
        close(client);
    }
    return 0;
}
