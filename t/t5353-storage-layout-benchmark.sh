#!/bin/sh

test_description='local storage layout benchmark artifacts'

. ./test-lib.sh

test_expect_success 'same corpus produces deterministic layout artifacts' '
	mkdir first second &&
	test-tool storage-layout first 1024 >first.json &&
	test-tool storage-layout second 1024 >second.json &&
	test_cmp first.json second.json &&
	for artifact in pack.pack pack.idx segment.seg segment.idx group.gsg group.gsi
	do
		test_path_is_file "first/$artifact" &&
		test_cmp "first/$artifact" "second/$artifact" || return 1
	done
'

test_expect_success 'pack pair is stock Git-readable' '
	git verify-pack -v first/pack.idx >verify-pack.raw &&
	sed -n "/^[0-9a-f]\\{40\\} /p" verify-pack.raw >verify-pack.out &&
	test_line_count = 8 verify-pack.out
'

test_expect_success 'JSON labels local scope and exposes upload and range inputs' '
	test_grep "\"schema\": \"git-storage-layout-benchmark/v1\"" first.json &&
	test_grep "\"measurementScope\": \"local-artifact-layout\"" first.json &&
	test_grep "\"cloudMeasured\": false" first.json &&
	test_grep "\"id\": \"A\"" first.json &&
	test_grep "\"id\": \"B\"" first.json &&
	test_grep "\"id\": \"C\"" first.json &&
	test_grep "\"metadataRange\"" first.json &&
	test_grep "\"objectRanges\"" first.json &&
	test_grep "\"groupTargetBytes\": 1024" first.json
'

test_done
