#!/bin/sh

test_description="index-only merges"

. ./test-lib.sh

# Testcase setup for rename/modify:
#   Commit A: new file: a
#   Commit B: modify a slightly
#   Commit C: rename a->b
# We should be able to merge B & C cleanly

test_expect_success 'setup rename/modify merge' '
	git init &&

	printf "1\n2\n3\n4\n5\n6\n7\n" >a &&
	git add a &&
	git commit -m A &&
	git tag A &&

	git checkout -b B A &&
	echo 8 >>a &&
	git add a &&
	git commit -m B &&

	git checkout -b C A &&
	git mv a b &&
	git commit -m C
'

test_expect_failure '--index-only with rename/modify works in non-bare-clone' '
	git checkout B^0 &&

	git merge --index-only -s recursive C^0 &&

	echo "Making sure the working copy was not updated" &&
	test ! -f b &&
	test -f a &&
	test $(git rev-parse B:a) = $(git hash-object a) &&

	echo "Making sure the index was updated" &&
	test 1 -eq $(git ls-files -s | wc -l) &&
	test $(git rev-parse B:a) = $(git rev-parse :b)
'

test_expect_failure '--index-only with rename/modify works in a bare clone' '
	git clone --bare . bare.clone &&
	(cd bare.clone &&

	 git update-ref --no-deref HEAD B &&
	 git read-tree HEAD &&

	 git merge --index-only -s recursive C^0 &&

	 echo "Making sure the working copy was not updated" &&
	 test ! -f b &&
	 test ! -f a &&

	 echo "Making sure the index was updated" &&
	 test 1 -eq $(git ls-files -s | wc -l) &&
	 test $(git rev-parse B:a) = $(git rev-parse :b)
	)
'

# Testcase requiring recursion to handle:
#
#    L1      L2
#     o---o--o
#    / \ /    \
# B o   X      o C
#    \ / \    /
#     o---o--o
#    R1      R2
#
# B : 1..20
# L1: 1..3 a..c 4..20
# R1: 1..13n..p14..20
# L2: 1..3 a..i 4..13 n..p 14..20
# R2: 1..3 a..c 4..13 n..v 14..20
# C:  1..3 a..i 4..13 n..v 14..20
#   needs 'recursive' strategy to get correct solution; 'resolve' would fail
#

test_expect_success 'setup single-file criss-cross resolvable with recursive strategy' '
	git reset --hard &&
	git rm -rf . &&
	git clean -fdqx &&
	rm -rf .git &&
	git init &&

	seq 1 20 >contents &&
	git add contents &&
	test_tick && git commit -m initial &&

	git branch R1 &&
	git checkout -b L1 &&
	seq 1 3 >contents &&
	seq 1 3 | tr 1-3 a-c >>contents &&
	seq 4 20 >>contents &&
	git add contents &&
	test_tick && git commit -m L1 &&

	git checkout R1 &&
	seq 1 13 >contents &&
	seq 1 3 | tr 1-3 n-p >>contents &&
	seq 14 20 >>contents &&
	git add contents &&
	test_tick && git commit -m R1 &&

	git checkout -b L2 L1^0 &&
	test_tick && git merge R1 &&
	seq 1 3 >contents &&
	seq 1 9 | tr 1-9 a-i >>contents &&
	seq 4 13 >>contents &&
	seq 1 3 | tr 1-3 n-p >>contents &&
	seq 14 20 >>contents &&
	git add contents &&
	test_tick && git commit -m L2 &&

	git checkout -b R2 R1^0 &&
	test_tick && git merge L1 &&
	seq 1 3 >contents &&
	seq 1 3 | tr 1-3 a-c >>contents &&
	seq 4 13 >>contents &&
	seq 1 9 | tr 1-9 n-v >>contents &&
	seq 14 20 >>contents &&
	git add contents &&
	test_tick && git commit -m R2 &&

	seq 1 3 >answer &&
	seq 1 9 | tr 1-9 a-i >>answer &&
	seq 4 13 >>answer &&
	seq 1 9 | tr 1-9 n-v >>answer &&
	seq 14 20 >>answer &&
	git tag answer $(git hash-object -w answer) &&
	rm -f answer
'

test_expect_failure 'recursive --index-only in non-bare repo' '
	git reset --hard &&
	git checkout L2^0 &&

	git merge --index-only -s recursive R2^0 &&

	test 1 = $(git ls-files -s | wc -l) &&
	test 0 = $(git ls-files -u | wc -l) &&
	test 0 = $(git ls-files -o | wc -l) &&

	test $(git rev-parse :contents) = $(git rev-parse answer) &&
	test $(git rev-parse L2:contents) = $(git hash-object contents)
'

test_expect_failure 'recursive --index-only in bare repo' '
	git clone --bare . bare.clone &&
	(cd bare.clone &&

	 git update-ref --no-deref HEAD L2 &&
	 git read-tree HEAD &&

	 git merge --index-only -s recursive R2^0 &&

	 test 1 = $(git ls-files -s | wc -l) &&
	 test 0 = $(git ls-files -u | wc -l) &&

	 test $(git rev-parse :contents) = $(git rev-parse answer) &&
	 test ! -f contents
	)
'

test_done
