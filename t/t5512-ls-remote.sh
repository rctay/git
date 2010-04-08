#!/bin/sh

test_description='git ls-remote'

. ./test-lib.sh

test_expect_success setup '

	>file &&
	git add file &&
	test_tick &&
	git commit -m initial &&
	git tag mark &&
	git show-ref --tags -d | sed -e "s/ /	/" >expected.tag &&
	(
		echo "$(git rev-parse HEAD)	HEAD"
		git show-ref -d	| sed -e "s/ /	/"
	) >expected.all &&

	git remote add self "$(pwd)/.git"

'

test_expect_success 'ls-remote --tags .git' '

	git ls-remote --tags .git >actual &&
	test_cmp expected.tag actual

'

test_expect_success 'ls-remote .git' '

	git ls-remote .git >actual &&
	test_cmp expected.all actual

'

test_expect_success 'ls-remote --tags self' '

	git ls-remote --tags self >actual &&
	test_cmp expected.tag actual

'

test_expect_success 'ls-remote self' '

	git ls-remote self >actual &&
	test_cmp expected.all actual

'

cat >exp <<EOF
fatal: Where do you want to list from today?
EOF
test_expect_success 'dies with message when no remote specified and no default remote found' '

	test_must_fail git ls-remote >actual 2>&1 &&
	test_cmp exp actual

'

test_expect_success 'use "origin" when no remote specified' '

	git remote add origin "$(pwd)/.git" &&
	git ls-remote >actual &&
	test_cmp expected.all actual

'

test_expect_success 'use branch.<name>.remote if possible' '

	# Remove "origin" so that we know that ls-remote is not using it.
	#
	# Ideally, we should test that branch.<name>.remote takes precedence
	# over "origin".
	#
	git remote rm origin &&
	git config branch.master.remote self &&
	git ls-remote >actual &&
	test_cmp expected.all actual

'

test_done
