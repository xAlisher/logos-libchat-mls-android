#!/usr/bin/env bash
# Build liblogoschat.so (the MLS/address libchat wrapper) for Android arm64-v8a.
#
# Reproduces prebuilt/arm64-v8a/liblogoschat.so from the pinned upstream
# `libchat` commit + the source patch in patches/ + the wrapper crate in
# wrapper/. See docs/BUILD.md and docs/build-fork-tree.md for the full story
# (every wall + fix). Green-on-first-CI is the goal: everything here is the
# exact working sequence from the local build.
#
# Requirements (Ubuntu 24.04 / GitHub Actions ubuntu-latest):
#   - rustup + `rustup target add aarch64-linux-android`
#   - Android NDK r27 (ANDROID_NDK_HOME)
#   - cmake, make, gcc, git, patch, perl  (aws-lc-rs builds via cmake)
#   - protoc / protobuf-compiler  (hashgraph-like-consensus builds via prost)
#
# Output: $OUT_DIR/liblogoschat.so (stripped) + the delivery .so's + SHA256SUMS
set -euo pipefail

# The exact upstream ref this wrapper was scoped and built against.
LIBCHAT_COMMIT=${LIBCHAT_COMMIT:-d2124fd07c206efe901dac67953d9da7d0f8bca9}
LIBCHAT_URL=${LIBCHAT_URL:-https://github.com/logos-messaging/libchat}
REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$REPO_DIR/libchat-build}
OUT_DIR=${OUT_DIR:-$REPO_DIR/out/arm64-v8a}

: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to an NDK r27 install}"
export ANDROID_NDK_ROOT=$ANDROID_NDK_HOME
TC=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64
CLANG=$TC/bin/aarch64-linux-android30-clang
STRIP=$TC/bin/llvm-strip
[ -x "$CLANG" ] || { echo "NDK clang not found: $CLANG"; exit 1; }
export PATH="$TC/bin:$HOME/.cargo/bin:$PATH"

echo "==> 1/6 clone libchat @ $LIBCHAT_COMMIT (+ submodules)"
if [ ! -d "$BUILD_DIR/.git" ]; then
  git clone "$LIBCHAT_URL" "$BUILD_DIR"
fi
cd "$BUILD_DIR"
git fetch origin "$LIBCHAT_COMMIT"
git checkout -f "$LIBCHAT_COMMIT"
git submodule update --init --recursive

echo "==> 2/6 vendor the wrapper crate + apply the android source patch"
mkdir -p extensions/liblogoschat-android/src
cp "$REPO_DIR/wrapper/Cargo.toml"   extensions/liblogoschat-android/Cargo.toml
cp "$REPO_DIR/wrapper/src/lib.rs"   extensions/liblogoschat-android/src/lib.rs
# Idempotent: skip if already applied (marker = the android arm in build.rs).
if ! grep -q '"macos" | "linux" | "android"' extensions/logos-delivery-rust/build.rs; then
  # `git checkout -f` above reverts TRACKED files but leaves the files the patch
  # ADDS behind as untracked, so a re-run on an existing BUILD_DIR would die with
  # "already exists in working directory". Drop exactly the patch's created files.
  for p in libchat-android-arm64 349-groupv1-remove-member 437-replace-on-desync-welcome 433-expose-group-creator; do
    git apply --summary "$REPO_DIR/patches/$p.patch" \
      | awk '/^ create mode /{print $4}' | xargs -r rm -f
  done
  git apply "$REPO_DIR/patches/libchat-android-arm64.patch"
  # #349: creator-gated remove-other-member for GroupV1 (additive; kept as a
  # separate patch so it never risks corrupting the main graph-hiding monolith).
  # Carries the receive-side removal-authorization gate + its tests.
  git apply "$REPO_DIR/patches/349-groupv1-remove-member.patch"
  # #437: replace-on-desync — a re-add welcome at a strictly newer epoch adopts the
  # fresh group instead of erroring GroupAlreadyExists, so #350 recovery actually
  # resyncs a desynced member (applied after 349; touches the same file).
  git apply "$REPO_DIR/patches/437-replace-on-desync-welcome.patch"
  # #433: expose the group's recorded creator (GroupConvo::creator → core.convo_creator
  # → client.group_creator), so a non-creator member can address the actual creator
  # instead of guessing the roster. Additive; applied after 437 (touches group_v1.rs +
  # the trait + core + client).
  git apply "$REPO_DIR/patches/433-expose-group-creator.patch"
fi
grep -q '"macos" | "linux" | "android"' extensions/logos-delivery-rust/build.rs \
  || { echo "patch did not apply"; exit 1; }
# #349: the receive-side gate is the security boundary — fail the build if the
# patch landed without it rather than shipping a silently unenforced removal.
grep -q 'removes_authorized' core/conversations/src/conversation/group_v1.rs \
  || { echo "#349 removal-authorization gate missing"; exit 1; }
# #437: fail the build if the replace-on-desync recovery path didn't land — else
# #350 auto-recovery silently reverts to stranding desynced members.
grep -q 'replace_old_group' core/conversations/src/conversation/group_v1.rs \
  || { echo "#437 replace-on-desync welcome path missing"; exit 1; }
# #437 security: the replace-on-desync path must stay creator-gated (author check) —
# fail the build if the gate is gone, else a non-creator fork could replace a group.
grep -q 'creator_credential_of' core/conversations/src/conversation/group_v1.rs \
  || { echo "#437 creator-authored replace gate missing"; exit 1; }
# #433: fail the build if the creator-exposure accessor didn't land, else #442
# ping-creator silently falls back to guessing the roster.
grep -q 'fn creator(&self) -> Option<String>' core/conversations/src/conversation.rs \
  || { echo "#433 GroupConvo::creator accessor missing"; exit 1; }

echo "==> 3/6 NDK + aws-lc-rs + delivery-node env"
export CC_aarch64_linux_android=$CLANG
export CXX_aarch64_linux_android=$TC/bin/aarch64-linux-android30-clang++
export AR_aarch64_linux_android=$TC/bin/llvm-ar
export RANLIB_aarch64_linux_android=$TC/bin/llvm-ranlib
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=$CLANG
# aws-lc-rs cross-compiles via the NDK cmake toolchain (arm64 needs NO nasm).
export CMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake
export ANDROID_ABI=arm64-v8a
export ANDROID_PLATFORM=android-30
export AWS_LC_SYS_CMAKE_BUILDER=1
# The embedded delivery node (nwaku) as an arm64 .so, ship-into-APK link mode.
DELIVERY_DIR=$REPO_DIR/prebuilt/arm64-v8a
[ -f "$DELIVERY_DIR/liblogosdelivery.so" ] || { echo "missing $DELIVERY_DIR/liblogosdelivery.so"; exit 1; }
export LOGOS_DELIVERY_LIB_DIR=$DELIVERY_DIR
export LOGOS_DELIVERY_RELOCATABLE=1

echo "==> 4/6 cargo build --release --target aarch64-linux-android -p liblogoschat-android"
cargo build --release --target aarch64-linux-android -p liblogoschat-android

echo "==> 5/6 stage + strip"
mkdir -p "$OUT_DIR"
cp target/aarch64-linux-android/release/liblogoschat.so "$OUT_DIR/"
cp "$DELIVERY_DIR/liblogosdelivery.so" "$DELIVERY_DIR/librln.so" \
   "$DELIVERY_DIR/libc++_shared.so" "$OUT_DIR/"
"$STRIP" --strip-unneeded "$OUT_DIR/liblogoschat.so"

echo "==> 6/6 verify"
file "$OUT_DIR/liblogoschat.so"
file "$OUT_DIR/liblogoschat.so" | grep -q 'ARM aarch64' || { echo "not an arm64 ELF"; exit 1; }
SYMS=$("$TC/bin/llvm-nm" -D --defined-only "$OUT_DIR/liblogoschat.so" | grep -c ' logoschat_')
echo "    exported logoschat_* symbols: $SYMS"
[ "$SYMS" -ge 14 ] || { echo "expected >=14 logoschat_* exports"; exit 1; }
# The wrapper must declare liblogosdelivery as a NEEDED (relocatable soname).
"$TC/bin/llvm-readelf" -d "$OUT_DIR/liblogoschat.so" | grep -q 'liblogosdelivery.so' \
  || { echo "liblogosdelivery.so missing from DT_NEEDED"; exit 1; }

( cd "$OUT_DIR" && sha256sum *.so > SHA256SUMS && cat SHA256SUMS )
echo "OK: $OUT_DIR"
