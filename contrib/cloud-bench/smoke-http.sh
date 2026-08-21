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
CLOUD_BENCH_RESULTS_DIR=$trash/results \
CLOUD_BENCH_ALLOW_PUSH=1 \
CLOUD_BENCH_CLOUD_ODB=0 \
CLOUD_BENCH_LATEST_RESULT=$script_dir/results/railway-cloud-odb-matrix-2026-08-20.json \
AWS_SESSION_TOKEN=session-token-smoke \
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
result_status=$(curl --silent --output /dev/null --write-out '%{http_code}' \
	"http://127.0.0.1:$port/results/latest.json")
test "$result_status" = 200

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
printf '%s\n' \
	'#!/bin/sh' \
	'test "$AWS_SESSION_TOKEN" = session-token-smoke || exit 1' \
	'test -n "$GIT_CLOUD_ODB_METRICS_RUN" || exit 0' \
	'printf "%s\\n" "$GIT_CLOUD_ODB_METRICS_RUN" >"$GIT_DIR/metrics-run"' \
	>"$trash/repos/bench.git/hooks/post-receive"
chmod +x "$trash/repos/bench.git/hooks/post-receive"
echo smoke >"$trash/client/smoke"
git -C "$trash/client" add smoke
git -C "$trash/client" commit --quiet -m smoke
metrics_run=0123456789abcdef0123456789abcdef
mkdir -p "$trash/results/process.ndjson.active"
: >"$trash/results/process.ndjson.active/$metrics_run"
git -C "$trash/client" \
	-c "http.extraHeader=X-Cloud-Odb-Metrics-Run: $metrics_run" \
	push --quiet origin HEAD:main
test "$(cat "$trash/repos/bench.git/metrics-run")" = "$metrics_run"
rm "$trash/results/process.ndjson.active/$metrics_run"

base=$(git -C "$trash/client" rev-parse HEAD) &&
tree=$(git -C "$trash/client" rev-parse HEAD^{tree}) &&
i=0 &&
while test "$i" -lt 50
do
	commit=$(printf 'gzip request %s\n' "$i" |
		git -C "$trash/client" commit-tree "$tree" -p "$base") &&
	git -C "$trash/client" update-ref "refs/heads/gzip-$i" "$commit" &&
	i=$((i + 1))
done &&
git -C "$trash/client" push --quiet --all origin

GIT_TRACE_CURL=$trash/curl.trace git clone --quiet "$url" "$trash/check"
test "$(cat "$trash/check/smoke")" = smoke
grep 'Content-Encoding: gzip' "$trash/curl.trace" >/dev/null

kill "$server_pid"
wait "$server_pid" 2>/dev/null || :
server_pid=

set +e
CLOUD_BENCH_REPO_ROOT=$trash/partial-repos \
CLOUD_BENCH_RESULTS_DIR=$trash/partial-results \
AWS_S3_BUCKET_NAME=partial-only \
PORT=$port \
	"$script_dir/cloud-bench" serve >"$trash/partial-s3.log" 2>&1
partial_status=$?
set -e
test "$partial_status" = 2
grep "partial AWS S3 environment" "$trash/partial-s3.log" >/dev/null
test ! -e "$trash/partial-repos/bench.git"

expect_invalid_s3 () {
	label=$1
	endpoint=$2
	bucket=$3
	style=$4
	set +e
	env \
		CLOUD_BENCH_REPO_ROOT="$trash/invalid-$label-repos" \
		CLOUD_BENCH_RESULTS_DIR="$trash/invalid-$label-results" \
		CLOUD_BENCH_CLOUD_ODB=1 \
		AWS_ENDPOINT_URL="$endpoint" \
		AWS_ACCESS_KEY_ID=invalid-access \
		AWS_SECRET_ACCESS_KEY=invalid-secret \
		AWS_S3_BUCKET_NAME="$bucket" \
		AWS_DEFAULT_REGION=us-east-1 \
		AWS_S3_URL_STYLE="$style" \
		PORT=$port \
		timeout 2 "$script_dir/cloud-bench" serve \
			>"$trash/invalid-$label.log" 2>&1
	invalid_status=$?
	set -e
	test "$invalid_status" -ne 0
	test ! -e "$trash/invalid-$label-repos/bench.git"
}

expect_invalid_s3 style https://example.invalid valid-bucket bogus
expect_invalid_s3 bucket https://example.invalid invalid_bucket path
expect_invalid_s3 short-bucket https://example.invalid ab path
expect_invalid_s3 uppercase-bucket https://example.invalid Invalid-bucket path
expect_invalid_s3 leading-hyphen https://example.invalid -invalid path
expect_invalid_s3 trailing-hyphen https://example.invalid invalid- path
expect_invalid_s3 adjacent-periods https://example.invalid invalid..bucket path
expect_invalid_s3 ip-address https://example.invalid 192.168.5.4 path
expect_invalid_s3 reserved-prefix https://example.invalid xn--invalid path
expect_invalid_s3 reserved-suffix https://example.invalid invalid--x-s3 path
expect_invalid_s3 endpoint http://example.invalid valid-bucket path

CLOUD_BENCH_REPO_ROOT=$trash/marker-repos \
CLOUD_BENCH_RESULTS_DIR=$trash/marker-results \
CLOUD_BENCH_CLOUD_ODB=1 \
CLOUD_BENCH_RUN_PREFIX=matrix/preserved-marker \
AWS_ENDPOINT_URL=https://example.invalid \
AWS_ACCESS_KEY_ID=marker-access \
AWS_SECRET_ACCESS_KEY=marker-secret \
AWS_S3_BUCKET_NAME=marker-smoke \
AWS_DEFAULT_REGION=us-east-1 \
AWS_S3_URL_STYLE=path \
PORT=$port \
	"$script_dir/cloud-bench" serve >"$trash/marker-server.log" 2>&1 &
server_pid=$!

attempt=0
until curl --fail --silent "http://127.0.0.1:$port/healthz" >/dev/null
do
	attempt=$((attempt + 1))
	if test "$attempt" -ge 50 || ! kill -0 "$server_pid" 2>/dev/null
	then
		cat "$trash/marker-server.log" >&2
		exit 1
	fi
	sleep 0.1
done

kill "$server_pid"
wait "$server_pid" 2>/dev/null || :
server_pid=
cp "$trash/marker-repos/bench.git/objects/cloud-odb" "$trash/marker-before"

printf 'unexpected trailing directive\n' >> \
	"$trash/marker-repos/bench.git/objects/cloud-odb"
cp "$trash/marker-repos/bench.git/objects/cloud-odb" "$trash/marker-tainted"

set +e
CLOUD_BENCH_REPO_ROOT=$trash/marker-repos \
CLOUD_BENCH_RESULTS_DIR=$trash/marker-results \
CLOUD_BENCH_CLOUD_ODB=1 \
CLOUD_BENCH_RUN_PREFIX=matrix/preserved-marker \
AWS_ENDPOINT_URL=https://example.invalid \
AWS_ACCESS_KEY_ID=marker-access \
AWS_SECRET_ACCESS_KEY=marker-secret \
AWS_S3_BUCKET_NAME=marker-smoke \
AWS_DEFAULT_REGION=us-east-1 \
AWS_S3_URL_STYLE=path \
PORT=$port \
	timeout 2 "$script_dir/cloud-bench" serve \
		>"$trash/marker-trailing.log" 2>&1
marker_status=$?
set -e
test "$marker_status" = 1
grep "marker does not match" "$trash/marker-trailing.log" >/dev/null
cmp "$trash/marker-tainted" \
	"$trash/marker-repos/bench.git/objects/cloud-odb"
cp "$trash/marker-before" \
	"$trash/marker-repos/bench.git/objects/cloud-odb"

set +e
CLOUD_BENCH_REPO_ROOT=$trash/marker-repos \
CLOUD_BENCH_RESULTS_DIR=$trash/marker-results \
CLOUD_BENCH_CLOUD_ODB=1 \
CLOUD_BENCH_RUN_PREFIX=matrix/different-marker \
AWS_ENDPOINT_URL=https://example.invalid \
AWS_ACCESS_KEY_ID=marker-access \
AWS_SECRET_ACCESS_KEY=marker-secret \
AWS_S3_BUCKET_NAME=marker-smoke \
AWS_DEFAULT_REGION=us-east-1 \
AWS_S3_URL_STYLE=path \
PORT=$port \
	timeout 2 "$script_dir/cloud-bench" serve \
		>"$trash/marker-mismatch.log" 2>&1
marker_status=$?
set -e
test "$marker_status" = 1
grep "marker does not match" "$trash/marker-mismatch.log" >/dev/null
cmp "$trash/marker-before" \
	"$trash/marker-repos/bench.git/objects/cloud-odb"

set +e
CLOUD_BENCH_REPO_ROOT=$trash/marker-repos \
CLOUD_BENCH_RESULTS_DIR=$trash/marker-results \
CLOUD_BENCH_CLOUD_ODB=1 \
CLOUD_BENCH_RUN_PREFIX=matrix/preserved-marker \
AWS_ENDPOINT_URL=https://example.invalid \
AWS_ACCESS_KEY_ID=marker-access \
AWS_SECRET_ACCESS_KEY=marker-secret \
AWS_S3_BUCKET_NAME=different-marker-smoke \
AWS_DEFAULT_REGION=us-east-1 \
AWS_S3_URL_STYLE=path \
PORT=$port \
	timeout 2 "$script_dir/cloud-bench" serve \
		>"$trash/marker-storage-mismatch.log" 2>&1
marker_status=$?
set -e
test "$marker_status" = 1
grep "marker does not match" "$trash/marker-storage-mismatch.log" >/dev/null
cmp "$trash/marker-before" \
	"$trash/marker-repos/bench.git/objects/cloud-odb"

rm "$trash/marker-repos/bench.git/objects/cloud-odb"
ln -s "$trash/dangling-marker-target" \
	"$trash/marker-repos/bench.git/objects/cloud-odb"
set +e
CLOUD_BENCH_REPO_ROOT=$trash/marker-repos \
CLOUD_BENCH_RESULTS_DIR=$trash/marker-results \
CLOUD_BENCH_CLOUD_ODB=1 \
CLOUD_BENCH_RUN_PREFIX=matrix/preserved-marker \
AWS_ENDPOINT_URL=https://example.invalid \
AWS_ACCESS_KEY_ID=marker-access \
AWS_SECRET_ACCESS_KEY=marker-secret \
AWS_S3_BUCKET_NAME=marker-smoke \
AWS_DEFAULT_REGION=us-east-1 \
AWS_S3_URL_STYLE=path \
PORT=$port \
	timeout 2 "$script_dir/cloud-bench" serve \
		>"$trash/marker-symlink.log" 2>&1
marker_status=$?
set -e
test "$marker_status" = 1
grep "marker must be a regular file" "$trash/marker-symlink.log" >/dev/null
test ! -e "$trash/dangling-marker-target"

set +e
CLOUD_BENCH_REPO_ROOT=$trash/marker-repos \
CLOUD_BENCH_RESULTS_DIR=$trash/marker-results \
CLOUD_BENCH_CLOUD_ODB=0 \
PORT=$port \
	timeout 2 "$script_dir/cloud-bench" serve \
		>"$trash/marker-disabled-symlink.log" 2>&1
marker_status=$?
set -e
test "$marker_status" = 1
grep "refusing to disable" "$trash/marker-disabled-symlink.log" >/dev/null
test ! -e "$trash/dangling-marker-target"

CLOUD_BENCH_REPO_ROOT=$trash/repos \
CLOUD_BENCH_RESULTS_DIR=$trash/results \
CLOUD_BENCH_ALLOW_PUSH=0 \
CLOUD_BENCH_CLOUD_ODB=0 \
CLOUD_BENCH_MAX_BUFFERED_REQUEST_BYTES=8 \
CLOUD_BENCH_MAX_CONCURRENT_REQUESTS=2 \
CLOUD_BENCH_REQUEST_TIMEOUT_SECONDS=1 \
CLOUD_BENCH_MAX_RESULT_BYTES=8 \
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

printf 123456789 >"$trash/results/latest.json"
result_status=$(curl --silent --output /dev/null --write-out '%{http_code}' \
	"http://127.0.0.1:$port/results/latest.json")
test "$result_status" = 413

if python3 "$script_dir/matrix.py" \
	--url "http://127.0.0.1:$port" \
	--writers 1 \
	--warmups 0 \
	--samples 1 \
	--payload-bytes 16 \
	--skip-failpoints \
	--output "$trash/rejected-matrix.json" >"$trash/rejected-matrix.stdout"
then
	echo >&2 "matrix accepted a run whose seed and writers were rejected"
	exit 1
fi
python3 - "$trash/rejected-matrix.json" <<'PY'
import json
import sys


with open(sys.argv[1], encoding="utf-8") as stream:
	result = json.load(stream)
assert result["ok"] is False
assert result["layouts"][0]["seed"]["success"] is False
assert result["layouts"][0]["writerLevels"][0]["correctnessPassed"] is False
PY

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


def start_slow_header():
	sock = connect()
	sock.sendall(
		b"POST " + endpoint + b" HTTP/1.1\r\n"
		b"Host: localhost\r\n"
		b"X-Slow: "
	)
	return sock


def status(sock):
	return int(sock.recv(64).split(b" ", 2)[1])


headers_one = start_slow_header()
headers_two = start_slow_header()
time.sleep(0.1)
excess = connect()
excess.sendall(b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")
assert status(excess) == 503
excess.close()
for unused in range(3):
	time.sleep(0.35)
	for sock in (headers_one, headers_two):
		try:
			sock.sendall(b"x")
		except OSError:
			pass
headers_one.close()
headers_two.close()
with urllib.request.urlopen(f"http://127.0.0.1:{port}/healthz", timeout=2) as response:
	assert response.status == 200

trickle = start_slow_body(4)
for unused in range(3):
	time.sleep(0.35)
	try:
		trickle.sendall(b"x")
	except BrokenPipeError:
		break
assert status(trickle) == 408
trickle.close()
time.sleep(0.1)

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

python3 - "$script_dir/matrix.py" <<'PY'
import concurrent.futures
import importlib.util
import pathlib
import sys
import tempfile
import threading


spec = importlib.util.spec_from_file_location("cloud_matrix", sys.argv[1])
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)
samples = [
	{"writersRaw": [{"success": True, "latencyMs": 1.0}]},
	{
		"writersRaw": [
			{"success": False, "latencyMs": 2.0},
			{"success": True, "latencyMs": 10.0},
			{"success": True, "latencyMs": 100.0},
		]
	},
]
latencies = matrix.successful_writer_latencies(samples)
assert latencies == [1.0, 10.0, 100.0]
assert matrix.percentile(latencies, 0.50) == 10.0

with tempfile.TemporaryDirectory() as directory:
	output = pathlib.Path(directory) / "latest.json"
	payloads = ['{"run":1}\n', '{"run":2}\n']
	barrier = threading.Barrier(2)

	def publish(payload):
		barrier.wait()
		matrix.atomic_write_result(output, payload)

	with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
		list(executor.map(publish, payloads))
	assert output.read_text() in payloads
	assert not list(output.parent.glob(".latest.json.*.tmp"))

	metrics = pathlib.Path(directory) / "process.ndjson"
	run_one = "1" * 32
	run_two = "2" * 32
	metrics.write_text(
		'{"runId":"' + run_one + '","puts":1}\n'
		'{"runId":"' + run_two + '","puts":50}\n'
	)
	cursor = matrix.MetricsCursor(metrics, run_one)
	assert matrix.summarize_metrics(cursor.read(0))["puts"] == 1
	assert matrix.redact_url_userinfo("https://user:token@example.com/root?x=1") == \
		"https://example.com/root?x=1"

	metrics_run = matrix.MetricsRun(metrics, run_one)
	assert metrics_run.path == pathlib.Path(f"{metrics}.{run_one}")
	assert metrics_run.active.is_file()
	metrics_run.path.write_text("metric\n")
	metrics_run.cleanup()
	assert not metrics_run.active.exists()
	assert not metrics_run.path.exists()

with tempfile.TemporaryDirectory() as directory:
	first = matrix.Corpus(directory, "1" * 32, 16)
	first_root = first.root_commit
with tempfile.TemporaryDirectory() as directory:
	second = matrix.Corpus(directory, "2" * 32, 16)
	assert second.root_commit != first_root


class Corpus:
	repo = "repo"

	def commit(self, *args, **kwargs):
		return "commit"


class Cursor:
	def mark(self):
		return 0

	def read(self, mark):
		return []


matrix.push_one = lambda *args, **kwargs: {
	"success": False,
	"stderrClass": "backend-crash",
}
matrix.visible_refs = lambda *args: (None, {}, "timeout")
failpoints = matrix.run_failpoints(Corpus(), Cursor(), "url", "run", 0)
assert all(not item["passed"] for item in failpoints)
assert all(item["refInspectionExitCode"] is None for item in failpoints)
PY

kill "$server_pid"
wait "$server_pid" 2>/dev/null || :
server_pid=

mkdir -p "$trash/segment-activation-repos"
cp -R "$trash/repos/bench.git" \
	"$trash/segment-activation-repos/bench.git"
git -C "$trash/segment-activation-repos/bench.git" segment-store import
segment_head=$(git -C "$trash/segment-activation-repos/bench.git" rev-parse main)
set +e
CLOUD_BENCH_REPO_ROOT=$trash/segment-activation-repos \
CLOUD_BENCH_RESULTS_DIR=$trash/segment-activation-results \
CLOUD_BENCH_CLOUD_ODB=1 \
CLOUD_BENCH_RUN_PREFIX=matrix/segment-activation \
AWS_ENDPOINT_URL=https://example.invalid \
AWS_ACCESS_KEY_ID=segment-access \
AWS_SECRET_ACCESS_KEY=segment-secret \
AWS_S3_BUCKET_NAME=segment-smoke \
AWS_DEFAULT_REGION=us-east-1 \
AWS_S3_URL_STYLE=path \
PORT=$port \
	timeout 2 "$script_dir/cloud-bench" serve \
		>"$trash/segment-activation.log" 2>&1
segment_status=$?
set -e
test "$segment_status" = 1
grep "refusing to hide an existing segment store" \
	"$trash/segment-activation.log" >/dev/null
test ! -e "$trash/segment-activation-repos/bench.git/objects/cloud-odb"
test "$(git -C "$trash/segment-activation-repos/bench.git" rev-parse main)" = \
	"$segment_head"

CLOUD_BENCH_REPO_ROOT=$trash/custom-repos \
CLOUD_BENCH_RESULTS_DIR=$trash/custom-results \
CLOUD_BENCH_REPO_NAME=custom.git \
CLOUD_BENCH_ALLOW_PUSH=1 \
CLOUD_BENCH_CLOUD_ODB=0 \
PORT=$port \
	"$script_dir/cloud-bench" serve >"$trash/custom-server.log" 2>&1 &
server_pid=$!
attempt=0
until curl --fail --silent "http://127.0.0.1:$port/healthz" >/dev/null
do
	attempt=$((attempt + 1))
	if test "$attempt" -ge 50 || ! kill -0 "$server_pid" 2>/dev/null
	then
		cat "$trash/custom-server.log" >&2
		exit 1
	fi
	sleep 0.1
done
set +e
python3 "$script_dir/matrix.py" --url "http://127.0.0.1:$port" \
	--repository custom.git --writers 1 --warmups 0 --samples 1 \
	--payload-bytes 128 --skip-failpoints \
	--metrics "$trash/custom-metrics.ndjson" \
	--output "$trash/custom-result.json" >/dev/null
matrix_status=$?
set -e
test "$matrix_status" = 1
python3 - "$trash/custom-result.json" <<'PY'
import json
import sys


result = json.load(open(sys.argv[1]))
layout = result["layouts"][0]
assert layout["url"].endswith("/custom.git")
assert layout["seed"]["success"] is True
assert layout["cloudEvidencePassed"] is False
assert result["cloudMeasured"] is False
assert result["ok"] is False
PY

echo "smart HTTP smoke test passed"
