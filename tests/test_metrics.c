#include "edgemesh/app.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

pthread_mutex_t em_state_mutex = PTHREAD_MUTEX_INITIALIZER;
em_backend em_backends[EM_BACKEND_COUNT];
em_policy em_current_policy = EM_POLICY_ROUND_ROBIN;
unsigned em_selection_cursor;
em_metrics em_global_metrics = { .mutex = PTHREAD_MUTEX_INITIALIZER };
volatile sig_atomic_t em_stopping;
time_t em_started_at;
int em_proxy_listener = -1, em_admin_listener = -1;
unsigned em_proxy_connections, em_admin_connections;

int main(void)
{
    char overview[4096], points[8192];
    em_started_at = time(NULL) - 2;
    em_begin_request();
    em_add_response_bytes(12);
    em_finish_request(1, 12, 7.5);
    em_begin_request();
    em_add_response_bytes(3); /* A partial client write before terminal failure. */
    em_finish_request(0, 0, 9.0);
    em_record_rejection(0);
    em_record_rejection(1);
    em_format_overview(overview, sizeof(overview));
    assert(strstr(overview, "\"requests_total\":2"));
    assert(strstr(overview, "\"responses_forwarded_total\":1"));
    assert(strstr(overview, "\"proxy_failures_total\":1"));
    assert(strstr(overview, "\"client_rejections_total\":1"));
    assert(strstr(overview, "\"capacity_rejections_total\":1"));
    assert(strstr(overview, "\"latency_p50_ms\":7.500"));
    em_format_metrics(points, sizeof(points), 10);
    assert(strstr(points, "\"requests_per_second\":2"));
    assert(strstr(points, "\"response_bytes_per_second\":15"));
    assert(strstr(points, "\"client_rejections_total\":1"));
    assert(strstr(points, "\"capacity_rejections_total\":1"));
    puts("metrics tests passed");
    return 0;
}
