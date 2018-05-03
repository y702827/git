#!/bin/sh
#
# Copyright (c) 2011 Elijah Newren
#

test_description='git reset --hard does not needlessly rewrite available files'

. ./test-lib.sh

test_expect_success 'setup' '
	echo lots and lots of content >file &&
	git add -A &&
	git commit -m initial
'

test_expect_failure 'reset --hard does not needlessly rewrite available file' '
	test-chmtime =1000000000 file &&
	test-chmtime -v +0 file >expect &&

	git mv file renamed &&
	git reset --hard &&

	test-chmtime -v +0 file >actual &&
	test_cmp expect actual
'

