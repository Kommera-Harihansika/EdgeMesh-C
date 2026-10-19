# EdgeMesh-C

EdgeMesh-C is an educational C17 HTTP reverse proxy and load balancer. This first runnable prototype provides a loopback proxy, three demo backends, round-robin and least-connections selection, health checks, retry-before-forwarding failover, runtime metrics, and the six documented local admin API endpoints.

The React dashboard and benchmark client are later project phases. Until a real benchmark client writes a result, `GET /api/v1/benchmarks/latest` returns `{"available":false,"run":null}`. The prototype does not make up benchmark data.

## Requirements

- macOS with a C17 compiler and POSIX sockets/pthreads
- CMake 3.16 or newer
- Python 3 for the CTest integration suite

The application is specified for macOS/POSIX. Native Windows/MinGW is not a supported build target; use a macOS machine or macOS CI for the supported build and test run.

## Build and test

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CTest runs the parser/selection/retry unit suite, metric-accounting unit suite, and a local integration suite when Python 3 is available.

## Run the prototype

Open four terminal windows in the repository. Start one backend in each of the first three:

```sh
./build/edgemesh_demo_backend 9001 backend-1
./build/edgemesh_demo_backend 9002 backend-2
./build/edgemesh_demo_backend 9003 backend-3
```

Start the proxy and admin API in the fourth:

```sh
./build/edgemesh
```

Both listeners bind to loopback: proxy `127.0.0.1:8080`, admin API `127.0.0.1:9090`.

## Demonstration walkthrough

1. Request a backend through the proxy and repeat to see round-robin identities:

   ```sh
   curl -i http://127.0.0.1:8080/demo?source=terminal
   curl -i http://127.0.0.1:8080/
   curl -i http://127.0.0.1:8080/
   ```

2. Inspect backend state and real metrics:

   ```sh
   curl http://127.0.0.1:9090/api/v1/backends
   curl http://127.0.0.1:9090/api/v1/overview
   curl 'http://127.0.0.1:9090/api/v1/metrics?window_seconds=60'
   ```

3. Change the selection policy:

   ```sh
   curl -i -X PUT http://127.0.0.1:9090/api/v1/policy \
     -H 'Content-Type: application/json' \
     -d '{"policy":"least_connections"}'
   ```

4. Stop one backend with Ctrl-C. After three failed probes it becomes unhealthy; requests continue through the remaining healthy backends. Restart the backend and wait for two successful probes before it re-enters service.
5. Stop all three backends. Once health checks mark them unhealthy, a valid proxy request returns `503 Service Unavailable`. Restart a backend and verify recovery.
6. Check the process health and benchmark-unavailable contract:

   ```sh
   curl http://127.0.0.1:9090/api/v1/health
   curl http://127.0.0.1:9090/api/v1/benchmarks/latest
   ```

Stop the C service with Ctrl-C. It uses one request per connection and closes each response.

## Prototype limits

The proxy accepts only bodyless origin-form HTTP/1.1 GET requests. Request targets are limited to 4 KiB and request headers to 16 KiB. Upstream responses are buffered up to 64 KiB total before forwarding. The service binds only to loopback and only uses the three configured backend ports. See `docs/requirements.md`, `docs/architecture.md`, `docs/api-contract.md`, and `docs/test-plan.md` for limits, metrics semantics, error mappings, and test coverage.
