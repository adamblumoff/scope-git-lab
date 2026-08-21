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
git -C "$bare" gc
test ! -e "$fallback_loose_path"
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
GIT_CLOUD_ODB_METRICS_PATH=$orphan_metrics \
GIT_CLOUD_ODB_METRICS_RUN=$orphan_run \
GIT_TEST_CLOUD_ODB_PENDING_GRACE_SECONDS=0 \
	git -C "$bare" cat-file -e "$fallback_oid^{commit}"
test "$(grep -c '"method":"DELETE"' "$orphan_metrics")" -ge 2
for staging in "$bare"/objects/cloud-write-*
do
	test ! -e "$staging"
done

printf 'cloud\n' >>"$client/object"
git -C "$client" commit --quiet -am cloud
git -C "$client" push --quiet "$bare" HEAD:main
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
