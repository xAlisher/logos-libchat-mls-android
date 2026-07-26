# libchat-mls arm64 build — fork-tree log

Red-team fork-tree discipline (`~/fieldcraft/protocols/red-team-fork-tree.md`):
every idea → move → result → wall → insight, dead ends included. This is the
executable record of cross-compiling the new pure-Rust `libchat` (MLS/address
generation) into an Android arm64 JNI `cdylib`.

- **libchat ref:** `d2124fd07c206efe901dac67953d9da7d0f8bca9` (main, 2026-07-24)
- **transitive pins:** chat_proto @ `37ec98a`, de-mls @ `2c7a8669`
- **delivery node:** prebuilt `liblogosdelivery.so` (+ `librln.so`, `libc++_shared.so`)
  from `xAlisher/logos-libdelivery-android` @ v0.1.0 — ABI verified below.
- **NDK:** r27 `27.1.12297006`, `aarch64-linux-android30-clang`
- **build dir:** `/extra/tmp/libchat-mls-build`

---

## Node 0 — delivery ABI match (M0' pre-check)  ✅ WIN

**Idea:** the wrapper links `extensions/logos-delivery-rust` which declares the
`logosdelivery_*` C ABI in `src/sys.rs` (no `#[link]`; build.rs links the .so).
Confirm our shipped arm64 `liblogosdelivery.so` exports exactly those symbols
before building anything on top.

**Move:** `llvm-nm -D --defined-only` on the prebuilt .so; diff against sys.rs.

**Result:** all 9 symbols sys.rs declares are present (create_node, start_node,
stop_node, destroy, subscribe, unsubscribe, send, set_event_callback,
get_node_info) — plus 5 extra (channel_*, get_available_*). 14 total. No drift.
`libc++_shared.so` in DT_NEEDED. **Zero ABI work needed** — drop the .so in.

---

## Node 1 — build.rs panics on android  ✅ FIX

**Idea (from scoping §5):** `extensions/logos-delivery-rust/build.rs` hard-panics
on any `target_os` other than macos/linux.

**Wall (exact):** `panic!("unsupported OS for logos-delivery transport: {other}")`
at the `match target_os` arm — would fire for `android`.

**Move:** add `"android"` to the accepted arm (treat like linux). In relocatable
mode (`LOGOS_DELIVERY_RELOCATABLE=1`) the absolute-stamp path (patchelf) is never
taken, so android just needs `rustc-link-search` + `rustc-link-lib=dylib=logosdelivery`.

**Insight:** #185 (the exact HEAD commit) added `LOGOS_DELIVERY_RELOCATABLE` — the
"copy the .so into the consumer bundle" mode an APK needs. The android patch is a
one-line addition to a match; the relocatable machinery already exists upstream.

---

## Node 2 — alloy default-features drag  ✅ PARE (pre-emptive, before first build)

**Idea (from scoping §5 wall 2):** `core/conversations` depends on `alloy = "2.0"`
with no feature list = full provider stack (tokio + hyper + a *second*
reqwest/rustls tree). Huge cross-compile surface.

**Move:** grepped actual usage — alloy appears in exactly ONE line:
`core/conversations/src/conversation/group_v2.rs:10 use alloy::signers::local::PrivateKeySigner`
(used once: `PrivateKeySigner::random()`). Pared to
`alloy = { version = "2.0", default-features = false, features = ["signer-local"] }`.

**Insight:** the scoping doc guessed "pare to alloy-primitives" but the real
dependency is `signer-local` (a narrow signer crate), NOT the provider stack.
`default-features=false, features=["signer-local"]` keeps the one thing used and
drops tokio/hyper/reqwest-0.13 entirely. High-value, low-effort — done before the
first build so the first build's cost already reflects it.

---

## Node 3 — the cdylib wrapper crate

**Move:** authored `extensions/liblogoschat-android` (`crate-type=["cdylib","staticlib"]`,
lib name `logoschat`), added to workspace members. Depends on `logos-chat` (facade),
`logos-account` (for the network-free address mint), `logos-generic-chat`.

C ABI exposed (mirrors the desktop 15-verb / event contract, thinned for M0'):
`logoschat_gen_address` (network-free mint — the smoke's core), `logoschat_open`,
`logoschat_get_address`, `logoschat_installation_name`, `logoschat_create_conversation`,
`logoschat_create_group`, `logoschat_add_group_member`, `logoschat_list_conversations`,
`logoschat_send_message`, `logoschat_set_event_callback` (spawns an event pump),
`logoschat_shutdown`, `logoschat_last_error`, `logoschat_free_string`.

Workspace is `panic="abort"` → every entry point returns null/-1 + a thread-local
error string instead of unwinding (no `catch_unwind`).

---

## Node 4 — first arm64 build  ✅ ALL DEP WALLS CLEARED

**Move:** `cargo build --release --target aarch64-linux-android -p liblogoschat-android`
with the env in `env.sh` (NDK r27 + `AWS_LC_SYS_CMAKE_BUILDER=1` + delivery lib
dir + `LOGOS_DELIVERY_RELOCATABLE=1`).

**Result — the headline:** the scoping doc's three predicted hard walls did NOT
block. Every dependency compiled clean for `aarch64-linux-android`:
- **aws-lc-rs** (`aws-lc-sys`) — built first try with `AWS_LC_SYS_CMAKE_BUILDER=1`
  + the NDK cmake toolchain env. **No ring swap needed.** (arm64 needs no NASM;
  that was the x86 pain.)
- **alloy** (`signer-local`) + **openmls 0.8.1** (libcrux provider) +
  **reqwest 0.12** (rustls) + **chat-proto** + **de-mls** — all compiled.
- **logos-delivery-rust link** — the relocatable link mode + `LOGOS_DELIVERY_LIB_DIR`
  resolved and linked `-llogosdelivery` with zero patchelf/nix.
- ~300 crates compiled; the ONLY failure was in our own wrapper crate.

**Wall (ours, trivial):** `E0277` — `thread::spawn` needs `Arc<SendPtr>: Send`
which needs `SendPtr: Sync`; we only `unsafe impl Send`. 

**Insight:** the whole "aws-lc-rs / alloy / rustls cross-compile" risk that framed
the go/no-go gate evaporated with the right env — the arm64 chat layer is genuinely
just `cargo build --target`. The real (tiny) work is in the wrapper.

## Node 5 — Send fix

**Move:** dropped the `Arc<SendPtr>`; move `SendPtr` directly into the pump thread
(it lives for the thread's lifetime, single-owner). Only `unsafe impl Send` needed.
Rebuild → **`liblogoschat.so` links, 16.7 MB (13.2 MB stripped), ELF ARM aarch64,
13 `logoschat_*` exports, `liblogosdelivery.so` in DT_NEEDED** (relocatable soname).

## Node 6 — on-device smoke (Samsung SM-G780G)  ✅ WIN

**Move:** cross-compiled `smoke.c` with NDK clang; pushed the 4 `.so`s + driver to
`/data/local/tmp`; ran `LD_LIBRARY_PATH=. ./smoke` (and `./smoke open`).

**Result:**
- `logoschat_gen_address` → **64-hex account address** printed on the physical
  arm64 device, network-free (dlopen pulls in liblogosdelivery + librln +
  libc++_shared successfully). **This is the M0' SUCCESS assertion.**
- `logoschat_open` (full stack) → started the embedded delivery node, published
  the device bundle to `devnet.chat-kc.logos.co`, opened encrypted SQLite, and
  returned a valid 64-hex address. Full stack functional on-device — a bonus
  beyond the M0' bar.

**Insight:** the arm64 chat layer is genuinely just `cargo build --target` + a
delivery `.so`. No Nim toolchain fight for the chat layer (all of that is inside
the reused delivery node).

## Node 7 — persistent identity (the §5/§8 make-or-break gate)  ✅ CLEARED

**Idea (scoping §3, "biggest single risk"):** `logos_chat::open()` mints
`TestLogosAccount::new()` + `DelegateSigner::random()` every call → the address
rotates per launch, orphaning persisted conversations. A stable address needs
load-or-create with from-bytes rehydration, which upstream doesn't expose
(only `::new()`/`::random()`; the signing keys are private fields).

**The move (a 4-crate additive fork, all in-tree):**
1. `core/crypto/src/signatures.rs` — add `Ed25519SigningKey::from_bytes(&[u8;32])`
   + `to_bytes() -> [u8;32]` (wrapping `ed25519_dalek::SigningKey::from_bytes` /
   `to_bytes`).
2. `core/account/src/account.rs` — add `TestLogosAccount::from_signing_key(...)`.
3. `crates/generic-chat/src/delegate.rs` — add `DelegateSigner::from_signing_key(...)`.
4. `crates/logos-chat/src/logos.rs` — add `generate_identity() -> ([u8;32],[u8;32])`
   and `open_persistent(config, account_seed, delegate_seed)` mirroring
   `open_with_transport` but rehydrating account+delegate from the seeds
   (address = `hex(verifying_key(account_seed))`, fixed by the seed). Re-export
   both from the facade.

The wrapper's `logoschat_open_persistent(db, key, registry, identity_path)` does
load-or-create: read a 64-byte seed file (account_seed ‖ delegate_seed) or
generate + write it, then `open_persistent`.

**Result (on-device, Samsung + Pixel):** `open_persistent` run twice — each fully
starting the node, publishing to the registry, opening storage, shutting down —
printed **the SAME 64-hex address both runs**:
- Samsung: `153208a8caf07ce3…3789a5f` == `153208a8caf07ce3…3789a5f` → **stable**.
- Pixel:   `aee70196be87f46f…56379a6c` == `aee70196be87f46f…56379a6c` → **stable**.

**Insight:** the persistent-identity gate the scoping doc flagged as the pivot's
make-or-break is a **small additive patch** (from-bytes constructors the crypto
substrate already supports — the seeds round-trip through `ed25519_dalek`), NOT a
deep fork. The address is now caller-owned and stable; M1' only needs to back the
seed file with an Android Keystore-encrypted blob. **The pivot delivers its core
persistent-address benefit — proven on two phones.**

---

## Root cause (the one sentence)

Cross-compiling the new pure-Rust MLS `libchat` to Android arm64 is a
`cargo build --target aarch64-linux-android` against a prebuilt delivery `.so`,
gated only by three env knobs (`AWS_LC_SYS_CMAKE_BUILDER=1`, an `alloy` feature
pare, `LOGOS_DELIVERY_RELOCATABLE=1`) and a one-line `android` arm in the
delivery build.rs — and a stable address across restarts needs nothing more than
adding `from_bytes` rehydration the crypto layer already round-trips.

---

## WALL: no MLS/conversation rehydration across a node restart (2026-07-24)

**Symptom (on-device).** Every conversation created in an EARLIER node session
fails on send with `send_message failed: convo with id: <id> was not found`.
Our app-side SQLite keeps the row + full history, so the thread looks healthy
while every send fails. Affects **1:1 AND groups** (logos-chat-android#103).

**Diagnosis (libchat @ d2124fd).** Four separate gaps, one of them decisive:

1. **DECISIVE — MLS state is in-memory only.**
   `core/conversations/src/inbox_v2/mls_provider.rs`:
   `impl OpenMlsProvider for MlsEphemeralPqProvider { type StorageProvider =
   openmls_memory_storage::MemoryStorage; }`
   `GroupV1Convo::load` does `MlsGroup::load(cx.mls_provider.storage(), &group_id)`,
   so after a restart the MLS group is ALWAYS absent regardless of any metadata we
   persist. A 1:1 is affected too because `DirectV1Convo` is a thin wrapper over
   `GroupV1Convo` (`type DelegateGroup = GroupV1Convo`).
2. `Core::new_with_name` (used by `ChatClient::new`, hence our `open_persistent`)
   is the **testing** constructor — it mints a fresh `Identity::new(...)` instead of
   `store.load_identity()`. `Core::new_from_store` is the persistent one and is unused.
3. `create_direct_convo_v1` and `create_group_convo_v2` never call
   `store.save_conversation(...)` (only `create_group_convo_v1` does), so
   `load_conversation_meta` -> `NoConvo`.
4. `ConversationKind` has only `{Unknown, GroupV1}` — no `DirectV1`/`GroupV2`, so
   `load_convo`/`load_group_convo` cannot reconstruct those kinds at all.

**Why it is tractable anyway.** `openmls_memory_storage` 0.5.0 exposes a public
`values: RwLock<HashMap<Vec<u8>,Vec<u8>>>` plus `serialize()`/`deserialize()` (and
ships a `persistence.rs`). So we can persist openmls's OWN key-value store verbatim
— no hand-written `StorageProvider` (~40 security-sensitive methods) required.

**FIX (shipped in `patches/libchat-android-arm64.patch`, additive, same style as
the M1' persistence patch).**
- `core/storage`: new `MlsStateStore` trait (`save_mls_state` / `load_mls_state`),
  added to the `ChatStore` supertrait bundle; `ConversationKind::GroupV2` added.
- `core/sqlite`: migration `003_mls_state.sql` (a single-row `mls_state` table) +
  `impl MlsStateStore for ChatStorage`. The blob holds MLS private key material,
  so it lives in the SAME encrypted database as the identity — never a plaintext
  sidecar file.
- `mls_provider.rs`: `MlsEphemeralPqProvider::from_storage(MemoryStorage)` plus
  `serialize_storage`/`deserialize_storage` over openmls' public `values` map.
  The wire layout matches upstream `MemoryStorage::{serialize,deserialize}`, but
  is re-implemented here because upstream gates those behind the test-only
  `test-utils` feature. No `StorageProvider` is hand-written.
- `Core::assemble`: rehydrates the provider from the store's blob when present.
  `Core::persist_mls_state()` (public) checkpoints it; called from
  `register_keypackage`, `create_direct_convo_v1`, `create_group_convo_v1/v2`,
  `group_add_member`, `send_content`, `handle_payload` and `wakeup` — the last
  four checkpoint even on error, since a partly applied MLS operation still
  mutated state. Exposed as `ChatClient::persist_mls_state`.
- Conversation meta is now saved on direct create (as `GroupV1`, which is what
  `DirectV1Convo` delegates to, so the existing `load_mls_convo` path rebuilds it)
  and on GroupV2 create.
- `ChatClient::new` uses `Core::new_from_store` (loads the stored installation
  identity) instead of the testing `new_with_name`, then calls
  `register_keypackage()` explicitly so a fresh last-resort key package is still
  published on every open — and that call now checkpoints the MLS state itself, so
  the key package's private init key survives the restart too.

**Proof (host x86_64, two separate processes against the same db + identity).**
Phase 1 `open_persistent` → `create_conversation(peer)` → shutdown; phase 2
`open_persistent` → `send_message(<that id>)` → `rc=0`, no error. The same driver
against a pre-fix build reproduces the bug verbatim:
`send_message failed: convo with id: <id> was not found`.

**LIMITATION — GroupV2 still cannot be rehydrated.** de-mls' `Conversation` only
offers `create`/`join`, no load path, so a GroupV2 built in an earlier session
cannot be reconstructed even with the MLS blob restored. Its meta row IS saved (so
it still shows in `list_conversations`) and `load_convo`/`load_group_convo` now
return an explicit `unsupported conversation type: group_v2 cannot be rebuilt from
storage: de-mls has no load path (rejoin the group)` instead of a misleading
"not found". Groups surviving a restart needs a load/serialize path upstream in
de-mls (or moving groups onto GroupV1, which does rehydrate).

---

## Node 8 — leave group (self-removal), verb #15 (2026-07-24)

**Idea.** The ABI could add members but never drop one — including yourself.
de-mls already models self-removal (`commit_round/apply.rs: leave_or_update(self_removed)`),
so a leave is the ordinary `Conversation::remove_member` round pointed at OUR OWN
member id — the value `group_v2.rs`'s existing `member_id(service_ctx)` helper
already returns.

**Move (additive, four layers + the header):**
- `core/conversations/src/conversation.rs` — `GroupConvo::leave(&mut self, cx)`
  with a default impl returning `UnsupportedFunction`, so only the kinds that
  model self-removal opt in (GroupV1 keeps the default).
- `core/conversations/src/conversation/group_v2.rs` — `GroupV2Convo::leave`
  calls `conversation.remove_member(provider, signer, &member_id(cx))`, then
  flushes through `after_op` even on failure (exactly as `add_member` does: a
  round that did open must be published and the wakeup re-armed). A new
  `map_leave_error` turns de-mls' `ConversationError::ConversationBlocked(state)`
  into a plain retry-later string instead of leaking the raw variant.
- `core/conversations/src/core.rs` — `Core::group_leave(convo_id)`, mirroring
  `group_add_member`: cache hit → leave; cache miss → the existing load path;
  `persist_mls_state()` afterwards either way (a partly applied leave still
  mutated MLS state).
- `crates/generic-chat/src/client.rs` — `ChatClient::leave_group(convo_id)`.
- `wrapper/src/lib.rs` + `include/liblogoschat.h` —
  `int logoschat_leave_group(void *handle, const char *convo_id)`, same contract
  as `logoschat_add_group_member` (0 = ok, -1 = error, text via
  `logoschat_last_error`). **ABI goes 14 → 15 exports.**

**What leave actually does.** It does NOT remove you synchronously. It opens a
`RemoveMember` consensus round with you as the target and publishes the round's
frame to the group's delivery address; the commit that ejects you lands
asynchronously (`wakeup` surfaces it as `leave_requested`). `rc=0` therefore means
"the round opened and was published", not "you are out".

**Two honest constraints (say them in the UI, don't paper over them):**
1. **Consensus round.** de-mls only accepts a new round while the conversation is
   `ConversationState::Working`. Mid-round (a vote or commit in flight) it answers
   `ConversationBlocked(<state>)`, which we surface as
   `cannot leave right now: the group is mid consensus round (state: …); retry once
   it returns to Working`. This is retry-later, not a hard failure.
2. **Only for a group live in THIS session.** GroupV2 still cannot be rebuilt from
   storage (de-mls has no load path — see the WALL above), so a group created or
   joined in a previous session cannot be left: the cache-miss path returns
   `unsupported conversation type: group_v2 cannot be rebuilt from storage: de-mls
   has no load path (rejoin the group)`. A durable leave needs a de-mls load/
   serialize path upstream.

**Proof (host x86_64, headless).** `scripts/desktop-peer-mls/peer.c` gained a
`leave <groupId>` command. Driving it over the real delivery node:
`newgroup` → `GROUP: 7af3669447`, then `leave 7af3669447` →
`LEAVE rc=0 last_error=""`, immediately preceded in the node log by
`start publish Waku message … contentTopic=/logos-chat/1/01ab1eb30b80/proto` —
i.e. the verb reached de-mls, the round opened, and its frame went out on the
group's delivery address. A bogus id gives
`leave_group failed: convo with id: PLACEHOLDER was not found`, so the not-found
path is distinct from the protocol path. `cargo test --release --workspace`: 130
passed, 0 failed. arm64 build: 15 `logoschat_*` exports.

---

## Node 9 — group metadata (name + description), verb #16 (2026-07-24)

**The belief that was wrong.** We had assumed a device that *joined* a group never
learns the group's real name, because the `conversation_started` event carries only
`convoId` + `class` — so the app fell back to a placeholder name for joined groups.
That assumption is **false**.

**Where the name actually lives.** The group's name and description are an **MLS
group extension** (`GROUP_METADATA_EXTENSION_TYPE`, `core/conversations/src/
conversation/mls_extensions.rs`), i.e. part of the group *state* every member
holds. It is set at creation (`create_group_convo_v2(&signers, name, desc)`) and
rides to every joiner **in the welcome** — so a joiner holds exactly the values the
creator set. Desktop Basecamp already reads it; that is why the name we set on the
phone showed up correctly there.

It was public all the way up in upstream `d2124fd`, we simply never exposed it:
- `core/conversations/src/conversation/group_v2.rs` — `fn metadata(&self) -> Option<ConvoMetadata>` (reads the extension)
- `core/conversations/src/types.rs` — `pub struct ConvoMetadata { pub name, pub desc }`
- `core/conversations/src/core.rs` — `pub fn convo_metadata(&self, convo_id)`
- `crates/generic-chat/src/client.rs` — `pub fn group_metadata(&self, convo_id) -> Result<ConvoMetadata, ClientError>`

**Move (wrapper-only — NO libchat source change).** Unlike Node 8, nothing had to
be added to the fork: `logos_chat` re-exports `logos_generic_chat::*`, so
`ConvoMetadata` and `ChatClient::group_metadata` are already reachable from the
wrapper crate. `patches/libchat-android-arm64.patch` is **unchanged** by this node.

- `wrapper/src/lib.rs` — `logoschat_group_metadata(handle, convo_id) -> *mut c_char`,
  returning `{"name":"…","desc":"…"}` (same allocation/error style as
  `logoschat_list_conversations`: caller frees with `logoschat_free_string`, NULL on
  error with the reason in `logoschat_last_error`).
- `include/liblogoschat.h` — documented declaration. **ABI goes 15 → 16 exports**
  (the app's JNI bridge is rebuilt separately).

**Error, not an empty name.** `Core::convo_metadata` fails in three distinct ways
and we surface all three as NULL + a message rather than an empty string:
a **direct (1:1)** conversation → `UnsupportedFunction(convo_id, "implementation
coming")`; a **legacy group** with no metadata extension →
`UnsupportedConvoType("metadata is not available for this legacy convo_type")`;
an unknown id → `NoConvo`. An empty `name` therefore means the creator really did
pass an empty name — it never means "unknown".

**Proof (host x86_64, headless).** A small dlopen driver (modeled on
`scripts/desktop-peer-mls/peer.c`): `open_persistent` → `create_group("proof-name",
"proof-desc")` → `logoschat_group_metadata(groupId)` returned verbatim
`{"name":"proof-name","desc":"proof-desc"}` — matching what was passed in. The same
verb on a **direct** conversation id returned NULL with
`group_metadata failed: convo:<id> does not support implementation coming`.
`cargo test --release --workspace`: 130 passed, 0 failed. arm64 build: **16**
`logoschat_*` exports.

**Side wall found while building the proof.** `logoschat_create_conversation(self)`
— a 1:1 with your OWN address — **aborts** the process inside libchat
(`group_v1.rs: called Result::unwrap() on Err(CreateCommitError(
ProposalValidationError(DuplicateSignatureKey)))`; the workspace is `panic="abort"`).
A self-conversation is not a supported shortcut for tests; use a second published
address. Worth guarding in the app before a user can type their own address.

## Node 10 — group members (current roster), verb #17 (2026-07-25)

**The need.** The app must read a group's CURRENT roster (as account addresses) to
diff against a prior snapshot and detect who joined or left. The event stream only
signals *that* membership changed (`ConversationMembersChanged { convo_id }`), never
*who* — so the app needs a pull verb that returns the live member set.

**Move (wrapper-only — NO libchat source change).** Exactly like Node 9: nothing was
added to the fork. `logos_chat` re-exports `logos_generic_chat::*`, and
`ChatClient::group_members(&mut self, convo_id) -> Result<Vec<GroupMember>, ClientError>`
(`crates/generic-chat/src/client.rs:255`) with `pub struct GroupMember { account:
Option<IdentId>, local_identity: IdentId }` are already reachable from the wrapper crate.
`patches/libchat-android-arm64.patch` is **unchanged** by this node (verified: a clean
`git checkout -f d2124fd` + drop-created-files + `git apply --check` passes).

- `wrapper/src/lib.rs` — `logoschat_group_members(handle, convo_id) -> *mut c_char`,
  returning a JSON array `[{"account":"<hex>","device":"<hex>"}, …]`. `account` is the
  member's verified account hex, or JSON `null` when the directory can't confirm the
  claim (the member is still cryptographically in the group, listed by its device);
  `device` is `local_identity` as hex. Same allocation/error style as
  `logoschat_group_metadata` (`IdentId::as_str()` for the hex, `json_str` per field,
  caller frees with `logoschat_free_string`, NULL on error with the reason in
  `logoschat_last_error`). One entry per account (self included).

- `include/liblogoschat.h` — documented declaration. **ABI goes 16 → 17 exports.**

**Error, not an empty array.** `group_members` fails for a direct (1:1) conversation
and for an unknown `convo_id`; both surface as NULL + a message, never `[]`. An empty
array would mean a real group with no members, which cannot happen (the creator is
always in it).

**Proof (host x86_64, headless).** A dlopen driver (modeled on
`scripts/desktop-peer-mls/peer.c`): `open_persistent` → print `logoschat_get_address`
→ `create_group("m","")` → `logoschat_group_members(groupId)` returned verbatim
`[{"account":"a9474cdb…dc6ad830","device":"6e6ceb…05fb4511"}]` — a valid array whose
sole `account` equals the printed self address `a9474cdb…dc6ad830` (creator = only
member). The same verb on a non-group / unknown convo id returned NULL with
`group_members failed: convo with id: 0000…0000 was not found`. (The Node-9 self-1:1
libchat abort still stands, so the error case uses an unknown id rather than a real
1:1 — same NULL+error branch.) `cargo test --release --workspace`: all green, 0 failed.
arm64 build: **17** `logoschat_*` exports.

---

## Node 11 — SDS reliable channels (delivery backfill/repair), logos-chat-android#211 (2026-07-26)

**Goal.** Migrate the delivery layer from plain Waku pub/sub (`logosdelivery_send` +
`logosdelivery_subscribe`, no backfill — a message published while the receiver has no
peer is lost forever) to the SDS **reliable channels** the prebuilt
`liblogosdelivery.so` already exports (`logosdelivery_channel_create/send/close`,
backed by `reliable_channel.nim` + sds-0.3.0: causal history, `missingDeps`,
sender-driven retransmission), so a receiver that was briefly offline backfills what
it missed. Keep the public Delivery API stable (embedded-logos-delivery + liblogoschat
unchanged). Branch `feat/sds-reliable-channels`.

### STEP 1 — the channel inbound event JSON (from upstream source, verbatim)

`logos-messaging/logos-delivery@master` `library/logos_delivery_api/node_api.nim`
(the `ChannelMessageReceivedEvent` listener wired in `logosdelivery_start_node`):

```json
{"eventType":"channel_message_received","channelId":"<id>","senderId":"<sds id>","payload":"<base64>"}
```

- No `contentTopic` (the channel owns it), **no** `messageId` / `missingDeps` /
  `repaired` flag. SDS-R repair is **transparent**: a repaired/backfilled message is
  surfaced as an ordinary `channel_message_received` — there is no separate event.
- Send-side events (`newJsonEvent`, flattened):
  `{"eventType":"channel_message_sent","channelId","requestId"}` and
  `{"eventType":"channel_message_error","channelId","requestId","error"}`.
- Config: `channels` are enabled by the current flat config already — upstream
  `logos_delivery_conf_json.nim`'s legacy `parseFlatConf` builds the full stack with
  `channelsConf: Opt.some`. A structured `channelsOverrides` block can NOT be mixed
  with the bare kernel fields (`tcpPort`, …) we send, so SDS defaults apply
  (`reliable_channel_manager.nim`/`scalable_data_sync.nim`): 5 s ack timeout,
  5 retransmissions (~25 s window), causal-history size 2.
- Receive requires BOTH a plain Waku subscription AND `channel_create`: the channel's
  ingress just filters the node's existing `MessageReceivedEvent` broker stream by
  content-topic + the `meta` spec marker `RELIABLE-CHANNEL-API/1`
  (`reliable_channel.nim:onMessageReceived`); `channel_create` does NOT subscribe Waku.

### STEP 2 — implementation (public Delivery API unchanged)

- `extensions/logos-delivery-rust/src/sys.rs` — added `logosdelivery_channel_create/
  send/close` extern decls.
- `wrapper.rs` — `LogosNodeCtx::{channel_create, channel_send, channel_close}` (same
  heap-boxed one-shot callback pattern as `subscribe`).
- `threaded.rs`:
  - `subscribe(topic)` (channel mode) → `node.subscribe(topic)` **and**
    `channel_create(channelId=topic, contentTopic=topic, senderId)`; `publish` →
    `channel_send(topic, {payload,ephemeral:false})`; `unsubscribe` →
    `channel_close` + `unsubscribe`. `channelId == content_topic` (trivial reverse map).
  - `WakuEvent::into_received` now also decodes `channel_message_received`
    (channelId → content_topic); a **mode-aware filter** drops the OTHER transport's
    events (in channel mode `message_received` carries raw SDS wire bytes and must not
    reach the app; in plain mode `channel_message_received` is dropped).
  - `senderId` = a process-unique non-empty id (an empty id disables SDS repair
    participation upstream).
- All shipped in `patches/libchat-android-arm64.patch`. Proof harness:
  `extensions/logos-delivery-rust/examples/channel_repair.rs` (+ `scripts/
  channel-repair-proof.sh`, `scripts/conn_diag.c`).

### STEP 3 — build ✅

`scripts/build-android-arm64.sh` reverts the checkout, reapplies the regenerated
patch, and rebuilds clean → `out/arm64-v8a/liblogoschat.so`, arm64 ELF, **17**
`logoschat_*` exports, `liblogosdelivery.so` in DT_NEEDED. Host build of the crate +
example also clean.

### STEP 4 — headless proof + WALLS

Two devices (Pixel + Samsung) and two host x86_64 processes on the real `logos.dev`
fleet. Findings, in order:

- **Connectivity is fine.** `conn_diag` (dlopen `liblogosdelivery.so` directly, query
  `Metrics`) shows `libp2p_peers 3.0→4.0` within seconds. The `no subscribed peers
  found` flood is only mix/rln filter noise. **Plain delivery works** phone↔phone and
  host↔host (baseline receiver gets the message); earlier total failures were the
  in-process dual-node case (nwaku has process-global state — one node per process)
  plus too-short warmup, not a transport bug.

- **WALL 1 — the shipped prebuilt lib predates the channel encryption fix.**
  On the arm64 prebuilt (`xAlisher/logos-libdelivery-android` v0.1.0, built from
  logos-delivery `7a3a064`, embedded date **2026-06-26**), `channel_send` returns a
  requestId but the message never reaches the wire; the send-side event stream shows:
  ```
  message_error:        "encryption failed: RequestBroker(Encrypt): no provider registered for input signature"
  channel_message_error:"one or more segments failed"
  ```
  `channel_send` does `Encrypt.request(...)` before publishing; with no provider the
  segment is dropped silently. Upstream fixed this in **#4051 "fix(channels): default
  channels to unencrypted so messages flow" (2dbf9a3, 2026-07-20)**, which calls
  `setNoopEncryption()` in `ReliableChannelManager.start()`. `git compare` confirms
  `7a3a064` is **14 commits behind** `2dbf9a3`. There is **no FFI to install an
  encryption provider from the app**, so the prebuilt's `channel_*` exports are
  non-functional. Fix = ship a liblogosdelivery built from ≥ #4051.

- Obtained a **post-#4051 host x86_64 lib** from logos-delivery CI artifact
  `liblogosdelivery` (2026-07-25, run 30176598972) — self-contained (no `librln`/
  `libc++_shared` in DT_NEEDED), exports `channel_*`. (nix route to build one was
  blocked: nix-daemon down, needs sudo; arm64 from-source needs nim ≥2.2 + Docker
  `cross` for RLN — not available here.)

- **WALL 2 — SDS parks the first / unidirectional message.** With the #4051 lib
  `channel_send` succeeds (no encryption error) and the SDS-wrapped WakuMessage (meta
  marker `RELIABLE-CHANNEL-API/1`, correct content-topic, payload "M0-live" visible in
  the envelope) **is received** by the channel node (`message_received` fires) — but
  the nim SDS ingest emits **no** `channel_message_received` and no error. Cause
  (`scalable_data_sync.nim:handleIncoming`): the message's `unwrapped.missingDeps.len
  > 0` (it references periodic SDS-sync IDs the receiver never saw), so it is **parked
  in `pendingContent`** pending repair. A single unidirectional message therefore
  never surfaces.

- **Channel delivery WORKS with bidirectional / streaming traffic (proven).**
  Two channel peers, receiver also sends once to bootstrap the SDS sync, sender then
  streams: receiver got `M1,M2,M3`, sender got the receiver's `R-hello`. So the SDS
  sync establishes once both directions exchange, and messages flow both ways.

- **Late-joiner catch-up WORKS; total-offline-gap backfill did NOT (this fleet).**
  Repair scenario (`scripts`/`host-repair2.sh`): A streams a channel; B is online for
  the baseline (`GOT PRE-1..3`), is KILLED (fully offline), A sends `GAP-1/GAP-2` while
  nobody listens, then a **fresh** node B2 returns, bootstraps bidirectionally, and A
  keeps streaming. Result: **B2 caught up the entire ongoing stream `POST-1..14`**
  after re-establishing sync — i.e. a returning node re-syncs and receives live
  channel traffic (a real reliability gain over plain, where a returning node loses
  the mesh-formation window). BUT the two messages published **entirely during the
  offline gap** (`GAP-1/GAP-2`, never acknowledged by any peer) were **not** backfilled
  to the fully-restarted node. The `logos.dev` fleet peers advertise
  `agent_version=logos-delivery-v0.38.1` (2026-05) — well before the reliable-channel/
  SDS-R work — so SDS-R store/repair of never-acknowledged messages to a state-less
  returning node does not complete against the current public fleet.

### Root cause (one sentence)

The migration is a small, stable-API change (add `channel_*` to the FFI + route
`subscribe`→`subscribe`+`channel_create` and `publish`→`channel_send`, decode the
`channel_message_received` event), but end-to-end channel traffic needs a
liblogosdelivery built from ≥ #4051 (the shipped prebuilt drops every send for lack
of an encryption provider) **and** an SDS-aware fleet; with a #4051 host lib channel
delivery + late-joiner catch-up are proven, while backfill of messages sent during a
total offline gap to a state-less returning node does not yet complete against the
current v0.38.1 fleet.

### Shipping decision — channels gated OFF by default

Because neither available native binary delivers channel traffic reliably end-to-end
(prebuilt: silent send failure; #4051 host lib: no gap-backfill against v0.38.1),
defaulting to channels would silently drop chat traffic. The full channel path is
therefore implemented but **gated behind `LOGOS_DELIVERY_CHANNELS=1`**; plain
relay/filter stays the default. Flip the default in `threaded.rs` once (a) the app
ships a liblogosdelivery ≥ #4051 and (b) gap-backfill is confirmed against an
SDS-capable fleet. Escape hatch documented inline.
