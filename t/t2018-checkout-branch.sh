#!/bin/sh

test_description='checkout '

. ./test-lib.sh

# Arguments: <branch> <sha> [<checkout options>]
do_checkout() {
	exp_branch=$1 &&
	exp_ref="refs/heads/$exp_branch" &&

	# if <sha> is not specified, use HEAD.
	exp_sha=${2:-$(git rev-parse --verify HEAD)} &&

	git checkout ${3+"$3"} -b $exp_branch $exp_sha &&

	test $exp_ref = $(git rev-parse --symbolic-full-name HEAD) &&
	test $exp_sha = $(git rev-parse --verify HEAD)
}

test_dirty() {
	! git diff --exit-code >/dev/null
}

setup_dirty() {
	echo >>file1 change2
}

test_expect_success 'setup' '
	test_commit initial file1 &&
	HEAD1=$(git rev-parse --verify HEAD) &&

	test_commit change1 file1 &&
	HEAD2=$(git rev-parse --verify HEAD) &&

	git branch -m branch1
'

test_expect_success 'checkout -b to a new branch' '
	do_checkout branch2
'

test_expect_success 'checkout -b to a new branch (explicit ref)' '
	git checkout branch1 &&
	git branch -D branch2 &&

	do_checkout branch2 $HEAD1
'

test_expect_success 'checkout -b to a new branch (dirty)' '
	git checkout branch1 &&
	git branch -D branch2 &&

	setup_dirty &&
	test_must_fail do_checkout branch2 $HEAD1 &&
	test_dirty
'

test_expect_success 'checkout -b to an existing branch fails' '
	git reset --hard HEAD &&
	git branch branch2 &&

	test_must_fail do_checkout branch2 $HEAD2
'

test_expect_success 'checkout -f -b to an existing branch resets branch' '
	git checkout branch1 &&

	do_checkout branch2 "" -f
'

test_expect_success 'checkout -f -b to an existing branch resets branch (explicit ref)' '
	git checkout branch1 &&

	do_checkout branch2 $HEAD1 -f
'

test_expect_success 'checkout -f -b to an existing branch resets branch (dirty) ' '
	git checkout branch1 &&

	setup_dirty &&
	do_checkout branch2 $HEAD1 -f &&
	test_must_fail test_dirty
'

test_done
