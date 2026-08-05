# Experiment #349 — can GroupV1 remove OTHER members? (result: YES)

**Question (the risk before committing to remove-then-add recovery #349/#350):**
GroupV1 is the production group type (#103) and had **no remove path at all** — not
even self-leave. Before building the full feature, prove the core MLS mechanic: does a
from-scratch `MlsGroup::remove_members` in GroupV1 (a) produce a real epoch-advancing
Commit, and (b) actually lock out the removed member?

**Result: PASSED** (`cargo test -p integration_tests_core --test remove_member_spike`,
host target, 1 passed). The remove executes, merges locally, advances the MLS epoch, and
the removed member is stranded on the old epoch (cannot decrypt subsequent traffic).

## Minimal implementation the experiment exercised
All in the gitignored build clone (`libchat-build/`, = upstream `d2124fd` + our
`patches/`); to productionize #349 these move into `patches/libchat-android-arm64.patch`.

1. **Trait** `core/conversations/src/conversation.rs` — new `GroupConvo::remove_member(cx, &[IdentIdRef])`, default `UnsupportedFunction` (mirrors `leave`).
2. **GroupV1** `.../conversation/group_v1.rs` — impl: resolve each target signer id to its
   MLS leaf by matching `member.credential.serialized_content()` (== the member's
   `id().as_str()` bytes, per `inbox_v2::identity`), then
   `mls_group.remove_members(provider, signer, &leaves)` → `merge_pending_commit` →
   `send_payload(commit)`. Symmetric to `add_member` (no Welcome for removal).
3. **Core** `.../core.rs` — `group_remove_member` + `_inner` (mirrors `group_leave`,
   checkpoints MLS state even on failure).
4. **Client facade** `crates/generic-chat/src/client.rs` — `remove_group_member(convo, accounts)`
   (mirrors `add_group_members`; resolves accounts → signers).

## Findings that shape the real #349 (beyond "it works")
- **Leaf identity ≠ account address.** The MLS leaf credential is the member's short
  `id()` (e.g. `"pax"`), NOT the account-address hash the app holds (`a18c8282…`). The
  client facade must map account → leaf credential (the reverse of what
  `client.group_members` already does via the directory). The experiment targeted the
  leaf id directly to isolate the MLS mechanic.
- **The test harness `handle_payload(..).unwrap()`s** — so a removed member draining
  undecryptable traffic *panics the harness*. That crash is itself proof of lockout, but
  a clean regression test must prove lockout via epoch divergence (as this one does),
  or the harness needs to tolerate a locked-out peer.

## Remaining for #349 (productionize) + #350 (auto recovery)
- Capture the impl into `patches/` (or upstream to `logos-messaging/libchat`), + creator-gating.
- Account→leaf-credential resolution in the client facade.
- FFI verb `logoschat_remove_group_member` → Kotlin `NodeBridge`/`LogosChatModule` → RN.
- #350: `readd:` marker → creator auto remove-then-add; wire the existing RN
  "Ask to be re-added" action to it (today it opens a pre-drafted 1:1 — manual).

See `349-remove_member_spike.rs` for the test.
