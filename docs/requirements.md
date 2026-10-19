# EdgeMesh-C Requirements

**Version:** 1.0 — MVP specification  
**Target environment:** macOS  
**Primary language:** C17  
**Build/test:** CMake and CTest

## 1. Product goal

EdgeMesh-C is an educational, C-first HTTP reverse proxy and load balancer. It distributes requests across three configured local backends, checks backend health, avoids unhealthy backends, records real performance metrics and exposes a local JSON API for a React + TypeScript dashboard. A repeatable C benchmark client compares behaviour under defined workloads.

This is a course project prototype, not a production-grade general-purpose proxy.

### First runnable prototype scope

The first implementation milestone delivers the C proxy/admin process, three C demo-backend processes, health checks, the six API endpoints, real metrics and focused tests. The dashboard and benchmark client remain later phases. Until a real benchmark result is available, the latest-benchmark endpoint reports unavailable. No metrics or benchmark results are simulated.

## 2. Fixed topology

One C application process owns two separate listeners:

| Component | Listen address | Purpose |
|---|---|---|
| Proxy | 127.0.0.1:8080 | Client HTTP traffic |
| Admin API | 127.0.0.1:9090 | Dashboard metrics and policy updates |

Three separate demo backend instances listen at 127.0.0.1:9001, 127.0.0.1:9002 and 127.0.0.1:9003. Each backend returns a small response identifying itself. Backend addresses are fixed in local configuration; clients cannot provide arbitrary upstream destinations.

The dashboard development server is expected at http://localhost:5173 or http://127.0.0.1:5173.

## 3. Functional requirements

### FR-01 — HTTP proxy
- Support a deliberately limited HTTP/1.1 subset: origin-form GET requests, bounded headers, one request per client connection and Connection: close.
- Require an HTTP/1.1 origin-form GET request with exactly one non-empty Host header and valid header syntax. Maximum request target is 4 KiB; maximum request-header section is 16 KiB.
- Accept no request body or transfer encoding; a single `Content-Length: 0` is allowed. Accept one request per client connection and return `Connection: close`.
- Preserve the path and query string when forwarding to a selected configured backend.
- Use a 2 s client-header read timeout, 1 s backend connect timeout, 2 s upstream response-read timeout and 5 s overall request deadline. Each attempt is bounded by the remaining overall deadline.
- Map malformed or unsupported framing to 400, unsupported methods to 405, request targets over 4 KiB to 414, and request headers over 16 KiB to 431.
- Map no eligible healthy backend and capacity rejection to 503, upstream timeout to 504, and terminal upstream connection/response failure (including invalid, incomplete or oversized responses) to 502.
- Correctly handle partial reads and writes. Return errors with `Connection: close`.
- Do not claim full HTTP/1.1 conformance. Request bodies, chunked request bodies, persistent connections, WebSockets, CONNECT, upgrades and arbitrary absolute URLs are out of scope.

### FR-02 — Load balancing
- Support exactly two MVP policies: round_robin and least_connections.
- Round robin rotates through eligible healthy backends.
- Least connections chooses the healthy backend with the fewest in-flight proxy requests; ties are resolved deterministically with rotation.
- Backend selection and reservation of an in-flight count must be thread-safe.
- If no backend is healthy, return HTTP 503 Service Unavailable.
- Permit at most one retry to a different healthy backend after an upstream connect failure, timeout or invalid/incomplete response, and only before any response byte has been successfully written to the client socket. Do not retry a valid upstream 4xx/5xx response. Never write a second status line after any response byte has been written.
- Buffer and validate the complete upstream response before forwarding. Limit the complete response, including headers and body, to 64 KiB; limit upstream response headers to 16 KiB. Reject incomplete, malformed or oversized responses before client forwarding so they remain eligible for the single retry.
- Require upstream responses to end by connection close; when Content-Length is present, verify it matches the body length at close. Transfer-encoded/chunked responses are unsupported in the first prototype and are treated as invalid upstream responses.

### FR-03 — Health checking and failover
- Probe each configured backend every 1 s with a 250 ms timeout.
- Track healthy, unhealthy or unknown state, last-check timestamp and probe latency when available.
- Keep backends `unknown` and ineligible until a successful probe. Mark a backend `unhealthy` after 3 consecutive failures; require 2 consecutive successes to recover it. A success resets the failure count and a failure resets the success count.
- Exclude unhealthy backends from new selections and return recovered backends to service after the recovery threshold.
- Do not block the entire proxy while a probe is waiting on network I/O.

### FR-04 — Real metrics
Collect request totals, successfully forwarded responses, proxy failures, active requests, per-backend request counts, response bytes, request latency and a bounded time series.
- The externally visible counter definitions are in `docs/api-contract.md` and apply to every API response. `requests_total` counts each valid supported client GET once after parsing, including requests that later fail. Client rejections and pre-parse capacity rejections have separate counters.
- Per-backend `requests_total` counts each backend attempt reserved for a logical request, including a retry attempt. Per-backend `proxy_failures_total` counts failed upstream attempts; the global `proxy_failures_total` counts a logical request only if no complete upstream response is ultimately forwarded.
- `responses_forwarded_total` counts complete valid upstream HTTP responses fully written to the client socket, including upstream 4xx/5xx. A partially written response is not a completed forwarded response.
- Response-byte metrics count upstream response header and body bytes successfully written to the client socket, including bytes written before a later partial-write failure. A successful socket write does not prove the remote client application received or processed those bytes.
- Health probes do not contribute to user-facing request, response, failure, latency or byte metrics.
- Proxy latency starts after request headers are parsed and ends after the complete upstream response is written to the client socket. Only completed forwarded responses enter latency percentiles. Define p50/p95/p99 over the trailing 60 s sample window; if no samples exist, return null.
- Bound metrics history and retained latency samples.

### FR-05 — Administrative JSON API
Implement the endpoints in api-contract.md and keep request/response schemas consistent with the frontend and tests. Use appropriate HTTP status codes and a common JSON error envelope.

### FR-06 — Benchmark client
- Provide a C command-line load generator for the local proxy with bounded request-count and concurrency options.
- Record configuration, requested/completed/failed requests, wall-clock duration, requests per second, response bytes per second and p50/p95/p99 latency.
- Save actual completed-run results in JSON and/or CSV.
- Do not fabricate results; unavailable measurements must be represented explicitly.

### FR-07 — Dashboard
- Implement Overview, Backend Health and Benchmark Lab screens.
- Use API responses for live values. Sample values may be used only in the design prototype or isolated frontend tests and must not be presented as live data.
- Display loading, disconnected, errors, empty results and unhealthy-backend states.
- Allow policy changes through the API and display whether the operation succeeded.

## 4. Non-functional requirements

- C17 is the implementation language for proxy, admin API, demo backend and benchmark client. React + TypeScript is only the presentation layer.
- Use POSIX/BSD sockets and pthreads supported by macOS; isolate platform-specific behaviour.
- Use CMake and CTest. Enable strong compiler warnings and use sanitizers where supported.
- Validate lengths and values; check system-call results; handle partial I/O, disconnects and timeouts; avoid unbounded buffers and unchecked allocation.
- Synchronise backend state, policy, active-connection reservations and metrics. Do not hold global locks during network I/O.
- Log health transitions and errors without logging arbitrary sensitive headers.
- Document build, run, test and benchmark commands.

## 5. Local security boundaries

- Bind proxy and admin listeners to loopback only by default.
- Backend targets are fixed to the configured local pool; no open-proxy or arbitrary URL-fetching capability.
- Bound header sizes, admin body sizes, connections and metric history.
- Allow at most 32 concurrent proxy connections and 8 concurrent admin connections. When either listener is at capacity, reject the accepted connection before request parsing with 503 and close it. Count such rejections in `capacity_rejections_total`; they do not increment `requests_total`.
- Allow only the documented local dashboard origins for browser CORS and never use wildcard CORS for policy-changing routes.
- CORS is not authentication and loopback is not a boundary against other local processes running as the same user. Do not expose the admin API publicly or claim production-grade security.
- TLS termination, HTTP/2, authentication infrastructure, cloud deployment and Kubernetes are out of scope.

## 6. Acceptance criteria

1. A clean checkout builds on macOS using documented CMake commands.
2. A request through the proxy receives a valid response from a configured demo backend.
3. Both policies can be selected; invalid values are rejected without changing the current policy.
4. Round robin rotates across healthy backends under a sequential test workload.
5. Least connections uses tracked in-flight counts with deterministic tie-breaking.
6. Stopping a backend marks it unhealthy within the configured probe threshold; remaining healthy backends continue serving requests.
7. If all backends are unavailable, a valid HTTP 503 is returned.
8. All six documented admin endpoints return schema-conforming JSON and documented errors.
9. Counters and latency metrics change in response to real requests.
10. The benchmark client writes actual results and their configuration to a result file.
11. Tests cover malformed requests, policy changes, failure/recovery and the no-healthy-backend path.
12. The dashboard reads live API data and handles API disconnection and missing results.

The first runnable prototype acceptance subset covers criteria 1–9 and the relevant malformed-input, failover, capacity, metric, and API tests. Benchmark-client and dashboard criteria remain deferred to their later phases.

Record commands and results as evidence. Source code existing is not proof that a criterion passed.

## 7. Explicitly deferred

TLS, HTTP/2, persistent connections, request-body forwarding, WebSockets, dynamic arbitrary backend registration, distributed operation, production authentication, autoscaling, deployment infrastructure and complex animation.
