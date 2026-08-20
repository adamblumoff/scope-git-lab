#!/bin/sh

test_description='performance of the experimental segment object store'
. ./perf-lib.sh

test_perf_fresh_repo

test_expect_success 'prepare conventional files store' '
	test-tool genrandom segment-base 1048576 >payload &&
	git add payload &&
	git commit -q -m base &&
	for i in $(test_seq 1 50)
	do
		echo "$i" >>payload &&
		git commit -q -am "update $i" || return 1
	done &&
	git repack -ad &&
	git cat-file --batch-all-objects --batch-check="%(objectname)" >oids &&
	printf "HEAD\n" >revs &&
	base=$(git rev-parse HEAD) &&
	git clone --quiet --bare . files-server-template.git &&
	git clone --quiet --bare . segment-server-template.git &&
	git -C segment-server-template.git segment-store import &&
	mv segment-server-template.git/objects/pack \
		segment-server-template.git/objects/pack.disabled &&
	git clone --quiet . push-client &&
	for i in $(test_seq 1 20)
	do
		echo "push $i" >>push-client/payload &&
		git -C push-client commit -q -am "push $i" || return 1
	done &&
	git -C push-client rev-list --reverse "$base..HEAD" >push-revs &&
	repo_url="file://$(pwd)" &&
	export repo_url base
'

test_size 'conventional pack bytes' '
	du -kc .git/objects/pack/* 2>/dev/null | tail -1 | awk "{print \$1 * 1024}"
'

test_perf 'conventional full-object reads' '
	git cat-file --batch <oids >/dev/null
'

test_perf 'conventional outbound pack' '
	git pack-objects --stdout --revs <revs >conventional.pack
'

test_size 'conventional outbound bytes' '
	test_file_size conventional.pack
'

test_perf 'conventional protocol clone' --setup 'rm -rf clone-files' '
	git clone --quiet --no-local "$repo_url" clone-files
'

test_perf 'conventional 20 sequential pushes' \
	--setup 'rm -rf files-server.git && cp -R files-server-template.git files-server.git' '
	server=$(pwd)/files-server.git &&
	while read oid
	do
		git -C push-client push --quiet "$server" \
			"$oid:refs/heads/main" || exit 1
	done <push-revs
'

test_perf 'import canonical segment' --setup 'rm -rf .git/objects/segments' '
	git segment-store import
'

test_expect_success 'remove conventional data from the measured read path' '
	mv .git/objects/pack pack.saved
'

test_size 'segment data bytes' '
	test_file_size .git/objects/segments/00000001.seg
'

test_size 'segment index bytes' '
	test_file_size .git/objects/segments/00000001.idx
'

test_perf 'segment full-object reads' '
	git cat-file --batch <oids >/dev/null
'

test_perf 'segment outbound pack' '
	git pack-objects --stdout --revs <revs >segment.pack
'

test_size 'segment outbound bytes' '
	test_file_size segment.pack
'

test_perf 'segment protocol clone' --setup 'rm -rf clone-segment' '
	git clone --quiet --no-local "$repo_url" clone-segment
'

test_perf 'segment 20 sequential pushes' \
	--setup 'rm -rf segment-server.git && cp -R segment-server-template.git segment-server.git' '
	server=$(pwd)/segment-server.git &&
	while read oid
	do
		git -C push-client push --quiet "$server" \
			"$oid:refs/heads/main" || exit 1
	done <push-revs
'

test_done
