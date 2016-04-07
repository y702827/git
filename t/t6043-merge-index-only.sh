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

test_expect_success '--index-only with rename/modify works in non-bare-clone' '
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

test_expect_success '--index-only with rename/modify works in a bare clone' '
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

test_expect_success 'recursive --index-only in non-bare repo' '
	git reset --hard &&
	git checkout L2^0 &&

	git merge --index-only -s recursive R2^0 &&

	test 1 = $(git ls-files -s | wc -l) &&
	test 0 = $(git ls-files -u | wc -l) &&
	test 0 = $(git ls-files -o | wc -l) &&

	test $(git rev-parse :contents) = $(git rev-parse answer) &&
	test $(git rev-parse L2:contents) = $(git hash-object contents)
'

test_expect_success 'recursive --index-only in bare repo' '
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

# Testcase for some simple merges
#   A
#   o-----o B
#    \
#     \---o C
#      \
#       \-o D
#        \
#         o E
#   Commit A: some file a
#   Commit B: adds file b, modifies end of a
#   Commit C: adds file c
#   Commit D: adds file d, modifies beginning of a
#   Commit E: renames a->subdir/a, adds subdir/e

test_expect_success 'setup simple merges' '
	git reset --hard &&
	git rm -rf . &&
	git clean -fdqx &&
	rm -rf .git &&
	git init &&

	seq 1 10 >a &&
	git add a &&
	test_tick && git commit -m A &&

	git branch A &&
	git branch B &&
	git branch C &&
	git branch D &&
	git branch E &&

	git checkout B &&
	echo b >b &&
	echo 11 >>a &&
	git add a b &&
	test_tick && git commit -m B &&

	git checkout C &&
	echo c >c &&
	git add c &&
	test_tick && git commit -m C &&

	git checkout D &&
	seq 2 10 >a &&
	echo d >d &&
	git add a d &&
	test_tick && git commit -m D &&

	git checkout E &&
	mkdir subdir &&
	git mv a subdir/a &&
	echo e >subdir/e &&
	git add subdir &&
	test_tick && git commit -m E
'

test_expect_success '--index-only ff update, non-bare' '
	git reset --hard &&
	git checkout A^0 &&

	git merge --index-only --ff-only E^0 &&

	git diff --staged --exit-code E &&
	test $(git hash-object a) = $(git rev-parse A:a) &&
	test ! -d subdir
'

test_expect_success '--index-only ff update, bare' '
	git clone --bare . bare.clone &&
	(cd bare.clone &&

	 git update-ref --no-deref HEAD A &&
	 git read-tree HEAD &&

	 git merge --index-only --ff-only E^0 &&

	 git diff --staged --exit-code E &&
	 test ! -f a &&
	 test ! -d subdir
	)
'

test_expect_success '--index-only ff update, non-bare with uncommitted changes' '
	git clean -fdx &&
	git reset --hard &&
	git checkout A^0 &&

	touch random_file && git add random_file &&

	git merge --index-only --ff-only E^0 &&

	test_must_fail git rev-parse HEAD:random_file &&
	test "$(git diff --name-only --cached E)" = "random_file" &&
	test $(git hash-object a) = $(git rev-parse A:a) &&
	test ! -d subdir
'

test_expect_success '--index-only w/ resolve, trivial, non-bare' '
	git clean -fdx &&
	git reset --hard &&
	git checkout B^0 &&

	git merge --index-only -s resolve C^0 | grep Wonderful &&

	test "$(git rev-list --count HEAD)" -eq 4 &&
	test $(git rev-parse :a) = $(git rev-parse B:a) &&
	test $(git rev-parse :b) = $(git rev-parse B:b) &&
	test $(git rev-parse :c) = $(git rev-parse C:c) &&
	test ! -f c
'

test_expect_failure '--index-only w/ resolve, trivial, bare' '
	rm -rf bare.clone &&
	git clone --bare . bare.clone &&
	(cd bare.clone &&

	 git update-ref --no-deref HEAD B &&
	 git read-tree HEAD &&

	 git merge --index-only -s resolve C^0 | grep Wonderful &&

	 test "$(git rev-list --count HEAD)" -eq 4 &&
	 test $(git rev-parse :a) = $(git rev-parse B:a) &&
	 test $(git rev-parse :b) = $(git rev-parse B:b) &&
	 test $(git rev-parse :c) = $(git rev-parse C:c) &&
	 test ! -f a &&
	 test ! -f b &&
	 test ! -f c
	)
'

test_expect_success '--index-only w/ resolve, non-trivial, non-bare' '
	git reset --hard &&
	git checkout B^0 &&

	git merge --index-only -s resolve D^0 &&

	test "$(git rev-list --count HEAD)" -eq 4 &&
	test $(git rev-parse :a) != $(git rev-parse B:a) &&
	test $(git rev-parse :a) != $(git rev-parse D:a) &&
	test $(git rev-parse :b) = $(git rev-parse B:b) &&
	test $(git rev-parse :d) = $(git rev-parse D:d) &&
	test $(git hash-object a) = $(git rev-parse B:a) &&
	test ! -f d
'

test_expect_success '--index-only w/ resolve, non-trivial, bare' '
	rm -rf bare.clone &&
	git clone --bare . bare.clone &&
	(cd bare.clone &&

	 git update-ref --no-deref HEAD B &&
	 git read-tree HEAD &&

	 git merge --index-only -s resolve D^0 &&

	 test "$(git rev-list --count HEAD)" -eq 4 &&
	 test $(git rev-parse :a) != $(git rev-parse B:a) &&
	 test $(git rev-parse :a) != $(git rev-parse D:a) &&
	 test $(git rev-parse :b) = $(git rev-parse B:b) &&
	 test $(git rev-parse :d) = $(git rev-parse D:d) &&
	 test ! -f a
	)
'

test_expect_failure '--index-only octopus, non-bare' '
	git reset --hard &&
	git checkout B^0 &&

	git merge --index-only -s octopus C^0 D^0 &&

	test "$(git rev-list --count HEAD)" -eq 5 &&
	test $(git rev-parse :a) != $(git rev-parse B:a) &&
	test $(git rev-parse :a) != $(git rev-parse C:a) &&
	test $(git rev-parse :a) != $(git rev-parse D:a) &&
	test $(git rev-parse :b) = $(git rev-parse B:b) &&
	test $(git rev-parse :c) = $(git rev-parse C:c) &&
	test $(git rev-parse :d) = $(git rev-parse D:d) &&
	test $(git hash-object a) = $(git rev-parse B:a) &&
	test ! -f c &&
	test ! -f d
'

test_expect_failure '--index-only octopus, bare' '
	rm -rf bare.clone &&
	git clone --bare . bare.clone &&
	(cd bare.clone &&

	 git update-ref --no-deref HEAD B &&
	 git read-tree HEAD &&

	 git merge --index-only -s octopus C^0 D^0 &&

	 test "$(git rev-list --count HEAD)" -eq 5 &&
	 test $(git rev-parse :a) != $(git rev-parse B:a) &&
	 test $(git rev-parse :a) != $(git rev-parse C:a) &&
	 test $(git rev-parse :a) != $(git rev-parse D:a) &&
	 test $(git rev-parse :b) = $(git rev-parse B:b) &&
	 test $(git rev-parse :c) = $(git rev-parse C:c) &&
	 test $(git rev-parse :d) = $(git rev-parse D:d) &&
	 test ! -f a &&
	 test ! -f c &&
	 test ! -f d
	)
'

test_expect_success '--index-only ours, non-bare' '
	git reset --hard &&
	git checkout B^0 &&

	git merge --index-only -s ours C^0 &&

	test "$(git rev-list --count HEAD)" -eq 4 &&
	test $(git rev-parse :a) = $(git rev-parse B:a) &&
	test $(git rev-parse :b) = $(git rev-parse B:b) &&
	test_must_fail git rev-parse :c &&
	test ! -f c
'

test_expect_success '--index-only ours, bare' '
	rm -rf bare.clone &&
	git clone --bare . bare.clone &&
	(cd bare.clone &&

	 git update-ref --no-deref HEAD B &&
	 git read-tree HEAD &&

	 git merge --index-only -s ours C^0 &&

	 test "$(git rev-list --count HEAD)" -eq 4 &&
	 test $(git rev-parse :a) = $(git rev-parse B:a) &&
	 test $(git rev-parse :b) = $(git rev-parse B:b) &&
	 test_must_fail git rev-parse :c &&
	 test ! -f a &&
	 test ! -f c
	)
'

test_expect_success '--index-only subtree, non-bare' '
	git reset --hard &&
	git checkout B^0 &&

	git merge --index-only -s subtree E^0 &&

	test "$(git rev-list --count HEAD)" -eq 4 &&
	test $(git rev-parse :a) = $(git rev-parse B:a) &&
	test $(git rev-parse :b) = $(git rev-parse B:b) &&
	test $(git rev-parse :e) = $(git rev-parse E:subdir/e) &&
	test ! -d subdir &&
	test ! -f e
'

test_expect_success '--index-only subtree, bare' '
	rm -rf bare.clone &&
	git clone --bare . bare.clone &&
	(cd bare.clone &&

	 git update-ref --no-deref HEAD B &&
	 git read-tree HEAD &&

	 git merge --index-only -s subtree E^0 &&

	 test "$(git rev-list --count HEAD)" -eq 4 &&
	 test $(git rev-parse :a) = $(git rev-parse B:a) &&
	 test $(git rev-parse :b) = $(git rev-parse B:b) &&
	 test $(git rev-parse :e) = $(git rev-parse E:subdir/e) &&
	 test ! -d subdir &&
	 test ! -f a &&
	 test ! -f e
	)
'

test_done
