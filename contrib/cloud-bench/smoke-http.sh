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

echo "smart HTTP smoke test passed"
