# EdgeMesh-C Architecture

**Version:** 1.0 — MVP  
**Platform:** macOS, C17 and POSIX sockets

## 1. System topology

    React + TypeScript dashboard (localhost:5173)
                    |
                    | JSON over HTTP, allowed local CORS origin
                    v
    Admin API listener — C application — 127.0.0.1:9090
                    |
                    v
          Bounded metrics/state snapshots

    Browser or C benchmark client
                    |
                    | HTTP GET
                    v
    Reverse-proxy listener — same C process — 127.0.0.1:8080
                    |
                    v
           Backend selector
       Round robin / Least connections
                    |
              Configured pool
             /      |      \
            v       v       v
       Backend 1 Backend 2 Backend 3
       :9001     :9002     :9003

    Health-check worker ---> backend state and metrics
    C benchmark client ----> local JSON/CSV result files
    Admin API -------------> reads latest completed benchmark result

The proxy and admin API are separate listeners and handlers in one C application process. Demo backends are separate local server processes. The dashboard is a separate development application. The benchmark client is a separate C command-line program.

The first runnable prototype implements the C process, demo backends, health worker, metrics and admin API. Dashboard and benchmark-client boxes describe later phases; their absence is not replaced by simulated data. The latest-benchmark route returns the documented unavailable state.

## 2. Module responsibilities

| Module | Responsibility |
|---|---|
| src/proxy | Accept client connections, enforce connection limits, parse one request and coordinate bounded proxy attempts |
| src/http | Parse the supported request subset, validate headers and produce HTTP errors |
| src/backend | Fixed backend pool, eligible-backend selection and thread-safe in-flight counters |
| src/health | Periodic bounded probes and state transitions using success/failure thresholds |
| src/metrics | Actual counters, latency samples, throughput and bounded time-series history |
| src/admin_api | Admin HTTP routes, JSON validation/serialization and policy updates |
| src/common | Shared bounded I/O, timestamps, logging and error helpers where useful |
| src/demo_backend | Small demo server returning its backend identity and providing controlled health behaviour |
| benchmark | C load generator and JSON/CSV result writer |
| dashboard | React + TypeScript screens that display API data without owning proxy state |
| tests | Unit, integration and failure-scenario tests |

The exact file split may evolve, but API and module contracts must remain consistent with the documentation.

## 3. Proxy request lifecycle

1. Accept a TCP client on 127.0.0.1:8080, respecting the configured connection limit.
2. Read until the end of the request headers with a 2 s timeout and a 16 KiB maximum header size.
3. Validate request method, target, version and header syntax. Accept only an HTTP/1.1 origin-form GET with one non-empty Host header; limit the target to 4 KiB and headers to 16 KiB. Accept no request body or transfer encoding; a single `Content-Length: 0` is allowed.
4. Select a healthy backend according to the current policy. If none is healthy, send 503 Service Unavailable.
5. Reserve the selected backend's in-flight count atomically with the selection decision.
6. Connect to the fixed backend with a 1 s connect timeout; forward the GET path/query and controlled headers. Do not blindly forward hop-by-hop headers.
7. Read an upstream response with a 2 s read timeout and a 5 s overall logical-request deadline. Buffer the complete response up to 64 KiB total (including the status/header section and body); cap upstream response headers at 16 KiB. Require connection-close delimitation and validate Content-Length when present; reject transfer-encoded/chunked responses in this MVP.
8. On upstream connect failure, timeout, or invalid/incomplete/oversized response, retry at most once on a different currently healthy backend while no response bytes have been written to the client socket. Never retry a valid upstream 4xx/5xx response. Once any response byte is written, never retry or send a second status line.
9. Write the buffered response to the client, handling partial writes and timeouts. A partial client write is a failed response and cannot be retried.
10. Close sockets and release every in-flight reservation on every success and failure path. Record outcomes using the metric contract in `docs/api-contract.md`.

This is not a complete HTTP implementation. The MVP is one request per client connection with connection-close behaviour. Request bodies, chunked request bodies, upgrades, CONNECT and persistent connections are excluded.

## 4. Backend state and selection

Each backend record contains a stable ID, fixed loopback address, health state, consecutive probe counts, last-check time, optional probe latency, in-flight request count and request/failure counters.

**Round robin:** Rotate through the configured list and skip unhealthy backends.

**Least connections:** Choose the healthy backend with the smallest in-flight count. Use deterministic rotation among ties to prevent persistent first-entry preference.

The selector and in-flight reservation must share a synchronisation strategy so concurrent workers cannot choose a backend using a stale count. Decrement the count on all exit paths. Never hold the backend lock during network I/O.

Health checks run in a separate worker so probes do not block proxy requests. Probe every backend once per second with a 250 ms timeout. A backend begins `unknown` and is not selectable until one successful probe. Three consecutive failures mark it unhealthy; two consecutive successes recover it. Probes update backend health fields only and never increment user-facing request, response, failure, byte or latency metrics.

## 5. Concurrency and resource limits

A detached pthread-per-connection model with a 512 KiB handler stack is bounded for this MVP: at most 32 proxy connections and 8 admin connections are active. Each listener reserves its slot before parsing. At capacity, return 503 with `Connection: close` and close the accepted socket; do not parse it. Capacity rejections are counted separately from logical requests. Proxy and admin work and health probes must not block one another indefinitely.

Timeouts, limits and thresholds are compile-time constants in `include/edgemesh/app.h` and `include/edgemesh/core.h`: 2 s client header read, 1 s upstream connect, 2 s upstream response read, 5 s total logical proxy request, 1 s health interval, 250 ms health probe, 32 proxy connections, 8 admin connections, 4 KiB request target, 16 KiB request/upstream headers and 64 KiB complete upstream response buffer.

- Protect backend state, active counts, selected policy and metrics with scoped mutexes or equivalent.
- Do not hold shared-state locks while waiting on socket I/O.
- A supported request increments the global logical request counter once after parsing. Per-backend request counters increment for each reserved backend attempt, including a retry. Detailed counter definitions and response-byte semantics are specified in `docs/api-contract.md`.
- Bound all buffers, request sizes, latency sample storage and time-series history.
- Handle signals and shut down listeners/workers cleanly.
- Keep a single, auditable cleanup path for sockets, buffers and in-flight reservations.

## 6. Admin API and dashboard

The admin API exposes only the six endpoints in api-contract.md. The policy endpoint accepts only the two enumerated policy values. The API does not execute commands or accept arbitrary backend addresses.

- Bind only to 127.0.0.1:9090.
- Limit admin headers to 16 KiB and request bodies to 4 KiB.
- Reject an oversized admin header section with 431 and an oversized admin body with 413. Malformed JSON and invalid parameter values receive 400; an unknown route receives 404; a known route with an unsupported method receives 405.
- Support OPTIONS preflight for the dashboard development origins.
- Permit only http://localhost:5173 and http://127.0.0.1:5173 through browser CORS.
- Do not treat CORS as authentication; local processes can make direct requests to a loopback service.

The future benchmark tool saves actual results locally. In this prototype the benchmark client is not implemented, so `/api/v1/benchmarks/latest` returns an explicit unavailable state. Never fabricate a result.

The future dashboard polls at two-second intervals, cleans up requests/timers when screens unmount, and renders loading, error, disconnected and empty states. Every integrated live metric must come from the API, not hardcoded mock values.

## 7. Target repository layout

    EdgeMesh-C/
    ├── AGENTS.md
    ├── CMakeLists.txt
    ├── README.md
    ├── LICENSE
    ├── .gitignore
    ├── docs/
    │   ├── requirements.md
    │   ├── architecture.md
    │   ├── api-contract.md
    │   └── test-plan.md
    ├── include/
    ├── src/
    │   ├── proxy/
    │   ├── http/
    │   ├── backend/
    │   ├── health/
    │   ├── metrics/
    │   ├── admin_api/
    │   ├── common/
    │   └── demo_backend/
    ├── tests/
    ├── benchmark/
    ├── dashboard/
    └── results/

This is a target layout. Create source files and directories as implementation needs them; empty directories alone are not an implementation milestone.

## 8. Decisions and trade-offs

- C is the networking core; TypeScript is presentation only.
- One C process owns two separate listeners to reduce process-management overhead.
- Backends are fixed local targets to avoid turning the prototype into an open proxy.
- Runtime metrics must be measured, not simulated.
- The HTTP subset is intentionally small to keep parsing and forwarding testable within the deadline.
- No container, cloud or production deployment is an MVP dependency.
