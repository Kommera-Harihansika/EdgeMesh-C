#!/usr/bin/env python3
"""Small integration-test upstream used to exercise incomplete and valid error responses."""
import socketserver
import sys
import time


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        data = b""
        self.request.settimeout(2)
        while b"\r\n\r\n" not in data and len(data) < 16384:
            chunk = self.request.recv(4096)
            if not chunk:
                return
            data += chunk
        first = data.split(b"\r\n", 1)[0]
        if first.startswith(b"GET /health "):
            body = b"{}"
            response = b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n" + body
        elif first.startswith(b"GET /broken "):
            response = b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\nx"
        elif first.startswith(b"GET /status "):
            body = b"unavailable"
            response = b"HTTP/1.1 503 Service Unavailable\r\nContent-Length: 11\r\nConnection: close\r\n\r\n" + body
        elif first.startswith(b"GET /slow "):
            time.sleep(3)
            body = b"late"
            response = b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\n" + body
        else:
            body = b"fault-backend"
            response = b"HTTP/1.1 200 OK\r\nContent-Length: 13\r\nConnection: close\r\n\r\n" + body
        try:
            self.request.sendall(response)
        except OSError:
            pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    server = Server(("127.0.0.1", int(sys.argv[1])), Handler)
    try:
        server.serve_forever(poll_interval=0.1)
    finally:
        server.server_close()
