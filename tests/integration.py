#!/usr/bin/env python3
"""End-to-end smoke/failure tests for the local C proxy and six-route admin API."""
import argparse
import http.client
import json
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request


def api(path, method="GET", body=None):
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request("http://127.0.0.1:9090" + path, data=data, method=method)
    if data is not None:
        request.add_header("Content-Type", "application/json; charset=utf-8")
    with urllib.request.urlopen(request, timeout=3) as response:
        return response.status, json.loads(response.read())


def proxy(path="/"):
    conn = http.client.HTTPConnection("127.0.0.1", 8080, timeout=4)
    conn.request("GET", path, headers={"Host": "localhost"})
    response = conn.getresponse()
    result = response.status, response.read()
    conn.close()
    return result


def wait_for(predicate, timeout=10, interval=0.1):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            if predicate():
                return
        except Exception:
            pass
        time.sleep(interval)
    raise AssertionError("condition did not become true before timeout")


def raw_proxy_status(request_bytes):
    with socket.create_connection(("127.0.0.1", 8080), timeout=3) as client:
        client.sendall(request_bytes)
        data = client.recv(1024)
    return int(data.split(b" ", 2)[1])


def raw_admin_status(request_bytes):
    with socket.create_connection(("127.0.0.1", 9090), timeout=3) as client:
        client.sendall(request_bytes)
        data = client.recv(1024)
    return int(data.split(b" ", 2)[1])


def terminate(process):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--proxy", required=True)
    parser.add_argument("--backend", required=True)
    args = parser.parse_args()
    root = os.path.dirname(__file__)
    backends = []
    fault = None
    app = None
    held_proxy = []
    held_admin = []
    try:
        for port, name in ((9001, "backend-1"), (9002, "backend-2"), (9003, "backend-3")):
            backends.append(subprocess.Popen([args.backend, str(port), name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        app = subprocess.Popen([args.proxy], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        wait_for(lambda: api("/api/v1/health")[0] == 200)
        wait_for(lambda: api("/api/v1/backends")[1]["backends"] and all(
            b["state"] == "healthy" for b in api("/api/v1/backends")[1]["backends"]))

        # Exercise all six existing routes and preserve the benchmark endpoint's unavailable state.
        assert api("/api/v1/health")[1]["status"] == "ok"
        assert len(api("/api/v1/backends")[1]["backends"]) == 3
        assert api("/api/v1/overview")[1]["policy"] == "round_robin"
        assert "points" in api("/api/v1/metrics?window_seconds=10")[1]
        assert api("/api/v1/benchmarks/latest")[1] == {"available": False, "run": None}
        assert api("/api/v1/policy", "PUT", {"policy": "least_connections"})[1]["updated"]
        assert api("/api/v1/policy", "PUT", {"policy": "round_robin"})[1]["updated"]
        try:
            api("/api/v1/policy", "PUT", {"policy": "unknown"})
            raise AssertionError("invalid policy unexpectedly succeeded")
        except urllib.error.HTTPError as error:
            assert error.code == 400
        try:
            api("/api/v1/metrics?window_seconds=9")
            raise AssertionError("invalid metrics window unexpectedly succeeded")
        except urllib.error.HTTPError as error:
            assert error.code == 400
        try:
            api("/api/v1/unknown")
            raise AssertionError("unknown route unexpectedly succeeded")
        except urllib.error.HTTPError as error:
            assert error.code == 404
        assert raw_admin_status(b"GET /api/v1/health HTTP/1.1\r\nHost: local\r\nContent-Length: 4097\r\n\r\n") == 413
        assert raw_admin_status(b"POST /api/v1/health HTTP/1.1\r\nHost: local\r\n\r\n") == 405

        # Sequential round-robin requests reach each healthy backend.
        ids = []
        for _ in range(3):
            status, body = proxy()
            assert status == 200
            ids.append(json.loads(body)["backend"])
        assert len(set(ids)) == 3, ids

        # A malformed/incomplete upstream response is buffered, rejected, and retried elsewhere.
        terminate(backends[0])  
        backends[0] = None
        status, body = proxy("/connect-failure")
        assert status == 200 and json.loads(body)["backend"] == "backend-2"
        fault = subprocess.Popen([sys.executable, os.path.join(root, "fault_backend.py"), "9001"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_for(lambda: api("/api/v1/backends")[1]["backends"][0]["state"] == "healthy")
        assert proxy()[0] == 200  # advance round robin from backend-3 to backend-1
        before_retry = api("/api/v1/overview")[1]["metrics"]
        before_backend = api("/api/v1/backends")[1]["backends"][0]
        status, body = proxy("/broken")
        assert status == 200 and json.loads(body)["backend"] in ("backend-2", "backend-3")
        after_retry = api("/api/v1/overview")[1]["metrics"]
        after_backend = api("/api/v1/backends")[1]["backends"][0]
        assert after_retry["requests_total"] == before_retry["requests_total"] + 1
        assert after_retry["proxy_failures_total"] == before_retry["proxy_failures_total"]
        assert after_backend["requests_total"] == before_backend["requests_total"] + 1
        assert after_backend["proxy_failures_total"] == before_backend["proxy_failures_total"] + 1
        # Advance the round-robin cursor so the next request targets test backend-1.
        # Diagnostic: identify which backend this request reaches.
        advance_status, advance_body = proxy()
        print(f"Before /status: status={advance_status}, body={advance_body!r}", flush=True)
        assert advance_status == 200
        before = {b["id"]: b["requests_total"] for b in api("/api/v1/backends")[1]["backends"]}
        status, body = proxy("/status")
        assert status == 503 and body == b"unavailable", (
        f"actual response: status={status}, body={body!r}"
        )
        after = {b["id"]: b["requests_total"] for b in api("/api/v1/backends")[1]["backends"]}
        assert after["backend-2"] + after["backend-3"] == before["backend-2"] + before["backend-3"]

        # A slow upstream is retried once to a different backend; without an alternative it maps to 504.
        assert proxy()[0] == 200
        assert proxy()[0] == 200
        status, body = proxy("/slow")
        assert status == 200 and json.loads(body)["backend"] == "backend-2"
        terminate(backends[1]); backends[1] = None
        terminate(backends[2]); backends[2] = None
        wait_for(lambda: all(b["state"] == "unhealthy" for b in api("/api/v1/backends")[1]["backends"][1:]), timeout=8)
        assert proxy("/slow")[0] == 504

        # Request errors map to the approved codes and are separate from logical requests.
        before_rejections = api("/api/v1/overview")[1]["metrics"]["requests_total"]
        assert raw_proxy_status(b"not-http\r\n\r\n") == 400
        assert raw_proxy_status(b"POST / HTTP/1.1\r\nHost: localhost\r\n\r\n") == 405
        long_target = b"/" + b"x" * 4096
        assert raw_proxy_status(b"GET " + long_target + b" HTTP/1.1\r\nHost: x\r\n\r\n") == 414
        assert raw_proxy_status(b"GET / HTTP/1.1\r\nHost: x\r\nX-Fill: " + b"x" * 16370 + b"\r\n\r\n") == 431
        overview = api("/api/v1/overview")[1]["metrics"]
        assert overview["requests_total"] >= 5
        assert overview["client_rejections_total"] >= 4
        assert overview["responses_forwarded_total"] >= 4
        assert overview["response_bytes_per_second"] > 0
        assert overview["requests_total"] == before_rejections
        time.sleep(1.2)
        assert api("/api/v1/overview")[1]["metrics"]["requests_total"] == before_rejections
        with socket.create_connection(("127.0.0.1", 8080), timeout=4) as idle:
            assert b"400" in idle.recv(512)

        # Saturate the bounded proxy and admin connection slots before parsing.
        for _ in range(32):
            held_proxy.append(socket.create_connection(("127.0.0.1", 8080), timeout=3))
        time.sleep(0.2)
        with socket.create_connection(("127.0.0.1", 8080), timeout=3) as extra:
            extra.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
            assert b"503" in extra.recv(512)
        for client in held_proxy:
            client.close()
        held_proxy.clear()
        for _ in range(8):
            held_admin.append(socket.create_connection(("127.0.0.1", 9090), timeout=3))
        time.sleep(0.2)
        with socket.create_connection(("127.0.0.1", 9090), timeout=3) as extra:
            extra.sendall(b"GET /api/v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n")
            assert b"503" in extra.recv(1024)
        for client in held_admin:
            client.close()
        held_admin.clear()
        assert api("/api/v1/overview")[1]["metrics"]["capacity_rejections_total"] >= 2

        # With all backends stopped and health thresholds elapsed, valid requests get 503.
        terminate(fault); fault = None
        backends = [None, None, None]
        wait_for(lambda: all(b["state"] == "unhealthy" for b in api("/api/v1/backends")[1]["backends"]), timeout=8)
        assert proxy()[0] == 503
        backends[1] = subprocess.Popen([args.backend, "9002", "backend-2"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_for(lambda: api("/api/v1/backends")[1]["backends"][1]["state"] == "healthy", timeout=6)
        assert proxy()[0] == 200
        print("integration tests passed")
    finally:
        for client in held_proxy + held_admin:
            client.close()
        terminate(fault)
        for process in backends:
            terminate(process)
        terminate(app)


if __name__ == "__main__":
    main()
