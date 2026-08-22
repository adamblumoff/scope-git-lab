#!/bin/sh

set -eu

test -n "${AWS_S3_BUCKET_NAME:-}" || {
	echo >&2 "AWS_S3_BUCKET_NAME is required"
	exit 2
}

trash=$(mktemp -d "${TMPDIR:-/tmp}/cloud-odb-fallback.XXXXXX")
cleanup () {
	rm -rf "$trash"
}
trap cleanup EXIT HUP INT TERM

bare=$trash/bench.git
client=$trash/client
prefix=${CLOUD_BENCH_RUN_PREFIX:-matrix/fallback-smoke-$$}

transport_metrics=$trash/transport-metrics.ndjson
set +e
GIT_TEST_S3_ALLOW_HTTP=1 \
AWS_ENDPOINT_URL=http://127.0.0.1:1 \
AWS_ACCESS_KEY_ID=transport-access \
AWS_SECRET_ACCESS_KEY=transport-secret \
AWS_S3_BUCKET_NAME=valid-bucket \
AWS_DEFAULT_REGION=us-east-1 \
AWS_S3_URL_STYLE=path \
GIT_CLOUD_ODB_METRICS_PATH=$transport_metrics \
GIT_CLOUD_ODB_METRICS_RUN=0123456789abcdef0123456789abcdef \
	git cloud-bench probe --json \
		>"$trash/transport-probe.out" 2>"$trash/transport-probe.err"
transport_status=$?
set -e
test "$transport_status" -ne 0
grep '"requests":1' "$transport_metrics" >/dev/null
grep '"transportFailures":1' "$transport_metrics" >/dev/null
grep '"transportError":[1-9]' "$transport_metrics" >/dev/null

git init --bare --quiet --initial-branch=main "$bare"
git init --quiet --initial-branch=main "$client"
git -C "$client" config user.name "Cloud ODB fallback smoke"
git -C "$client" config user.email "cloud-odb-fallback@example.invalid"
printf 'fallback\n' >"$client/object"
git -C "$client" add object
git -C "$client" commit --quiet -m fallback
git -C "$client" push --quiet "$bare" HEAD:main
git -C "$bare" repack -ad

printf 'loose fallback\n' >>"$client/object"
git -C "$client" commit --quiet -am loose-fallback
git -C "$client" push --quiet "$bare" HEAD:main
fallback_loose_oid=$(git -C "$client" rev-parse HEAD)
fallback_loose_path=$bare/objects/$(printf '%s' "$fallback_loose_oid" | cut -c1-2)/$(printf '%s' "$fallback_loose_oid" | cut -c3-)
test -f "$fallback_loose_path"

freshen_contents='freshen fallback object'
freshen_oid=$(printf '%s' "$freshen_contents" | git -C "$bare" hash-object -w --stdin)
freshen_path=$bare/objects/$(printf '%s' "$freshen_oid" | cut -c1-2)/$(printf '%s' "$freshen_oid" | cut -c3-)
freshen_before=$(stat -c %Y "$freshen_path")

fallback_oid=$(git -C "$client" rev-parse HEAD)
storage_id=$(git cloud-bench validate-config --print-storage-id)
printf 'git-cloud-odb 2\nlayout group\nprefix %s/group\nstorage %s\n' \
	"$prefix" "$storage_id" \
	>"$bare/objects/cloud-odb"

cp "$bare/objects/cloud-odb" "$trash/cloud-odb.marker"
dd if=/dev/zero of="$bare/objects/cloud-odb" bs=1025 count=1 2>/dev/null
set +e
git -C "$bare" cat-file -e "$fallback_oid^{commit}" \
	>"$trash/marker-large.out" 2>"$trash/marker-large.err"
marker_status=$?
set -e
test "$marker_status" -ne 0
grep "cloud ODB marker .* exceeds the size limit" \
	"$trash/marker-large.err" >/dev/null
mv "$trash/cloud-odb.marker" "$bare/objects/cloud-odb"

cp "$bare/objects/cloud-odb" "$client/.git/objects/cloud-odb"
git -C "$client" ls-files --stage >"$trash/index-before-failed-commit"
printf 'must not reach the index\n' >"$client/rejected-index-entry"
rejected_oid=$(git -C "$client" hash-object rejected-index-entry)
set +e
GIT_TEST_CLOUD_ODB_FAILPOINT=commit-error \
	git -C "$client" add rejected-index-entry \
	>"$trash/add-failed-commit.out" 2>"$trash/add-failed-commit.err"
add_status=$?
set -e
test "$add_status" -ne 0
grep "failed to commit ODB transaction" \
	"$trash/add-failed-commit.err" >/dev/null
git -C "$client" ls-files --stage >"$trash/index-after-failed-commit"
cmp "$trash/index-before-failed-commit" "$trash/index-after-failed-commit"
rm "$client/.git/objects/cloud-odb"
set +e
git -C "$client" cat-file -e "$rejected_oid" \
	>"$trash/rejected-object.out" 2>"$trash/rejected-object.err"
rejected_status=$?
set -e
test "$rejected_status" -ne 0

sleep 1
test "$(printf '%s' "$freshen_contents" |
	git -C "$bare" hash-object -w --stdin)" = "$freshen_oid"
freshen_after=$(stat -c %Y "$freshen_path")
test "$freshen_after" -gt "$freshen_before"

git -C "$bare" commit-graph write
test -f "$bare/objects/info/commit-graph"
set +e
printf 'done\n' | git -C "$bare" fast-import \
	>"$trash/fast-import.out" 2>"$trash/fast-import.err"
fast_import_status=$?
set -e
test "$fast_import_status" -ne 0
grep "fast-import is not supported with a cloud ODB" \
	"$trash/fast-import.err" >/dev/null

git -C "$bare" cat-file --batch-all-objects \
	--batch-check='%(objectname)' >"$trash/fallback-enumerated"
grep "^$fallback_oid$" "$trash/fallback-enumerated" >/dev/null
git -C "$bare" gc --auto
set +e
git -C "$bare" gc >"$trash/gc.out" 2>"$trash/gc.err"
gc_status=$?
set -e
test "$gc_status" -ne 0
grep "files fallback optimization is unsupported by the cloud ODB" \
	"$trash/gc.err" >/dev/null
test -f "$fallback_loose_path"
git -C "$bare" cat-file -e "$fallback_loose_oid^{commit}"
git -C "$bare" multi-pack-index write
git -C "$bare" multi-pack-index verify
git -C "$bare" repack -ad

printf 'failpoint staging\n' >>"$client/object"
git -C "$client" commit --quiet -am failpoint-staging
orphan_metrics=$trash/orphan-metrics.jsonl
orphan_run=11111111111111111111111111111111
set +e
GIT_CLOUD_ODB_METRICS_PATH=$orphan_metrics \
GIT_CLOUD_ODB_METRICS_RUN=$orphan_run \
GIT_TEST_CLOUD_ODB_FAILPOINT=after-artifact-upload \
	git -C "$client" push --quiet "$bare" HEAD:main
failpoint_status=$?
set -e
test "$failpoint_status" -ne 0
set -- "$bare"/objects/cloud-write-*
test -d "$1"
rm "$1/data.gsg" "$1/index.gsi"
rmdir "$1"
set +e
GIT_CLOUD_ODB_METRICS_PATH=$orphan_metrics \
GIT_CLOUD_ODB_METRICS_RUN=$orphan_run \
GIT_TEST_CLOUD_ODB_PENDING_GRACE_SECONDS=0 \
GIT_TEST_CLOUD_ODB_FAILPOINT=after-gc-lease \
	git -C "$bare" cat-file -e "$fallback_oid^{commit}" \
	>"$trash/gc-owner.out" 2>"$trash/gc-owner.err"
gc_owner_status=$?
set -e
test "$gc_owner_status" -ne 0
GIT_CLOUD_ODB_METRICS_PATH=$orphan_metrics \
GIT_CLOUD_ODB_METRICS_RUN=$orphan_run \
GIT_TEST_CLOUD_ODB_PENDING_GRACE_SECONDS=0 \
GIT_TEST_CLOUD_ODB_GC_LEASE_SECONDS=0 \
	git -C "$bare" cat-file -e "$fallback_oid^{commit}"
test "$(grep -c '"method":"DELETE"' "$orphan_metrics")" -ge 2
test "$(grep -c '"method":"DELETE".*"deletes":1' "$orphan_metrics")" -ge 2
for staging in "$bare"/objects/cloud-write-*
do
	test ! -e "$staging"
done

printf 'published without ref\n' >>"$client/object"
git -C "$client" commit --quiet -am published-without-ref
unreferenced_oid=$(git -C "$client" rev-parse HEAD)
published_metrics=$trash/published-metrics.jsonl
published_run=22222222222222222222222222222222
set +e
GIT_CLOUD_ODB_METRICS_PATH=$published_metrics \
GIT_CLOUD_ODB_METRICS_RUN=$published_run \
GIT_TEST_CLOUD_ODB_FAILPOINT=after-cas \
	git -C "$client" push --quiet "$bare" HEAD:main
published_status=$?
set -e
test "$published_status" -ne 0
test "$(git -C "$bare" rev-parse main)" = "$fallback_oid"
GIT_CLOUD_ODB_METRICS_PATH=$published_metrics \
GIT_CLOUD_ODB_METRICS_RUN=$published_run \
GIT_TEST_CLOUD_ODB_PENDING_GRACE_SECONDS=0 \
	git -C "$bare" cloud-bench recover
git -C "$bare" cat-file -e "$fallback_oid^{commit}"
if grep -q '"method":"DELETE"' "$published_metrics"
then
	echo >&2 "unfenced recovery deleted a published artifact"
	exit 1
fi
git -C "$bare" cat-file -e "$unreferenced_oid^{commit}"

stale_oid=$(printf 'stale reader artifact %s\n' "$prefix" |
	git -C "$bare" hash-object -w --stdin)
request_fifo=$trash/stale-reader.in
response_fifo=$trash/stale-reader.out
mkfifo "$request_fifo" "$response_fifo"
git -C "$bare" cat-file --batch-check <"$request_fifo" >"$response_fifo" &
batch_pid=$!
exec 3>"$request_fifo"
exec 4<"$response_fifo"
printf '%s\n' "$stale_oid" >&3
IFS= read -r stale_result <&4
test "${stale_result#"$stale_oid blob "}" != "$stale_result"
GIT_TEST_CLOUD_ODB_PENDING_GRACE_SECONDS=0 \
	git -C "$bare" cloud-bench recover
replacement_oid=$(printf 'replacement reader artifact %s\n' "$prefix" |
	git -C "$bare" hash-object -w --stdin)
printf '%s\n' "$replacement_oid" >&3
IFS= read -r replacement_result <&4
test "${replacement_result#"$replacement_oid blob "}" != \
	"$replacement_result"
exec 3>&-
exec 4<&-
wait "$batch_pid"
GIT_TEST_CLOUD_ODB_PENDING_GRACE_SECONDS=0 \
	git -C "$bare" cloud-bench recover

alternate_metrics=$trash/alternate-metrics.jsonl
alternate_run=55555555555555555555555555555555
alternate_oid=$(printf 'alternate owner artifact %s\n' "$prefix" |
	GIT_CLOUD_ODB_METRICS_PATH=$alternate_metrics \
	GIT_CLOUD_ODB_METRICS_RUN=$alternate_run \
	git -C "$bare" hash-object -w --stdin)
alternate_consumer=$trash/alternate-consumer.git
git init --bare --quiet "$alternate_consumer"
printf '%s\n' "$bare/objects" \
	>"$alternate_consumer/objects/info/alternates"
shared_staging=$bare/objects/cloud-write-999999
mkdir "$shared_staging"
printf 'shared writer data\n' >"$shared_staging/data.gsg"
printf 'shared writer index\n' >"$shared_staging/index.gsi"
GIT_CLOUD_ODB_METRICS_PATH=$alternate_metrics \
GIT_CLOUD_ODB_METRICS_RUN=$alternate_run \
GIT_TEST_CLOUD_ODB_PENDING_GRACE_SECONDS=0 \
	git -C "$alternate_consumer" cat-file -e "$alternate_oid^{blob}"
test -f "$shared_staging/data.gsg"
test -f "$shared_staging/index.gsi"
rm "$shared_staging/data.gsg" "$shared_staging/index.gsi"
rmdir "$shared_staging"
if grep -q '"method":"DELETE"' "$alternate_metrics"
then
	echo >&2 "alternate consumer reclaimed its owner's publication"
	exit 1
fi
GIT_TEST_CLOUD_ODB_PENDING_GRACE_SECONDS=0 \
	git -C "$bare" cloud-bench recover

direct_metrics=$trash/direct-metrics.jsonl
direct_run=44444444444444444444444444444444
direct_blob=$(printf 'direct reachable blob\n' |
	GIT_CLOUD_ODB_METRICS_PATH=$direct_metrics \
	GIT_CLOUD_ODB_METRICS_RUN=$direct_run \
	git -C "$bare" hash-object -w --stdin)
direct_tree=$(printf '100644 blob %s\tdirect\n' "$direct_blob" |
	GIT_CLOUD_ODB_METRICS_PATH=$direct_metrics \
	GIT_CLOUD_ODB_METRICS_RUN=$direct_run \
	git -C "$bare" mktree)
direct_commit=$(printf 'direct publication graph\n' |
	GIT_AUTHOR_NAME='Cloud ODB smoke' \
	GIT_AUTHOR_EMAIL=cloud-odb-smoke@example.invalid \
	GIT_COMMITTER_NAME='Cloud ODB smoke' \
	GIT_COMMITTER_EMAIL=cloud-odb-smoke@example.invalid \
	GIT_CLOUD_ODB_METRICS_PATH=$direct_metrics \
	GIT_CLOUD_ODB_METRICS_RUN=$direct_run \
	git -C "$bare" commit-tree "$direct_tree")
git -C "$bare" update-ref refs/heads/direct "$direct_commit"
direct_recovery_one=$trash/direct-recovery-one.jsonl
direct_recovery_two=$trash/direct-recovery-two.jsonl
GIT_CLOUD_ODB_METRICS_PATH=$direct_recovery_one \
GIT_CLOUD_ODB_METRICS_RUN=$direct_run \
GIT_TEST_CLOUD_ODB_PENDING_GRACE_SECONDS=0 \
	git -C "$bare" cloud-bench recover
GIT_CLOUD_ODB_METRICS_PATH=$direct_recovery_two \
GIT_CLOUD_ODB_METRICS_RUN=$direct_run \
GIT_TEST_CLOUD_ODB_PENDING_GRACE_SECONDS=0 \
	git -C "$bare" cloud-bench recover
test "$(grep -c '"rangeRequests":1' "$direct_recovery_one")" = \
	"$(grep -c '"rangeRequests":1' "$direct_recovery_two")"
grep '"method":"PUT"' "$direct_recovery_one" >/dev/null
if grep -q '"method":"PUT"' "$direct_recovery_two"
then
	echo >&2 "published recovery left direct-write journal records"
	exit 1
fi
if grep -q '"method":"DELETE"' "$direct_recovery_one" ||
	grep -q '"method":"DELETE"' "$direct_recovery_two"
then
	echo >&2 "recovery deleted a transitively reachable publication"
	exit 1
fi
test "$(git -C "$bare" rev-parse refs/heads/direct^{tree})" = "$direct_tree"
test "$(git -C "$bare" show refs/heads/direct:direct)" = \
	'direct reachable blob'

printf 'cloud\n' >>"$client/object"
git -C "$client" commit --quiet -am cloud
confirmed_metrics=$trash/confirmed-metrics.jsonl
confirmed_run=33333333333333333333333333333333
GIT_CLOUD_ODB_METRICS_PATH=$confirmed_metrics \
GIT_CLOUD_ODB_METRICS_RUN=$confirmed_run \
	git -C "$client" push --quiet "$bare" HEAD:main
confirmed_recovery_metrics=$trash/confirmed-recovery-metrics.jsonl
GIT_CLOUD_ODB_METRICS_PATH=$confirmed_recovery_metrics \
GIT_CLOUD_ODB_METRICS_RUN=66666666666666666666666666666666 \
GIT_TEST_CLOUD_ODB_PENDING_GRACE_SECONDS=0 \
	git -C "$bare" cloud-bench recover
if grep -q '"objectReads":[1-9]' "$confirmed_recovery_metrics"
then
	echo >&2 "published recovery traversed cloud object payloads"
	exit 1
fi
if grep -q '"method":"DELETE"' "$confirmed_metrics"
then
	echo >&2 "recovery deleted a ref-confirmed publication"
	exit 1
fi
git -C "$bare" cat-file -e "$(git -C "$client" rev-parse HEAD)^{commit}"

double_orphan_metrics=$trash/double-orphan-metrics.jsonl
double_orphan_run=77777777777777777777777777777777
for orphan_label in one two
do
	printf 'double orphan %s\n' "$orphan_label" >>"$client/object"
	git -C "$client" commit --quiet -am "double-orphan-$orphan_label"
	set +e
	GIT_CLOUD_ODB_METRICS_PATH=$double_orphan_metrics \
	GIT_CLOUD_ODB_METRICS_RUN=$double_orphan_run \
	GIT_TEST_CLOUD_ODB_FAILPOINT=after-artifact-upload \
		git -C "$client" push --quiet "$bare" HEAD:main
	double_orphan_status=$?
	set -e
	test "$double_orphan_status" -ne 0
done
GIT_CLOUD_ODB_METRICS_PATH=$double_orphan_metrics \
GIT_CLOUD_ODB_METRICS_RUN=$double_orphan_run \
GIT_TEST_CLOUD_ODB_PENDING_GRACE_SECONDS=0 \
	git -C "$bare" cloud-bench recover
test "$(grep -c '"method":"DELETE"' "$double_orphan_metrics")" = 2
GIT_CLOUD_ODB_METRICS_PATH=$double_orphan_metrics \
GIT_CLOUD_ODB_METRICS_RUN=$double_orphan_run \
GIT_TEST_CLOUD_ODB_PENDING_GRACE_SECONDS=0 \
	git -C "$bare" cloud-bench recover
test "$(grep -c '"method":"DELETE"' "$double_orphan_metrics")" = 4

printf 'second cloud artifact\n' >>"$client/object"
git -C "$client" commit --quiet -am second-cloud-artifact
git -C "$client" push --quiet "$bare" HEAD:main

git -C "$bare" rev-list --objects --all |
	awk '{print $1}' | sort -u >"$trash/reachable"
git -C "$bare" cat-file --batch-all-objects \
	--batch-check='%(objectname)' | sort -u >"$trash/enumerated"
if test -s "$trash/reachable" &&
	comm -23 "$trash/reachable" "$trash/enumerated" | grep .
then
	echo >&2 "reachable objects are missing from cloud ODB enumeration"
	exit 1
fi
git clone --quiet --no-local "$bare" "$trash/clone"
git -C "$trash/clone" fsck --strict

echo "cloud ODB fallback enumeration smoke test passed"
