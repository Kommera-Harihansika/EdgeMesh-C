#include "edgemesh/app.h"
#include "edgemesh/net.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

pthread_mutex_t em_state_mutex = PTHREAD_MUTEX_INITIALIZER;
em_backend em_backends[EM_BACKEND_COUNT];
em_policy em_current_policy = EM_POLICY_ROUND_ROBIN;
unsigned em_selection_cursor = 0;
em_metrics em_global_metrics = { .mutex = PTHREAD_MUTEX_INITIALIZER };
volatile sig_atomic_t em_stopping = 0;
time_t em_started_at;
int em_proxy_listener = -1, em_admin_listener = -1;
unsigned em_proxy_connections = 0, em_admin_connections = 0;

static void stop_handler(int signal_number)
{
    (void)signal_number;
    em_stopping = 1;
    if (em_proxy_listener >= 0) close(em_proxy_listener);
    if (em_admin_listener >= 0) close(em_admin_listener);
}

int main(void)
{
    const char *ids[EM_BACKEND_COUNT] = { "backend-1", "backend-2", "backend-3" };
    const unsigned short ports[EM_BACKEND_COUNT] = { 9001, 9002, 9003 };
    pthread_t proxy_thread, admin_thread, health_thread;
    struct sigaction action;
    signal(SIGPIPE, SIG_IGN);
    memset(&action, 0, sizeof(action)); action.sa_handler = stop_handler; sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL); sigaction(SIGTERM, &action, NULL);
    em_started_at = time(NULL);
    for (unsigned i = 0; i < EM_BACKEND_COUNT; ++i) {
        snprintf(em_backends[i].id, sizeof(em_backends[i].id), "%s", ids[i]);
        snprintf(em_backends[i].address, sizeof(em_backends[i].address), "127.0.0.1");
        em_backends[i].port = ports[i]; em_backends[i].state = 0;
    }
    em_proxy_listener = em_make_listener(EM_PROXY_PORT);
    em_admin_listener = em_make_listener(EM_ADMIN_PORT);
    if (em_proxy_listener < 0 || em_admin_listener < 0) {
        perror("failed to bind loopback listeners");
        if (em_proxy_listener >= 0) close(em_proxy_listener);
        if (em_admin_listener >= 0) close(em_admin_listener);
        return 1;
    }
    int health_started = pthread_create(&health_thread, NULL, em_health_main, NULL) == 0;
    int proxy_started = health_started && pthread_create(&proxy_thread, NULL, em_proxy_listener_main, NULL) == 0;
    int admin_started = proxy_started && pthread_create(&admin_thread, NULL, em_admin_listener_main, NULL) == 0;
    if (!admin_started) {
        fprintf(stderr, "failed to start worker thread\n"); stop_handler(0);
        if (proxy_started) pthread_join(proxy_thread, NULL);
        if (health_started) { em_stopping = 1; pthread_join(health_thread, NULL); }
        return 1;
    }
    printf("EdgeMesh-C listening on 127.0.0.1:%d (proxy) and :%d (admin)\n", EM_PROXY_PORT, EM_ADMIN_PORT);
    fflush(stdout);
    pthread_join(proxy_thread, NULL); pthread_join(admin_thread, NULL);
    em_stopping = 1;
    pthread_join(health_thread, NULL);
    while (em_proxy_connections || em_admin_connections) { struct timespec delay = { 0, 10000000 }; nanosleep(&delay, NULL); }
    return 0;
}
