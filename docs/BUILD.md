# Building `liblogoschat.so` for Android arm64-v8a

This repo cross-compiles the **new pure-Rust `libchat`** generation (stable hex
**addresses** + **MLS groups** + **persistent identity**) into an Android
arm64-v8a JNI `cdylib`, `liblogoschat.so`, plus its embedded delivery node.

- **Upstream ref:** `logos-messaging/libchat` @ `d2124fd07c206efe901dac67953d9da7d0f8bca9`
  (main, 2026-07-24). Transitive pins: `chat_proto@37ec98a`, `de-mls@2c7a8669`.
- **What we add:** a thin C-ABI wrapper crate (`wrapper/`) over the
  `logos_chat::open` facade, plus a small source patch
  (`patches/libchat-android-arm64.patch`) that (a) teaches
  `logos-delivery-rust/build.rs` about `android`, (b) pares `alloy` to
  `signer-local`, and (c) adds from-bytes rehydration + `open_persistent`/
  `generate_identity` for a **stable address across restarts**.
- **Delivery node:** the arm64 `liblogosdelivery.so` (+ `librln.so`,
  `libc++_shared.so`) is reused verbatim from
  [`xAlisher/logos-libdelivery-android`](https://github.com/xAlisher/logos-libdelivery-android)
  and vendored in `prebuilt/arm64-v8a/`. Its `logosdelivery_*` C ABI is an exact
  match for `logos-delivery-rust/src/sys.rs` (verified — no drift).

## Reproduce

```bash
export ANDROID_NDK_HOME=~/Android/Sdk/ndk/27.1.12297006
rustup target add aarch64-linux-android
bash scripts/build-android-arm64.sh
# -> out/arm64-v8a/{liblogoschat.so, liblogosdelivery.so, librln.so,
#                   libc++_shared.so, SHA256SUMS}
```

The script clones libchat @ the pinned ref, vendors the wrapper crate, applies
the patch, sets the NDK + `AWS_LC_SYS_CMAKE_BUILDER=1` +
`LOGOS_DELIVERY_RELOCATABLE=1` env, builds, strips, and verifies the arm64 ELF /
13 exports / delivery `DT_NEEDED`.

## The three "walls" — and why none blocked

The MLS-rebuild scoping doc flagged three cross-compile risks. With the right
env, **none blocked** (full story in `docs/build-fork-tree.md`):

1. **aws-lc-rs** (pulled by reqwest/rustls) — built first try with
   `AWS_LC_SYS_CMAKE_BUILDER=1` + the NDK cmake toolchain. No `ring` swap needed
   (arm64 needs no NASM — that was the x86 pain).
2. **alloy default-features** (tokio/hyper/reqwest-0.13 drag) — `conversations`
   uses exactly one symbol (`alloy::signers::local::PrivateKeySigner`); pared to
   `default-features=false, features=["signer-local"]`.
3. **liblogosdelivery ABI** — the shipped arm64 node already exports the exact
   `logosdelivery_*` symbols `sys.rs` declares. Drop-in.

The only real build error was a trivial `Send`/`Sync` bug in the wrapper.

## Runtime / packaging

`liblogoschat.so` has `liblogosdelivery.so` in `DT_NEEDED` (relocatable soname —
the ship-into-APK mode from upstream #185). Ship all four `.so`s together in the
APK's `jniLibs/arm64-v8a` (needs `useLegacyPackaging`), or, for a bare smoke,
place them in one dir and `LD_LIBRARY_PATH=.`. `liblogosdelivery.so` +
`librln.so` need `libc++_shared.so` beside them.

## On-device smoke

`scripts/smoke.c` dlopens the lib and calls `logoschat_gen_address` (network-
free) and, with args, `logoschat_open` / `logoschat_open_persistent`:

```bash
TC=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64
$TC/bin/aarch64-linux-android30-clang scripts/smoke.c -o smoke -ldl
adb -s <serial> push prebuilt/arm64-v8a/*.so smoke /data/local/tmp/lc/
adb -s <serial> shell 'cd /data/local/tmp/lc && LD_LIBRARY_PATH=. ./smoke persist'
```

Verified 2026-07-24 on a Samsung SM-G780G (arm64, Android 13) and a Pixel
(arm64): `gen_address` prints a 64-hex address; `open_persistent` prints the
**same** address across two runs (persistent-identity gate cleared).
