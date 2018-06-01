#!/bin/sh

test_description='merge-recursive expensiveness'
. ./test-lib.sh

# This test is only useful in combination with some code changes that
# print out how many times MOVE_ARRAYS is called.

test_expect_success 'count move_arrays' '
	test_create_repo testing &&
	(
		cd testing &&

		test_seq 2 8 >counting &&
		>other_file &&
		git add counting other_file &&
		git commit -m initial &&

		git branch A &&
		git branch B &&

		git checkout A &&
		test_seq 1 8 >counting &&
		git add counting &&
		git commit -m A &&

		git checkout B &&
		test_seq 2 9 >counting &&
		git add counting &&
		git commit -m B &&

		git merge A >out &&
		grep MOVE_ARRAY out >count &&
		test_line_count = 1 count
	)
'

test_done
