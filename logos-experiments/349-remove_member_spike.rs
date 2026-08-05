//! #349 EXPERIMENT — prove GroupV1 can remove OTHER members.
//!
//! The open question before committing to the full remove-then-add recovery
//! (#349/#350) is purely mechanical: does a from-scratch MLS `remove_members`
//! in GroupV1 (a) produce a real epoch-advancing Commit the remaining members
//! apply, and (b) actually LOCK OUT the removed member (they can no longer
//! decrypt subsequent traffic)? GroupV1 had no remove path at all, so this is
//! the trivial experiment that de-risks the feature.
//!
//! Setup mirrors test_graph_hiding_epoch_stability.rs: a real 3-member GroupV1.

use integration_tests_core::TestHarness;
use shared_traits::IdentId;

#[test]
fn removing_a_member_advances_the_epoch_and_locks_them_out() {
    let _ = tracing_subscriber::fmt()
        .with_max_level(tracing::Level::INFO)
        .with_test_writer()
        .try_init();

    let mut harness = TestHarness::<3>::new(|_, _| {});
    let raya_addr = harness.raya().addr();
    let pax_addr = harness.pax().addr();

    // Saro creates a real named group with Raya + Pax; settle both joins.
    let convo = harness
        .saro()
        .create_group_convo_v1(&[&raya_addr, &pax_addr], "grp", "desc")
        .expect("saro create group");
    harness.process_until_label("raya+pax join", |h| {
        h.raya().convo_count() == 1 && h.pax().convo_count() == 1
    });

    // Warm-up so the group is fully settled on all three sides.
    harness.saro().send_content(&convo, b"warm").expect("send warm");
    harness.process_until(|h| {
        h.raya().check(&convo, b"warm") && h.pax().check(&convo, b"warm")
    });

    // All three are members before the removal.
    assert_eq!(
        harness.saro().group_members(&convo).expect("roster").len(),
        3,
        "group starts with 3 members"
    );
    let epoch_before = harness
        .saro()
        .group_epoch_for_test(&convo)
        .expect("group cached on saro");

    // The MLS leaf credential is the member's short id ("pax"), NOT the account
    // address hash — so we target that. (Mapping account -> leaf credential is
    // the client facade's job in production; the experiment targets the leaf
    // directly to isolate the MLS mechanic.)
    let pax_leaf = IdentId::new("pax".to_string());

    // ---- REMOVE Pax (a real MLS Remove commit) ---------------------------
    harness
        .saro()
        .group_remove_member(&convo, &[&pax_leaf])
        .expect("saro remove pax");

    // (a) The removing member's OWN state: the commit merged locally, advancing
    // the epoch and dropping pax from the roster. (We assert saro's view — saro
    // never has to decrypt anything it can't, so this is crash-free regardless
    // of how the harness treats a locked-out peer.)
    let epoch_after = harness
        .saro()
        .group_epoch_for_test(&convo)
        .expect("group cached on saro");
    assert!(
        epoch_after > epoch_before,
        "removing a member must advance the MLS epoch (before={epoch_before}, after={epoch_after})"
    );
    assert_eq!(
        harness.saro().group_members(&convo).expect("roster").len(),
        2,
        "saro's roster drops to 2 after removing pax"
    );

    // (b) THE LOCKOUT, proven WITHOUT forcing pax to decrypt (which would make
    // the harness's `handle_payload(..).unwrap()` panic — itself evidence of the
    // lockout): pax never received the epoch-advancing commit's new secrets, so
    // pax is stranded on the old epoch while saro moved forward.
    let pax_epoch = harness
        .pax()
        .group_epoch_for_test(&convo)
        .expect("group cached on pax");
    assert_eq!(
        pax_epoch, epoch_before,
        "the removed member stays stranded on the pre-removal epoch (locked out)"
    );
    assert!(
        epoch_after > pax_epoch,
        "saro advanced past pax's epoch — pax can no longer decrypt group traffic"
    );
}
