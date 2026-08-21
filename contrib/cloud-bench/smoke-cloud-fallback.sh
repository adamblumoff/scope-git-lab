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

fallback_oid=$(git -C "$client" rev-parse HEAD)
printf 'git-cloud-odb 1\nlayout group\nprefix %s/group\n' "$prefix" \
	>"$bare/objects/cloud-odb"

git -C "$bare" cat-file --batch-all-objects \
	--batch-check='%(objectname)' >"$trash/fallback-enumerated"
grep "^$fallback_oid$" "$trash/fallback-enumerated" >/dev/null
git -C "$bare" gc --auto
git -C "$bare" gc

printf 'cloud\n' >>"$client/object"
git -C "$client" commit --quiet -am cloud
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
