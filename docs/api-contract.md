# EdgeMesh-C Admin API Contract

**Version:** 1.0 — MVP  
**Base URL:** http://127.0.0.1:9090  
**Allowed dashboard development origins:** http://localhost:5173 and http://127.0.0.1:5173  
**Format:** UTF-8 JSON

This document is the contract between the C admin API and React + TypeScript. Field names and enum values are case-sensitive. Update the frontend types and tests whenever this contract is intentionally changed.

## 1. General rules

- Bind only to 127.0.0.1:9090. Each request uses one connection; responses use Connection: close.
- Maximum admin request-header section: 16 KiB. Maximum admin request body: 4 KiB. Proxy request limits are specified in `docs/requirements.md`; these admin limits do not replace them.
- JSON content type: application/json; charset=utf-8.
- Timestamps use RFC 3339 UTC, e.g. 2026-10-19T05:30:00Z. Unavailable timestamps are null.
- Durations ending in _ms are milliseconds; rates ending in _per_second are per second.
- Counters are non-negative integers. Percentiles are numbers or null when no valid samples exist.
- Unknown route: 404. Unsupported method for a known route: 405. Malformed request/JSON or invalid values: 400. Oversized header section: 431. Oversized body: 413. Internal error: 500.
- CORS must never use wildcard origin. For an allowed browser origin return that exact origin and Vary: Origin. Support OPTIONS preflight for API routes.
- CORS is not authentication. This is a local educational prototype, not a production security boundary.

## 2. Error envelope

    {
      "error": {
        "code": "INVALID_POLICY",
        "message": "policy must be round_robin or least_connections"
      }
    }

The code is a stable uppercase identifier. The message is safe and explanatory. Do not include stack traces, arbitrary request bodies, secrets or sensitive local paths.

## 3. GET /api/v1/health

Returns 200 when the admin API is responding. This reports process/API health, not whether all backends are healthy.

Example response:

    {
      "status": "ok",
      "service": "edgemesh-c",
      "version": "0.1.0",
      "uptime_seconds": 128
    }

## 4. GET /api/v1/overview

Returns a snapshot of proxy state and aggregate metrics.

Example response (illustrative schema values only):

    {
      "proxy_running": true,
      "policy": "round_robin",
      "uptime_seconds": 128,
      "backends": {
        "total": 3,
        "healthy": 3,
        "unhealthy": 0,
        "unknown": 0
      },
      "metrics": {
        "requests_total": 420,
        "responses_forwarded_total": 414,
        "proxy_failures_total": 6,
        "client_rejections_total": 2,
        "capacity_rejections_total": 0,
        "requests_per_second": 7.2,
        "response_bytes_per_second": 18240,
        "active_requests": 2,
        "latency_p50_ms": 4.8,
        "latency_p95_ms": 12.6,
        "latency_p99_ms": 18.1
      },
      "sample_window_seconds": 60,
      "generated_at": "2026-10-19T05:30:00Z"
    }

Definitions:
- `requests_total` is a cumulative count of valid supported proxy GET requests, incremented once after parsing and before backend selection. It includes requests that later fail; a retry does not increment it again. Malformed or unsupported input is not counted here.
- `client_rejections_total` counts proxy input rejected before a valid supported GET is established, including malformed, unsupported, oversized, timed-out or disconnected client input. `capacity_rejections_total` counts accepted proxy or admin connections refused before request parsing. Neither rejection counter increments `requests_total`.
- Global `proxy_failures_total` counts valid logical proxy requests that do not finish with a complete forwarded upstream response, including no healthy backend, exhausted upstream attempts, and client-write failure. A failed first backend attempt followed by a successful retry does not increment the global counter.
- In `/api/v1/backends`, each backend's `requests_total` counts attempts reserved for that backend, including a retry. Its `proxy_failures_total` counts failed upstream attempts for that backend. `responses_forwarded_total` counts complete upstream responses fully written through that backend, including upstream 4xx/5xx.
- Global `responses_forwarded_total` counts complete valid upstream HTTP responses fully written to the client socket, including upstream 4xx/5xx. An incomplete upstream response or partial client write is not a completed forwarded response.
- `response_bytes_per_second` counts upstream response header and body bytes successfully written to the client socket. It includes partial writes made before a later failure and excludes locally generated proxy error responses. A successful socket write does not prove the remote client application received or processed those bytes.
- Health probes do not contribute to any user-facing request, response, failure, byte or latency metric. Probe timestamp and latency fields on each backend are health-state data only.
- `requests_per_second` and `response_bytes_per_second` are rolling-window rates. Overview uses the trailing 60 seconds; before the service has run for 60 seconds, divide by elapsed whole seconds so startup rates do not include time before process start.
- `active_requests` counts parsed valid logical proxy requests currently in progress; pre-parse connections are excluded.
- Proxy latency starts after request headers are parsed and ends when the complete upstream response is written to the client socket. Only complete forwarded responses enter latency percentiles. Overview percentiles use the trailing 60-second window and return null when empty.

## 5. GET /api/v1/backends

Returns every configured backend.

Example response (one illustrative entry; a real response includes all configured entries):

    {
      "backends": [
        {
          "id": "backend-1",
          "address": "127.0.0.1:9001",
          "state": "healthy",
          "active_connections": 1,
          "requests_total": 180,
          "responses_forwarded_total": 178,
          "proxy_failures_total": 2,
          "last_check_at": "2026-10-19T05:29:59Z",
          "last_response_ms": 1.7
        }
      ],
      "generated_at": "2026-10-19T05:30:00Z"
    }

State is exactly healthy, unhealthy or unknown. last_check_at and last_response_ms may be null before the first completed probe.

## 6. PUT /api/v1/policy

Request body:

    {
      "policy": "least_connections"
    }

Allowed values are round_robin and least_connections.

Successful response: 200 OK.

    {
      "policy": "least_connections",
      "updated": true
    }

Reject missing fields, incorrect JSON types, unknown values or trailing unexpected content with 400 Bad Request. A body over 4 KiB returns 413 Payload Too Large. A rejected update must not change the current policy. Policy updates and selector reads must be synchronised.

## 7. GET /api/v1/metrics?window_seconds=60

The optional window_seconds parameter is an integer from 10 through 300 inclusive. Default is 60. Reject invalid or out-of-range values with 400 Bad Request.

Example response:

    {
      "window_seconds": 60,
      "points": [
        {
          "timestamp": "2026-10-19T05:29:00Z",
          "requests_total": 390,
          "responses_forwarded_total": 385,
          "proxy_failures_total": 5,
          "client_rejections_total": 1,
          "capacity_rejections_total": 0,
          "requests_per_second": 6.4,
          "response_bytes_per_second": 16500,
          "latency_p50_ms": 4.6,
          "latency_p95_ms": 11.9,
          "latency_p99_ms": 17.0
        }
      ],
      "generated_at": "2026-10-19T05:30:00Z"
    }

Return only one-second samples that actually exist in the requested window; omit empty seconds. The `*_total` fields are cumulative snapshots at each point's timestamp. The rate fields are counts/bytes observed during that one-second bucket. Point latency percentiles use successful response samples in that bucket and are null when it has none. Empty points is valid immediately after startup. Do not invent history to fill gaps.

## 8. GET /api/v1/benchmarks/latest

Returns the newest completed benchmark result saved by the local benchmark tool. This endpoint does not start a run.

The first runnable prototype defers the benchmark client, so this endpoint returns the unavailable response until a genuine result is provided by a later implementation. Do not fabricate benchmark values.

When there is no completed run, return 200 OK:

    {
      "available": false,
      "run": null
    }

When a run exists:

    {
      "available": true,
      "run": {
        "run_id": "bench-20261019-053000",
        "completed_at": "2026-10-19T05:30:00Z",
        "policy": "round_robin",
        "target": "http://127.0.0.1:8080/",
        "requested_requests": 1000,
        "concurrency": 20,
        "completed_requests": 996,
        "failed_requests": 4,
        "duration_seconds": 8.4,
        "requests_per_second": 118.6,
        "response_bytes_per_second": 246000,
        "latency_p50_ms": 92.0,
        "latency_p95_ms": 180.0,
        "latency_p99_ms": 240.0,
        "result_file": "results/latest.json"
      }
    }

All benchmark numbers above are example schema values, not measured or promised performance. Persisted values must come from an actual completed run. The API reads only a fixed documented result path; it must never accept an arbitrary path from the caller. If persisted data is invalid or unreadable, return a safe 500 error and log the internal reason.

## 9. Metric definitions

- The overview and backend counter semantics above are normative for every endpoint and test.
- p50/p95/p99 are computed from completed response latency samples in the documented interval; with no samples return null.
- Response-byte rates include upstream response headers and body only when successfully written to the client socket. Partial writes count the bytes written even though the response is incomplete.
- Benchmark duration is wall-clock time from the first dispatched request until the run completes.
- Benchmark requests_per_second is completed requests divided by that duration.
- Record requested, completed and failed counts separately.
- The dashboard must respect units and render null/empty data safely.

## 10. Contract tests

Test successful and error paths for every endpoint, invalid JSON, invalid policy, unsupported methods, oversized bodies and schema consistency. The first prototype tests the unavailable benchmark state; a populated saved-result state is tested when the benchmark result reader is implemented. Keep TypeScript types aligned to this contract.
