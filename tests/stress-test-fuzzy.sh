#!/bin/bash
#
# Stress-test interdiff --fuzzy by comparing every commit in a range
# against itself. Any non-empty output indicates a bug.
#
# Usage: ./test-commit-range.sh <git-tree> <commit-range>
#
# Output: only prints SHAs that produce non-empty interdiff output,
# along with the first line of that output for triage.

set -uo pipefail

if [ $# -ne 2 ]; then
	echo "Usage: $0 <git-tree> <commit-range>" >&2
	exit 1
fi

GIT_TREE="$1"
COMMIT_RANGE="$2"
INTERDIFF="${INTERDIFF:-interdiff}"
JOBS="${JOBS:-$(nproc)}"
TIMEOUT="${TIMEOUT:-30}"

test_commit () {
	local sha="$1"
	local patch out

	if ! patch=$(git -C "$GIT_TREE" format-patch -1 --stdout "$sha" 2>&1); then
		echo "FAIL $sha format-patch: $patch"
		return
	fi

	local rc=0
	out=$(timeout "$TIMEOUT" "$INTERDIFF" --fuzzy <(echo "$patch") <(echo "$patch") 2>&1) || rc=$?

	if [ "$rc" -eq 124 ]; then
		echo "FAIL $sha timed out after ${TIMEOUT}s"
	elif [ "$rc" -ne 0 ]; then
		echo "FAIL $sha exit $rc: $(echo "$out" | head -1)"
	elif [ -n "$out" ]; then
		echo "FAIL $sha $(echo "$out" | head -1)"
	fi
}

export -f test_commit
export GIT_TREE INTERDIFF TIMEOUT

git -C "$GIT_TREE" rev-list "$COMMIT_RANGE" | xargs -P "$JOBS" -I{} bash -c 'test_commit "$@"' _ {}
