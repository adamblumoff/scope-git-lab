#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd -P)
trash=$(mktemp -d "${TMPDIR:-/tmp}/cloud-bench-http.XXXXXX")
server_pid=

cleanup () {
	if test -n "$server_pid"
	then
		kill "$server_pid" 2>/dev/null || :
		wait "$server_pid" 2>/dev/null || :
	fi
	rm -rf "$trash"
}
trap cleanup EXIT HUP INT TERM

port=$(python3 - <<'PY'
import socket

with socket.socket() as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)

CLOUD_BENCH_REPO_ROOT=$trash/repos \
CLOUD_BENCH_ALLOW_PUSH=1 \
PORT=$port \
	"$script_dir/cloud-bench" serve >"$trash/server.log" 2>&1 &
server_pid=$!

attempt=0
until curl --fail --silent "http://127.0.0.1:$port/healthz" >"$trash/health"
do
	attempt=$((attempt + 1))
	if test "$attempt" -ge 50 || ! kill -0 "$server_pid" 2>/dev/null
	then
		cat "$trash/server.log" >&2
		exit 1
	fi
	sleep 0.1
done

printf '{"status":"ok"}\n' >"$trash/expect-health"
cmp "$trash/expect-health" "$trash/health"

url=http://127.0.0.1:$port/bench.git
bad_status=$(curl --path-as-is --silent --output /dev/null --write-out '%{http_code}' \
	"$url/../other.git/info/refs?service=git-upload-pack")
test "$bad_status" = 404

GIT_TRACE_PACKET=$trash/packet.trace \
	git -c protocol.version=2 ls-remote "$url" >/dev/null
grep "version 2" "$trash/packet.trace" >/dev/null

git clone --quiet "$url" "$trash/client"
git -C "$trash/client" config user.name "Cloud Bench"
git -C "$trash/client" config user.email "cloud-bench@example.com"
echo smoke >"$trash/client/smoke"
git -C "$trash/client" add smoke
git -C "$trash/client" commit --quiet -m smoke
git -C "$trash/client" push --quiet origin HEAD:main

git clone --quiet "$url" "$trash/check"
test "$(cat "$trash/check/smoke")" = smoke

kill "$server_pid"
wait "$server_pid" 2>/dev/null || :
server_pid=

CLOUD_BENCH_REPO_ROOT=$trash/repos \
CLOUD_BENCH_ALLOW_PUSH=1 \
CLOUD_BENCH_MAX_BUFFERED_REQUEST_BYTES=8 \
CLOUD_BENCH_MAX_CONCURRENT_REQUESTS=2 \
CLOUD_BENCH_REQUEST_TIMEOUT_SECONDS=1 \
PORT=$port \
	"$script_dir/cloud-bench" serve >"$trash/limited-server.log" 2>&1 &
server_pid=$!

attempt=0
until curl --fail --silent "http://127.0.0.1:$port/healthz" >/dev/null
do
	attempt=$((attempt + 1))
	if test "$attempt" -ge 50 || ! kill -0 "$server_pid" 2>/dev/null
	then
		cat "$trash/limited-server.log" >&2
		exit 1
	fi
	sleep 0.1
done

python3 - "$port" <<'PY'
import socket
import sys
import time
import urllib.request


port = int(sys.argv[1])
endpoint = b"/bench.git/git-upload-pack"


def connect():
	return socket.create_connection(("127.0.0.1", port), timeout=2)


def start_slow_body(length):
	sock = connect()
	sock.sendall(
		b"POST " + endpoint + b" HTTP/1.1\r\n"
		b"Host: localhost\r\n"
		b"Content-Type: application/x-git-upload-pack-request\r\n"
		b"Content-Length: " + str(length).encode() + b"\r\n\r\n"
	)
	return sock


def status(sock):
	return int(sock.recv(64).split(b" ", 2)[1])


reserved = start_slow_body(8)
time.sleep(0.1)
overflow = connect()
overflow.sendall(
    b"POST " + endpoint + b" HTTP/1.1\r\n"
    b"Host: localhost\r\n"
    b"Content-Type: application/x-git-upload-pack-request\r\n"
    b"Content-Length: 1\r\n\r\nx"
)
assert status(overflow) == 503
overflow.close()
reserved.close()
time.sleep(0.1)

slow_one = start_slow_body(4)
slow_two = start_slow_body(4)
time.sleep(0.1)
excess = connect()
excess.sendall(b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")
assert status(excess) == 503
excess.close()

assert status(slow_one) == 408
assert status(slow_two) == 408
slow_one.close()
slow_two.close()
with urllib.request.urlopen(f"http://127.0.0.1:{port}/healthz", timeout=2) as response:
	assert response.status == 200
PY

echo "smart HTTP smoke test passed"
