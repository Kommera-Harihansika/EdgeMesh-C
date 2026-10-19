# EdgeMesh-C Test Plan

**Version:** 1.0 — MVP  
**Environment:** macOS, C17, CMake, CTest, Python 3 for integration tests

## 1. First prototype coverage

The first runnable prototype tests the C proxy, three demo backends, the six documented admin API endpoints, health transitions, metrics and failure handling. The dashboard and benchmark client are deferred, so their browser and benchmark-comparison tests are not part of this milestone. The latest-benchmark endpoint must return unavailable when no genuine result exists.

## 2. Build and test workflow

From a clean macOS checkout:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`core_unit` covers parsing, backend selection, retry eligibility, response validation and the 64 KiB response-buffer boundary. `metrics_unit` checks counter, byte, rate and latency semantics. `local_integration` starts the C process and demo backends to exercise real sockets and API behavior when Python 3 is available. These commands describe the workflow; report actual execution results separately. Use a separate AddressSanitizer/UndefinedBehaviorSanitizer build where the compiler supports it.

## 3. Unit tests

| Area | Cases | Expected outcome |
|---|---|---|
| Proxy HTTP parser | Valid HTTP/1.1 origin-form GET, missing/duplicate Host, malformed lines, unsupported method, HTTP/1.0, absolute-form target, invalid header syntax | Accept only the documented GET subset; malformed input is 400 and unsupported method is 405 |
| Request limits and framing | 4 KiB target boundary, target over 4 KiB, 16 KiB header boundary, oversized header, body, transfer encoding, duplicate Content-Length | Accept within limits; return 414 for an oversized target, 431 for oversized headers, and 400 for unsupported/ambiguous framing |
| Backend selection | Unknown/unhealthy exclusion, round-robin rotation, least-connections choice and ties, reservations, exclusion of prior backend | Select only healthy backends, reserve atomically, rotate ties deterministically, and release reservations exactly once |
| Retry safety | First eligible upstream failure before client writes; second attempt; valid upstream response; bytes already written | Permit at most one retry to another backend only for eligible upstream failures with zero client-socket response bytes; prohibit retry and a second status line after any byte is written |
| Upstream response validation | Valid Content-Length, EOF-delimited response, incomplete body, malformed status/header, duplicate length, chunked response, 16 KiB header boundary, 64 KiB total response boundary and oversize | Buffer and validate before forwarding; reject incomplete, invalid, chunked or oversized responses before a client write |
| Metrics | Logical request success/failure, retry attempt, client rejection, capacity rejection, partial response write, empty sample window and percentile calculation | Global logical counters increment once per parsed GET; per-backend counters include each attempt; probes are not recorded; byte counts reflect successful socket writes; empty percentiles are null |
| Health | Unknown startup, first successful probe, 3 consecutive failures, 2 consecutive successes, reset of opposite counter | Unknown is ineligible; thresholds change state exactly as specified; probes do not change user-facing metrics |
| Admin API input | Valid/invalid policy JSON, missing field, wrong type, trailing data, invalid metrics window, unknown route, wrong method, 4 KiB body and 16 KiB headers | Rejected updates do not change policy; return the API contract status and JSON error envelope |
| JSON schemas | Overview, backend list, metrics points, health and latest benchmark unavailable state | JSON parses, fields and units match `docs/api-contract.md`, and unavailable benchmark data is not fabricated |

## 4. Integration and failure tests

The Python integration suite runs against loopback ports 8080, 9090 and 9001–9003. It must:

1. Launch three demo backend instances and the C proxy/admin process; verify backend state becomes healthy only after probes.
2. Send proxy GETs and confirm backend identity and round-robin distribution; change policy using `PUT /api/v1/policy`.
3. Exercise all six documented endpoints, invalid policy/window values, unknown routes, unsupported methods, and admin body/header limits.
4. Replace one backend with a test upstream that returns an incomplete response. Verify the proxy retries a different healthy backend, counts one logical request, records both backend attempts, and does not count a terminal global failure when retry succeeds.
5. Return a valid upstream 503 response and verify it is forwarded without retry.
6. Send malformed, unsupported, oversized-target and oversized-header proxy requests; verify 400/405/414/431 and that these do not increment `requests_total`.
7. Hold 32 proxy and 8 admin connections, then verify the next connection receives 503 before parsing and increments the separate capacity counter.
8. Verify response bytes and latency change only on actual proxy traffic; leave health probes running and confirm they do not increment user-facing request metrics.
9. Stop all backends and verify they become unhealthy after 3 failed probes and a valid proxy request receives 503. Restart a backend, verify recovery after 2 successful probes, and confirm it serves traffic again.
10. Confirm the latest-benchmark route returns `available: false` in this prototype; both the benchmark client and result reader are deferred.

The test suite cannot reliably force a real operating-system partial client-socket write on every machine. The retry gate is also tested as a unit; integration tests should record this limitation rather than imply that socket delivery to the remote application is observable.

## 5. Manual demonstration checklist

1. Build and run CTest, retaining the actual output.
2. Start the three demo backends and then `edgemesh` in separate terminals.
3. Use `curl` to request the proxy repeatedly and show backend identities.
4. Inspect `/api/v1/backends`, `/api/v1/overview`, and `/api/v1/metrics` to show real state and metrics.
5. Change the policy with `PUT /api/v1/policy` and inspect the returned policy.
6. Stop one backend and show the unhealthy transition while other backends continue serving requests; restart and show recovery.
7. Stop all backends and show the proxy's 503 response, then restart one and verify recovery.
8. Show `/api/v1/benchmarks/latest` reporting unavailable. Do not present sample numbers as measured performance.

## 6. Evidence

Record the exact configure, build and CTest commands and their outputs. Preserve integration logs and any genuine benchmark result only when a benchmark client exists and completed a run. Never mark an unexecuted test as passed, and do not claim macOS validation from a Windows or other platform run.
