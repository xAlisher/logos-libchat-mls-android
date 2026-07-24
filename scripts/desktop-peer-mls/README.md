# desktop-peer-mls — headless interop rig for the MLS/address libchat

A scriptable, headless peer that speaks the exact wire protocol the Android app
speaks, for interop testing (reverse-leg 1:1, MLS groups) without a phone on the
other end. It dlopens the **HOST (x86_64)** build of this repo's wrapper
(`liblogoschat.so`, the C ABI in `include/liblogoschat.h`, wrapping the pure-Rust
`libchat` facade the desktop Basecamp `chat_module` also links).

It is the M2' successor to the old intro-bundle `scripts/desktop-peer` in the app
repo: persistent identity + stable hex address + 1:1 + MLS groups, raw-UTF-8
message bytes (no hex codec).

## Build the host wrapper (one-time)

From a checkout of the libchat workspace at the pinned ref (see `docs/BUILD.md`),
with the wrapper crate vendored as `extensions/liblogoschat-android` and the
delivery build.rs android/relocatable patch applied:

```bash
# host x86_64 delivery node + RLN (the installed Basecamp module ships them)
export LOGOS_DELIVERY_LIB_DIR="$HOME/.local/share/Logos/LogosBasecamp/modules/delivery_module"
export LOGOS_DELIVERY_RELOCATABLE=1
export CARGO_TARGET_DIR=/extra/tmp/libchat-mls-build/target-host
cargo build --release -p liblogoschat-android
# -> $CARGO_TARGET_DIR/release/liblogoschat.so  (x86_64, NEEDED liblogosdelivery.so)
```

No `--target` = host build. aws-lc-rs / openmls / rustls all build natively with
the system toolchain (cmake + gcc). ~1–2 min.

## Run

```bash
LIB=/extra/tmp/libchat-mls-build/target-host/release/liblogoschat.so \
  ./scripts/desktop-peer-mls/peer.sh
```

`peer.sh` compiles `peer.c`, points `LD_LIBRARY_PATH` at the host delivery dir
(so the wrapper's `NEEDED liblogosdelivery.so` — and `librln.so` beside it —
resolve), opens a persistent identity + encrypted store under `/tmp/logoschat-peer`,
prints `PEER ADDRESS: <hex64>`, then `READY`, then reads commands on stdin.

Env knobs: `LIB`, `DELIVERY_DIR`, `WORKDIR`, `REGISTRY` (empty = baked-in default).

## Commands (stdin, one per line)

| command | effect |
|---|---|
| `address` | reprint my hex address |
| `list` | `list_conversations()` → JSON array of ids |
| `newconvo <addr> [nick]` | `create_conversation(addr)` → prints `CONVO: <id>` (nick is a local label, ignored by the lib) |
| `send <convoId> <text…>` | `send_message(convoId, utf8 bytes)` — 1:1 |
| `newgroup <name> \| <desc>` | `create_group(name, desc)` → prints `GROUP: <id>` (desc optional, after a literal ` \| `) |
| `addmember <groupId> <addr>` | `add_group_member(groupId, addr)` |
| `groupsend <groupId> <text…>` | `send_message(groupId, utf8 bytes)` — same verb as `send`, kept for clarity |
| `quit` | `shutdown` + exit |

Every inbound event prints as a timestamped line:
`[EVT hh:mm:ss.mmm] type=<n> <name> json=<json>` — the delivery node also logs to
stdout (chronicles); filter with `grep -E 'PEER|EVT|CONVO|GROUP|SEND|ADDMEMBER'`.

## Scripted use

Drive it from a FIFO or a here-doc. Reverse-leg 1:1 example (peer sends, phone
receives): create a conversation to the phone's address, then send:

```bash
mkfifo /tmp/peer.in
./scripts/desktop-peer-mls/peer.sh < /tmp/peer.in &
exec 3>/tmp/peer.in
echo "newconvo <PHONE_ADDRESS>" >&3   # note the CONVO: <id> it prints
echo "send <id> hi-from-peer" >&3
```

Groups: `newgroup demo | a test group` → `addmember <groupId> <PHONE_ADDRESS>` →
`groupsend <groupId> hello-group`. The joiner surfaces a `conversation_started`
(class GroupV2) then `members_changed`, then the message. GroupV2 (de-mls) needs
a commit round to settle — allow a few seconds between add and the first send.
