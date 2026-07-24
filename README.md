# logos-libchat-mls-android

Android **arm64-v8a** build of the **new pure-Rust `libchat`** generation —
stable hex **addresses**, **MLS groups** (OpenMLS / de-mls), and **persistent
identity** — cross-compiled into a JNI `cdylib` (`liblogoschat.so`) for the
[logos-chat-android](https://github.com/xAlisher/logos-chat-android) app.

This is the MLS-rebuild sibling of
[`xAlisher/logos-libchat-android`](https://github.com/xAlisher/logos-libchat-android)
(the older ephemeral intro-bundle build). It wraps
[`logos-messaging/libchat`](https://github.com/logos-messaging/libchat) @
`d2124fd` in a thin C ABI and reuses the proven arm64 delivery node from
[`xAlisher/logos-libdelivery-android`](https://github.com/xAlisher/logos-libdelivery-android).

## What's here

```
prebuilt/arm64-v8a/   liblogoschat.so (stripped) + liblogosdelivery.so + librln.so
                      + libc++_shared.so + SHA256SUMS   ← drop into jniLibs
include/              liblogoschat.h    ← the C ABI (13 verbs + event callback)
wrapper/              the cdylib wrapper crate source (logos_chat facade -> C ABI)
patches/              libchat-android-arm64.patch (android build.rs, alloy pare,
                      persistent-identity rehydration)
scripts/              build-android-arm64.sh (exact working sequence) + smoke.c
docs/                 BUILD.md, build-fork-tree.md (every wall + fix)
.github/workflows/    build.yml (from-source rebuild + artifact)
```

## C ABI (see `include/liblogoschat.h`)

Identity/lifecycle: `logoschat_gen_address` (network-free mint),
`logoschat_open`, `logoschat_open_persistent` (stable address across restarts),
`logoschat_shutdown`. Verbs: `get_address`, `installation_name`,
`create_conversation`, `create_group`, `add_group_member`, `list_conversations`,
`send_message`, `set_event_callback`. Plus `last_error` + `free_string`. Mirrors
the desktop `chat_module` contract; compiled `panic="abort"` (errors -> null/-1 +
thread-local message).

## Build

```bash
export ANDROID_NDK_HOME=~/Android/Sdk/ndk/27.1.12297006
rustup target add aarch64-linux-android
bash scripts/build-android-arm64.sh
```

See [docs/BUILD.md](docs/BUILD.md). The three scoped cross-compile walls
(aws-lc-rs, alloy, delivery ABI) are documented with their exact fixes in
[docs/build-fork-tree.md](docs/build-fork-tree.md) — none blocked.

## Status

- **M0' (build + on-device smoke): done.** `liblogoschat.so` (arm64, 13 exports)
  smoked on a Samsung SM-G780G and a Pixel: `gen_address` -> 64-hex address; full
  `open()` -> embedded node + registry + address; `open_persistent()` -> **same
  address across restarts** (the persistent-identity gate).
- Next: M1' wires these verbs into the app's Kotlin/JS bridge and deletes the
  ephemeral intro-bundle apparatus.

Built autonomously as part of the logos-chat-android MLS rebuild.
