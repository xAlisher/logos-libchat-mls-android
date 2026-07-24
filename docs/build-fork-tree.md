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
