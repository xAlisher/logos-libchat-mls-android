#!/usr/bin/env bash
# Regenerate patches/349-groupv1-remove-member.patch from the current libchat-build
# working tree.
#
# The #349 work is kept as an ADDITIVE patch on top of the graph-hiding monolith
# (patches/libchat-android-arm64.patch) so it can never corrupt it. That means the
# durable source of truth is the patch file, not libchat-build/ (which is
# .gitignore'd). Editing the vendored tree and forgetting to regenerate silently
# drops the change from the reproducible build — including the #349 receive-side
# removal-authorization gate, whose absence is a security regression, not a build
# failure. Hence this script.
#
# How: reset the tree to pristine + monolith, stage THAT as the index, overlay the
# saved #349 files, and diff. The result is exactly the #349 delta.
#
#   bash scripts/regen-349-patch.sh
#   # then re-run scripts/build-android-arm64.sh to prove it applies + builds
set -euo pipefail

REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$REPO_DIR/libchat-build}
PATCH=$REPO_DIR/patches/349-groupv1-remove-member.patch

# Every file the #349 patch touches (modified or created).
FILES="
core/conversations/src/conversation.rs
core/conversations/src/conversation/group_v1.rs
core/conversations/src/conversation/mls_extensions.rs
core/conversations/src/core.rs
crates/generic-chat/src/client.rs
core/integration_tests_core/tests/remove_member_spike.rs
core/integration_tests_core/tests/remove_member_authorization.rs
"
# Files the patch CREATES (must not exist in the pristine+monolith baseline).
CREATED="
core/integration_tests_core/tests/remove_member_spike.rs
core/integration_tests_core/tests/remove_member_authorization.rs
"

cd "$BUILD_DIR"

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
for f in $FILES; do
  [ -f "$f" ] || { echo "missing $f in $BUILD_DIR"; exit 1; }
  mkdir -p "$STAGE/$(dirname "$f")"
  cp "$f" "$STAGE/$f"
done

echo "==> reset to pristine + monolith"
git checkout -f >/dev/null
for f in $CREATED; do rm -f "$f"; done
git apply --summary "$REPO_DIR/patches/libchat-android-arm64.patch" \
  | awk '/^ create mode /{print $4}' | xargs -r rm -f
git apply "$REPO_DIR/patches/libchat-android-arm64.patch"
git add -A

echo "==> overlay #349 and diff"
cp -r "$STAGE/." .
# shellcheck disable=SC2086
git add -N $CREATED
git diff > "$PATCH"
git reset >/dev/null

grep -q 'removes_authorized' "$PATCH" \
  || { echo "regenerated patch is missing the removal-authorization gate"; exit 1; }
echo "OK: $PATCH ($(grep -c '^--- a/\|^+++ b/.*remove_member' "$PATCH") file headers)"
