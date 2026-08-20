#!/usr/bin/env python3

import os
import shutil
import subprocess
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlsplit


REPO_ROOT = os.environ["GIT_HTTP_ROOT"]
REPO_NAME = os.environ["GIT_HTTP_REPO_NAME"]
REPO_PREFIX = f"/{REPO_NAME}"
MAX_REQUEST_BYTES = int(
    os.environ.get("CLOUD_BENCH_MAX_REQUEST_BYTES", str(1024 * 1024 * 1024))
)


class GitHandler(BaseHTTPRequestHandler):
    server_version = "scope-git-cloud-bench"
    sys_version = ""

    def do_HEAD(self):
        if urlsplit(self.path).path == "/healthz":
            self._send_health(include_body=False)
        else:
            self.send_error(404)

    def do_GET(self):
        if urlsplit(self.path).path == "/healthz":
            self._send_health(include_body=True)
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

    def _serve_git(self):
        target = urlsplit(self.path)
        path_info = unquote(target.path)
        endpoint = path_info.removeprefix(REPO_PREFIX)
        if not path_info.startswith(REPO_PREFIX) or endpoint not in (
            "/info/refs",
            "/git-upload-pack",
            "/git-receive-pack",
        ):
            self.send_error(404)
            return

        process = None
        request_body = None
        try:
            if self.command == "POST":
                request_body = self._read_request_body()
                if request_body is None:
                    return

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
            if request_body is not None:
                request_body.close()

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

        body = tempfile.TemporaryFile()
        remaining = length
        while remaining:
            chunk = self.rfile.read(min(remaining, 64 * 1024))
            if not chunk:
                body.close()
                self.send_error(400)
                return None
            body.write(chunk)
            remaining -= len(chunk)
        body.seek(0)
        return body

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
        git_protocol = self.headers.get("Git-Protocol")
        if content_length is not None:
            env["CONTENT_LENGTH"] = content_length
        if content_type is not None:
            env["CONTENT_TYPE"] = content_type
        if git_protocol is not None:
            env["HTTP_GIT_PROTOCOL"] = git_protocol
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

        self.send_response(status)
        for name, value in headers:
            if name.lower() not in ("connection", "status", "transfer-encoding"):
                self.send_header(name, value)
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        shutil.copyfileobj(process.stdout, self.wfile, length=64 * 1024)
        process.stdout.close()
        return_code = process.wait()
        if return_code:
            self.log_error("git http-backend exited with status %d", return_code)


class GitHTTPServer(ThreadingHTTPServer):
    daemon_threads = True


port = int(os.environ.get("PORT", "8080"))
GitHTTPServer(("0.0.0.0", port), GitHandler).serve_forever()
