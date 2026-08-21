#!/bin/sh

test_description='segment object database'

. ./test-lib.sh

test_expect_success 'create history and record files ODB results' '
	git init repo &&
	(
		cd repo &&
		test_commit root &&
		git branch -M main &&
		git checkout -b side &&
		test_commit side &&
		git checkout main &&
		test_commit main &&
		git merge --no-ff side -m merge &&
		git tag -a -m annotated v1 &&
		test-tool genrandom large 2097152 >large.bin &&
		git add large.bin &&
		test_tick &&
		git commit -m large &&
		git hash-object -w --stdin </dev/null >../empty-blob.oid &&
		git mktree </dev/null >../empty-tree.oid &&
		git repack -ad &&
		git prune-packed &&
		printf "unreachable\n" | git hash-object -w --stdin >../unreachable.oid &&

		git cat-file --batch-all-objects --batch-check="%(objectname)" |
			sort -u >../objects.before &&
		git cat-file --batch <../objects.before >../cat-file.before &&
		git rev-list --objects --all >../rev-list.before &&
		git log --all --date-order \
			--format="%H %P %T %D %s" >../log.before
	) &&
	test_path_is_dir repo/.git/objects/pack
'

test_expect_success 'import files ODB into a segment store' '
	git -C repo segment-store import &&
	test_path_is_file repo/.git/objects/segments/manifest &&
	test_path_is_file repo/.git/objects/segments/00000001.seg &&
	test_path_is_file repo/.git/objects/segments/00000001.idx &&
	test_path_is_dir repo/.git/objects/pack
'

test_expect_success RUST 'compatibility map uses the segment files delegate' '
	git clone --quiet --bare --no-local \
		"file://$(pwd)/repo" compat-segment.git &&
	git -C compat-segment.git segment-store import &&
	git -C compat-segment.git config core.repositoryformatversion 1 &&
	git -C compat-segment.git config extensions.objectformat sha1 &&
	git -C compat-segment.git config extensions.compatobjectformat sha256 &&
	git -C compat-segment.git rev-parse --git-dir >compat.gitdir &&
	test_file_not_empty compat.gitdir
'

test_expect_success 'stats describe the imported segment store' '
	object_count=$(sed -n "$=" objects.before) &&
	git -C repo segment-store stats >stats &&
	sed -n "1,2p" stats >stats.header &&
	cat >expect <<-EOF &&
	segments 1
	objects $object_count
	EOF
	test_cmp expect stats.header &&
	test_grep "^data_bytes [1-9][0-9]*$" stats &&
	test_grep "^index_bytes [1-9][0-9]*$" stats &&
	test_line_count = 4 stats
'

test_expect_success 'large manifests do not retain a descriptor per segment' '
	cp repo/.git/objects/segments/manifest manifest.one &&
	test_when_finished "mv manifest.one \
		repo/.git/objects/segments/manifest" &&
	segment_line=$(sed -n 3p manifest.one) &&
	{
		sed -n "1,2p" manifest.one &&
		for i in $(test_seq 128)
		do
			echo "$segment_line" || return 1
		done
	} >repo/.git/objects/segments/manifest &&
	run_with_limited_open_files git -C repo cat-file -e HEAD^{commit}
'

test_expect_success 'activation retains objects from in-flight files writers' '
	mv repo/.git/objects/segments/manifest \
		repo/.git/objects/segments/manifest.disabled &&
	test_when_finished "test ! -f \
		repo/.git/objects/segments/manifest.disabled || mv \
		repo/.git/objects/segments/manifest.disabled \
		repo/.git/objects/segments/manifest" &&
	tree=$(git -C repo rev-parse HEAD^{tree}) &&
	late=$(echo late | git -C repo commit-tree "$tree" -p HEAD) &&
	git -C repo update-ref refs/heads/late-files "$late" &&
	mv repo/.git/objects/segments/manifest.disabled \
		repo/.git/objects/segments/manifest &&
	git -C repo cat-file -e refs/heads/late-files^{commit} &&
	git -C repo update-ref -d refs/heads/late-files
'

test_expect_success 'manifest activates segment reads without files ODB data' '
	unreachable=$(cat unreachable.oid) &&
	loose_dir=$(echo "$unreachable" | cut -c1-2) &&
	mv repo/.git/objects/pack repo/.git/objects/pack.disabled &&
	mv "repo/.git/objects/$loose_dir" \
		"repo/.git/objects/$loose_dir.disabled" &&
	git -C repo cat-file --batch-all-objects --batch-check="%(objectname)" |
		sort -u >objects.after &&
	git -C repo cat-file --batch <objects.before >cat-file.after &&
	git -C repo rev-list --objects --all >rev-list.after &&
	git -C repo log --all --date-order \
		--format="%H %P %T %D %s" >log.after &&
	test_cmp objects.before objects.after &&
	test_cmp cat-file.before cat-file.after &&
	test_cmp rev-list.before rev-list.after &&
	test_cmp log.before log.after
'

test_expect_success 'full and connectivity-only fsck read the segment store' '
	git -C repo fsck --strict --no-dangling &&
	git -C repo fsck --connectivity-only --no-dangling
'

test_expect_success 'upload-pack clones from segment-only storage' '
	git clone --quiet --no-local "file://$(pwd)/repo" clone-from-segment &&
	git -C clone-from-segment rev-parse HEAD >clone.head &&
	git -C repo rev-parse HEAD >repo.head &&
	test_cmp repo.head clone.head
'

test_expect_success 'receive-pack publishes accepted pushes as new segments' '
	git clone --quiet --bare --no-local "file://$(pwd)/repo" server.git &&
	git -C server.git segment-store import &&
	git clone --quiet --no-local "file://$(pwd)/server.git" client &&
	write_script server.git/hooks/pre-receive <<-\EOF &&
	read old new ref
	git cat-file -e "$new^{commit}"
	EOF
	test_commit -C client pushed &&
	git -C client push origin HEAD:refs/heads/main &&
	test_path_is_file server.git/objects/segments/00000002.seg &&
	test_path_is_file server.git/objects/segments/00000002.idx &&
	git -C server.git segment-store stats >push.stats &&
	test_grep "^segments 2$" push.stats &&
	git -C server.git cat-file -e refs/heads/main^{commit} &&
	mv server.git/objects/segments/manifest \
		server.git/objects/segments/manifest.disabled &&
	git -C server.git fsck --connectivity-only --no-dangling &&
	mv server.git/objects/segments/manifest.disabled \
		server.git/objects/segments/manifest
'

test_expect_success 'receive-pack reports manifest lock contention' '
	test_commit -C client lock-contention &&
	git -C server.git rev-parse refs/heads/main >lock.old &&
	touch server.git/objects/segments/manifest.lock &&
	test_when_finished "rm -f server.git/objects/segments/manifest.lock" &&
	test_must_fail git -C client push origin HEAD:refs/heads/main 2>err &&
	test_grep "unable to migrate objects to permanent storage" err &&
	test_grep ! "remote end hung up unexpectedly" err &&
	git -C server.git rev-parse refs/heads/main >lock.actual &&
	test_cmp lock.old lock.actual &&
	git -C server.git fsck --connectivity-only --no-dangling
'

test_expect_success 'rejected pushes do not publish a segment' '
	cp server.git/objects/segments/manifest manifest.before-reject &&
	write_script server.git/hooks/pre-receive <<-\EOF &&
	exit 1
	EOF
	test_commit -C client rejected &&
	test_must_fail git -C client push origin HEAD:refs/heads/main &&
	test_cmp manifest.before-reject server.git/objects/segments/manifest &&
	test_path_is_missing server.git/objects/segments/00000003.seg &&
	git -C server.git fsck --connectivity-only --no-dangling
'

test_expect_success 'receive-pack crash points keep refs and object stores readable' '
	for failpoint in after-segment-fsync after-index-fsync \
		before-manifest-rename after-manifest-rename
	do
		git clone --quiet --bare --no-local \
			"file://$(pwd)/repo" "push-crash-$failpoint.git" &&
		git -C "push-crash-$failpoint.git" segment-store import &&
		git clone --quiet --no-local \
			"file://$(pwd)/push-crash-$failpoint.git" \
			"push-crash-$failpoint-client" &&
		test_commit -C "push-crash-$failpoint-client" "$failpoint" &&
		git -C "push-crash-$failpoint.git" rev-parse refs/heads/main \
			>"$failpoint.old" &&
		test_must_fail env GIT_TEST_SEGMENT_FAILPOINT=$failpoint \
			git -C "push-crash-$failpoint-client" push origin \
			HEAD:refs/heads/main &&
		git -C "push-crash-$failpoint.git" rev-parse refs/heads/main \
			>"$failpoint.actual" &&
		test_cmp "$failpoint.old" "$failpoint.actual" &&
		git -C "push-crash-$failpoint.git" segment-store stats >/dev/null &&
		git -C "push-crash-$failpoint.git" fsck \
			--connectivity-only --no-dangling || return 1
	done
'

test_expect_success 'manual compaction replaces the shelf but retains old files' '
	git -C server.git segment-store compact &&
	git -C server.git segment-store stats >compact.stats &&
	test_grep "^segments 1$" compact.stats &&
	test_path_is_file server.git/objects/segments/00000001.seg &&
	test_path_is_file server.git/objects/segments/00000002.seg &&
	test_path_is_file server.git/objects/segments/00000003.seg &&
	git -C server.git cat-file -e refs/heads/main^{commit} &&
	git -C server.git fsck --connectivity-only --no-dangling
'

test_expect_success 'import crash points leave a readable repository' '
	for failpoint in after-segment-fsync after-index-fsync \
		before-manifest-rename after-manifest-rename
	do
		git clone --quiet --bare --no-local \
			"file://$(pwd)/repo" "crash-$failpoint.git" &&
		test_must_fail env GIT_TEST_SEGMENT_FAILPOINT=$failpoint \
			git -C "crash-$failpoint.git" segment-store import &&
		git -C "crash-$failpoint.git" cat-file -e HEAD^{commit} &&
		git -C "crash-$failpoint.git" fsck \
			--connectivity-only --no-dangling || return 1
	done
'

test_expect_success 'missing objects are reported by the segment store' '
	missing=$(test_oid deadbeef) &&
	echo "$missing" | git -C repo cat-file --batch-check >actual &&
	echo "$missing missing" >expect &&
	test_cmp expect actual &&
	test_must_fail git -C repo cat-file -e "$missing"
'

test_expect_success 'segment stores retain conventional alternates' '
	git clone --quiet --shared repo alternate-client &&
	git -C alternate-client segment-store import &&
	git -C alternate-client cat-file -e HEAD^{commit} &&
	git -C alternate-client fsck --connectivity-only --no-dangling
'

test_expect_success 'metadata paths do not inflate segment payloads' '
	cp repo/.git/objects/segments/00000001.seg segment-data.good &&
	test_when_finished "mv segment-data.good \
		repo/.git/objects/segments/00000001.seg" &&
	chmod +w repo/.git/objects/segments/00000001.seg &&
	printf y | dd of=repo/.git/objects/segments/00000001.seg \
		bs=1 seek=16 conv=notrunc 2>/dev/null &&
	git -C repo cat-file --batch-all-objects \
		--batch-check="%(objectname)" | sort -u >metadata.objects &&
	test_cmp objects.before metadata.objects &&
	git -C repo cat-file --batch-check="%(objectname) %(objecttype) \
		%(objectsize) %(objectsize:disk)" <objects.before >/dev/null &&
	git -C repo cat-file -e HEAD^{commit} &&
	test_must_fail git -C repo fsck --no-dangling 2>err &&
	test_file_not_empty err
'

test_expect_success 'removing the manifest disables segment reads' '
	mv repo/.git/objects/segments/manifest \
		repo/.git/objects/segments/manifest.disabled &&
	test_must_fail git -C repo cat-file -e HEAD &&
	mv repo/.git/objects/segments/manifest.disabled \
		repo/.git/objects/segments/manifest
'

test_expect_success 'renaming the manifest rolls back to the files ODB' '
	mv repo/.git/objects/pack.disabled repo/.git/objects/pack &&
	mv "repo/.git/objects/$loose_dir.disabled" \
		"repo/.git/objects/$loose_dir" &&
	mv repo/.git/objects/segments/manifest \
		repo/.git/objects/segments/manifest.disabled &&
	git -C repo cat-file --batch <objects.before >cat-file.rollback &&
	test_cmp cat-file.before cat-file.rollback &&
	mv repo/.git/objects/segments/manifest.disabled \
		repo/.git/objects/segments/manifest
'

test_expect_success 'malformed manifest is rejected' '
	cp repo/.git/objects/segments/manifest manifest.good &&
	printf "not a segment manifest\n" >repo/.git/objects/segments/manifest &&
	test_must_fail git -C repo segment-store stats 2>err &&
	test_file_not_empty err &&
	mv manifest.good repo/.git/objects/segments/manifest
'

test_expect_success 'truncated index is rejected' '
	cp repo/.git/objects/segments/00000001.idx index.good &&
	test_copy_bytes 12 <index.good >index.bad &&
	mv index.bad repo/.git/objects/segments/00000001.idx &&
	test_must_fail git -C repo segment-store stats 2>err &&
	test_file_not_empty err &&
	mv index.good repo/.git/objects/segments/00000001.idx
'

test_expect_success 'implausible canonical size is rejected before allocation' '
	cp repo/.git/objects/segments/00000001.idx index.good &&
	test_when_finished "mv index.good \
		repo/.git/objects/segments/00000001.idx" &&
	chmod +w repo/.git/objects/segments/00000001.idx &&
	printf "\177\377\377\377\377\377\377\377" |
		dd of=repo/.git/objects/segments/00000001.idx \
		bs=1 seek=$((32 + $(test_oid rawsz) + 8)) \
		conv=notrunc 2>/dev/null &&
	corrupt_oid=$(sed -n 1p objects.before) &&
	test_must_fail git -C repo cat-file -p "$corrupt_oid" 2>err &&
	test_grep "unable to inflate object from segment" err
'

test_expect_success POSIXPERM 'segment files honor shared repository permissions' '
	git clone --quiet --bare --no-local "file://$(pwd)/repo" shared.git &&
	git -C shared.git config core.sharedrepository 0666 &&
	(
		umask 0077 &&
		git -C shared.git segment-store import
	) &&
	echo -r--r--r-- >expect &&
	test_modebits shared.git/objects/segments/00000001.seg >actual &&
	test_cmp expect actual &&
	test_modebits shared.git/objects/segments/00000001.idx >actual &&
	test_cmp expect actual
'

test_done
