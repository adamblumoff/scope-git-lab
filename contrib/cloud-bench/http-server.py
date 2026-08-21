#!/usr/bin/env python3

import os
import re
import shutil
import socket
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlsplit


def env_bool(name, default=False):
    value = os.environ.get(name)
    if value is None:
        return default
    normalized = value.lower()
    if normalized in ("1", "true", "yes"):
        return True
    if normalized in ("0", "false", "no"):
        return False
    raise ValueError(f"{name} must be true or false")


REPO_ROOT = os.environ["GIT_HTTP_ROOT"]
REPO_NAMES = tuple(os.environ["GIT_HTTP_REPO_NAMES"].split(":"))
REPO_PREFIXES = tuple(f"/{name}" for name in REPO_NAMES)
MAX_REQUEST_BYTES = int(
    os.environ.get("CLOUD_BENCH_MAX_REQUEST_BYTES", str(1024 * 1024 * 1024))
)
MAX_BUFFERED_REQUEST_BYTES = int(
    os.environ.get("CLOUD_BENCH_MAX_BUFFERED_REQUEST_BYTES", str(MAX_REQUEST_BYTES))
)
MAX_RECEIVE_RESPONSE_BYTES = int(
    os.environ.get("CLOUD_BENCH_MAX_RECEIVE_RESPONSE_BYTES", str(16 * 1024 * 1024))
)
MAX_RESULT_BYTES = int(
    os.environ.get("CLOUD_BENCH_MAX_RESULT_BYTES", str(64 * 1024 * 1024))
)
MAX_CONCURRENT_REQUESTS = int(
    os.environ.get("CLOUD_BENCH_MAX_CONCURRENT_REQUESTS", "64")
)
REQUEST_TIMEOUT_SECONDS = float(
    os.environ.get("CLOUD_BENCH_REQUEST_TIMEOUT_SECONDS", "30")
)
ALLOW_FAILPOINTS = env_bool("CLOUD_BENCH_ALLOW_FAILPOINTS")

if min(
    MAX_REQUEST_BYTES,
    MAX_BUFFERED_REQUEST_BYTES,
    MAX_RECEIVE_RESPONSE_BYTES,
    MAX_RESULT_BYTES,
    MAX_CONCURRENT_REQUESTS,
) <= 0:
    raise ValueError("request limits must be positive")
if REQUEST_TIMEOUT_SECONDS <= 0:
    raise ValueError("request timeout must be positive")


class GitHandler(BaseHTTPRequestHandler):
    server_version = "scope-git-cloud-bench"
    sys_version = ""

    def setup(self):
        super().setup()
        self._input_deadline = time.monotonic() + REQUEST_TIMEOUT_SECONDS
        self._input_expired = False
        self._input_complete = False
        self._input_lock = threading.Lock()
        self._input_timer = threading.Timer(
            REQUEST_TIMEOUT_SECONDS, self._expire_request_input
        )
        self._input_timer.daemon = True
        self._input_timer.start()
        self.connection.settimeout(REQUEST_TIMEOUT_SECONDS)

    def finish(self):
        self._finish_request_input()
        super().finish()

    def _expire_request_input(self):
        with self._input_lock:
            if self._input_complete:
                return
            self._input_expired = True
        try:
            self.connection.shutdown(socket.SHUT_RD)
        except OSError:
            pass

    def _finish_request_input(self):
        with self._input_lock:
            self._input_complete = True
        self._input_timer.cancel()

    def do_HEAD(self):
        self._finish_request_input()
        if urlsplit(self.path).path == "/healthz":
            self._send_health(include_body=False)
        else:
            self.send_error(404)

    def do_GET(self):
        self._finish_request_input()
        request_path = urlsplit(self.path).path
        if request_path == "/healthz":
            self._send_health(include_body=True)
            return
        if request_path == "/results/latest.json":
            self._send_latest_result()
            return
        self._serve_git()

    def do_POST(self):
        self._serve_git()

    def _send_health(self, include_body):
        body = b'{"status":"ok"}\n'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if include_body:
            self.wfile.write(body)

    def _send_latest_result(self):
        path = os.environ.get("CLOUD_BENCH_LATEST_RESULT", "/results/latest.json")
        try:
            snapshot = None
            result_size = 0
            for _ in range(3):
                candidate = tempfile.TemporaryFile()
                with open(path, "rb") as result:
                    before = os.fstat(result.fileno())
                    result_size = 0
                    while True:
                        chunk = result.read(64 * 1024)
                        if not chunk:
                            break
                        result_size += len(chunk)
                        if result_size > MAX_RESULT_BYTES:
                            candidate.close()
                            self.send_error(413)
                            return
                        candidate.write(chunk)
                    after = os.fstat(result.fileno())
                identity = lambda stat: (
                    stat.st_dev,
                    stat.st_ino,
                    stat.st_size,
                    stat.st_mtime_ns,
                    stat.st_ctime_ns,
                )
                if identity(before) == identity(after):
                    snapshot = candidate
                    break
                candidate.close()
            if snapshot is None:
                self.send_error(503, "result changed while being read")
                return
            with snapshot:
                snapshot.seek(0)
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(result_size))
                self.end_headers()
                shutil.copyfileobj(snapshot, self.wfile, length=64 * 1024)
        except FileNotFoundError:
            self.send_error(404)
            return

    def _serve_git(self):
        target = urlsplit(self.path)
        path_info = unquote(target.path)
        repo_prefix = next(
            (prefix for prefix in REPO_PREFIXES if path_info.startswith(prefix)),
            None,
        )
        endpoint = path_info.removeprefix(repo_prefix) if repo_prefix else ""
        if repo_prefix is None or endpoint not in (
            "/info/refs",
            "/git-upload-pack",
            "/git-receive-pack",
        ):
            self.send_error(404)
            return

        process = None
        request_body = None
        reserved_bytes = 0
        try:
            if self.command == "POST":
                request = self._read_request_body()
                if request is None:
                    return
                request_body, reserved_bytes = request
                self._finish_request_input()

            env = self._cgi_environment(path_info, target.query)
            process = subprocess.Popen(
                ["git", "http-backend"],
                stdin=request_body if request_body is not None else subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                env=env,
            )
            self._forward_cgi_response(process)
        except (BrokenPipeError, ConnectionResetError):
            if process is not None:
                process.terminate()
                process.wait()
        except OSError as error:
            self.log_error("git http-backend failed: %s", error)
            self.send_error(502)
        finally:
            try:
                if request_body is not None:
                    request_body.close()
            finally:
                if reserved_bytes:
                    self.server.release_body_bytes(reserved_bytes)

    def _read_request_body(self):
        value = self.headers.get("Content-Length")
        try:
            length = int(value) if value is not None else -1
        except ValueError:
            length = -1
        if length < 0:
            self.send_error(411)
            return None
        if length > MAX_REQUEST_BYTES:
            self.send_error(413)
            return None
        if not self.server.reserve_body_bytes(length):
            self.send_error(503, "request-body capacity exhausted")
            return None

        body = None
        try:
            body = tempfile.TemporaryFile()
            remaining = length
            while remaining:
                timeout = self._input_deadline - time.monotonic()
                if timeout <= 0:
                    raise TimeoutError
                self.connection.settimeout(timeout)
                chunk = self.rfile.read1(min(remaining, 64 * 1024))
                if not chunk:
                    try:
                        body.close()
                    finally:
                        self.server.release_body_bytes(length)
                    self.send_error(408 if self._input_expired else 400)
                    return None
                body.write(chunk)
                remaining -= len(chunk)
            self.connection.settimeout(REQUEST_TIMEOUT_SECONDS)
        except (socket.timeout, TimeoutError):
            self.connection.settimeout(REQUEST_TIMEOUT_SECONDS)
            try:
                body.close()
            finally:
                self.server.release_body_bytes(length)
            self.send_error(408)
            return None
        except Exception:
            try:
                if body is not None:
                    body.close()
            finally:
                self.server.release_body_bytes(length)
            raise
        body.seek(0)
        return body, length

    def _cgi_environment(self, path_info, query):
        env = {
            "GATEWAY_INTERFACE": "CGI/1.1",
            "GIT_HTTP_EXPORT_ALL": "1",
            "GIT_PROJECT_ROOT": REPO_ROOT,
            "HOME": os.environ.get("HOME", "/tmp"),
            "PATH": os.environ.get("PATH", ""),
            "PATH_INFO": path_info,
            "QUERY_STRING": query,
            "REMOTE_ADDR": self.client_address[0],
            "REQUEST_METHOD": self.command,
            "SCRIPT_NAME": "",
            "SERVER_NAME": self.server.server_name,
            "SERVER_PORT": str(self.server.server_port),
            "SERVER_PROTOCOL": self.request_version,
        }
        content_length = self.headers.get("Content-Length")
        content_type = self.headers.get("Content-Type")
        content_encoding = self.headers.get("Content-Encoding")
        git_protocol = self.headers.get("Git-Protocol")
        if content_length is not None:
            env["CONTENT_LENGTH"] = content_length
        if content_type is not None:
            env["CONTENT_TYPE"] = content_type
        if content_encoding is not None:
            env["HTTP_CONTENT_ENCODING"] = content_encoding
        if git_protocol is not None:
            env["HTTP_GIT_PROTOCOL"] = git_protocol
        failpoint = self.headers.get("X-Cloud-Odb-Failpoint")
        allowed_failpoints = {
            "before-artifact-build",
            "before-artifact-upload",
            "after-artifact-upload",
            "before-cas",
            "after-cas",
        }
        if ALLOW_FAILPOINTS and failpoint in allowed_failpoints:
            env["GIT_TEST_CLOUD_ODB_FAILPOINT"] = failpoint
        metrics_run = self.headers.get("X-Cloud-Odb-Metrics-Run")
        if metrics_run is not None and re.fullmatch(r"[0-9a-f]{32}", metrics_run):
            env["GIT_CLOUD_ODB_METRICS_RUN"] = metrics_run
        for name in (
            "AWS_ENDPOINT_URL",
            "AWS_ACCESS_KEY_ID",
            "AWS_SECRET_ACCESS_KEY",
            "AWS_S3_BUCKET_NAME",
            "AWS_DEFAULT_REGION",
            "AWS_S3_URL_STYLE",
            "GIT_CLOUD_ODB_METRICS_PATH",
            "GIT_HTTP_PROXY_AUTHMETHOD",
            "GIT_SSL_NO_VERIFY",
            "GIT_SSL_CAINFO",
            "GIT_SSL_CAPATH",
            "GIT_SSL_CERT",
            "GIT_SSL_CERT_TYPE",
            "GIT_SSL_KEY",
            "GIT_SSL_KEY_TYPE",
            "GIT_SSL_CIPHER_LIST",
            "GIT_SSL_VERSION",
            "GIT_SSL_CERT_PASSWORD_PROTECTED",
            "GIT_PROXY_SSL_CAINFO",
            "GIT_PROXY_SSL_CERT",
            "GIT_PROXY_SSL_KEY",
            "GIT_PROXY_SSL_CERT_PASSWORD_PROTECTED",
            "HTTPS_PROXY",
            "https_proxy",
            "ALL_PROXY",
            "all_proxy",
            "NO_PROXY",
            "no_proxy",
        ):
            value = os.environ.get(name)
            if value is not None:
                env[name] = value
        return env

    def _forward_cgi_response(self, process):
        status = 200
        headers = []
        header_bytes = 0

        while True:
            line = process.stdout.readline(64 * 1024 + 1)
            header_bytes += len(line)
            if not line or header_bytes > 64 * 1024:
                process.terminate()
                process.wait()
                self.send_error(502)
                return
            if line in (b"\n", b"\r\n"):
                break

            try:
                name, value = line.decode("iso-8859-1").rstrip("\r\n").split(":", 1)
            except ValueError:
                process.terminate()
                process.wait()
                self.send_error(502)
                return
            if name.lower() == "status":
                status = int(value.strip().split(" ", 1)[0])
            else:
                headers.append((name, value.strip()))

        if urlsplit(self.path).path.endswith("/git-receive-pack"):
            self._buffer_receive_response(process, status, headers)
            return

        self._send_cgi_headers(status, headers)
        shutil.copyfileobj(process.stdout, self.wfile, length=64 * 1024)
        process.stdout.close()
        return_code = process.wait()
        if return_code:
            self.log_error("git http-backend exited with status %d", return_code)

    def _send_cgi_headers(self, status, headers, content_length=None):
        self.send_response(status)
        for name, value in headers:
            if name.lower() not in (
                "connection",
                "content-length",
                "status",
                "transfer-encoding",
            ):
                self.send_header(name, value)
        if content_length is not None:
            self.send_header("Content-Length", str(content_length))
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

    def _buffer_receive_response(self, process, status, headers):
        response_body = tempfile.TemporaryFile()
        response_bytes = 0
        while True:
            chunk = process.stdout.read(64 * 1024)
            if not chunk:
                break
            response_bytes += len(chunk)
            if response_bytes > MAX_RECEIVE_RESPONSE_BYTES:
                process.terminate()
                process.wait()
                response_body.close()
                self.send_error(502, "receive-pack response is too large")
                return
            response_body.write(chunk)
        process.stdout.close()
        return_code = process.wait()
        if return_code:
            self.log_error("git http-backend exited with status %d", return_code)
            response_body.close()
            self.send_error(500, "git receive-pack failed")
            return
        response_body.seek(0)
        self._send_cgi_headers(status, headers, response_bytes)
        shutil.copyfileobj(response_body, self.wfile, length=64 * 1024)
        response_body.close()


class GitHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, server_address, handler_class):
        super().__init__(server_address, handler_class)
        self._request_slots = threading.BoundedSemaphore(MAX_CONCURRENT_REQUESTS)
        self._body_bytes = 0
        self._body_lock = threading.Lock()

    def process_request(self, request, client_address):
        if not self._request_slots.acquire(blocking=False):
            try:
                try:
                    request.sendall(
                        b"HTTP/1.1 503 Service Unavailable\r\n"
                        b"Connection: close\r\n"
                        b"Content-Length: 0\r\n\r\n"
                    )
                except OSError:
                    pass
            finally:
                self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self._request_slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._request_slots.release()

    def reserve_body_bytes(self, count):
        with self._body_lock:
            if count > MAX_BUFFERED_REQUEST_BYTES - self._body_bytes:
                return False
            self._body_bytes += count
            return True

    def release_body_bytes(self, count):
        with self._body_lock:
            self._body_bytes -= count


port = int(os.environ.get("PORT", "8080"))
GitHTTPServer(("0.0.0.0", port), GitHandler).serve_forever()
