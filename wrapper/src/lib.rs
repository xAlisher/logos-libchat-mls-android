//! Thin C-ABI `cdylib` wrapper over the `logos_chat` facade for the Android app.
//!
//! Mirrors the desktop `chat_module` verb/event contract (get-address,
//! create-conversation, groups, send, events) over a flat C ABI the app's JNI
//! bridge binds by symbol name. The workspace builds with `panic = "abort"`, so
//! every entry point is written to never unwind: errors become a null return
//! plus a thread-local message readable via `logoschat_last_error`.
//!
//! `logoschat_gen_address` is a network-free helper: it mints a fresh account
//! and returns its hex address without starting the delivery node or touching
//! the registry — the M0' on-device smoke calls it to prove the address /
//! crypto path runs on arm64 even with no network.

use std::cell::RefCell;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::ptr;
use std::thread::{self, JoinHandle};

use crossbeam_channel::Receiver;
use logos_account::TestLogosAccount;
use logos_chat::{
    Event, LogosChatClient, LogosConfig, generate_identity, open, open_persistent,
};
use logos_generic_chat::GroupMetadata;

thread_local! {
    static LAST_ERROR: RefCell<Option<CString>> = const { RefCell::new(None) };
}

fn set_last_error(msg: impl Into<String>) {
    let s = msg.into();
    let c = CString::new(s).unwrap_or_else(|_| CString::new("error (nul in message)").unwrap());
    LAST_ERROR.with(|e| *e.borrow_mut() = Some(c));
}

/// Returns the last error string set on THIS thread, or "" if none. The pointer
/// is valid until the next fallible call on the same thread. Do not free.
#[unsafe(no_mangle)]
pub extern "C" fn logoschat_last_error() -> *const c_char {
    LAST_ERROR.with(|e| match &*e.borrow() {
        Some(c) => c.as_ptr(),
        None => c"".as_ptr(),
    })
}

/// Free a `*mut c_char` returned by any `logoschat_*` function.
///
/// # Safety
/// `ptr` must be a pointer previously returned by this library (or null).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn logoschat_free_string(ptr: *mut c_char) {
    if !ptr.is_null() {
        unsafe { drop(CString::from_raw(ptr)) };
    }
}

fn cstr<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(p) }.to_str().ok()
}

fn to_c_string(s: impl Into<Vec<u8>>) -> *mut c_char {
    match CString::new(s) {
        Ok(c) => c.into_raw(),
        Err(_) => ptr::null_mut(),
    }
}

// ── Network-free address minting (M0' smoke) ────────────────────────────────

/// Mint a fresh account and return its hex address (64 hex chars). No network,
/// no node — proves the account/crypto path on-device. Caller frees the result
/// with `logoschat_free_string`. Returns null only on an allocation failure.
#[unsafe(no_mangle)]
pub extern "C" fn logoschat_gen_address() -> *mut c_char {
    let account = TestLogosAccount::new();
    to_c_string(account.address())
}

// ── Full client handle ──────────────────────────────────────────────────────

/// Callback the app registers to receive events. `event_type` is a small int
/// (see EVENT_* constants), `json` is a UTF-8 JSON description of the event
/// (valid only for the duration of the call), `user_data` is passed through.
pub type EventCallback =
    unsafe extern "C" fn(event_type: c_int, json: *const c_char, user_data: *mut c_void);

// Event type tags handed to the callback.
const EVENT_CONVERSATION_STARTED: c_int = 1;
const EVENT_MESSAGE_RECEIVED: c_int = 2;
const EVENT_MEMBERS_CHANGED: c_int = 3;
const EVENT_INBOUND_ERROR: c_int = 4;

struct SendPtr {
    cb: EventCallback,
    user_data: *mut c_void,
}
// The app guarantees the callback + user_data are safe to invoke from the pump
// thread (documented in the header). Mark Send so we can move them into it.
unsafe impl Send for SendPtr {}

struct Handle {
    client: LogosChatClient,
    // Taken by `set_event_callback`; None once a pump owns it.
    events: Option<Receiver<Event>>,
    pump: Option<JoinHandle<()>>,
}

/// Open a Logos chat client: starts the embedded delivery node, publishes the
/// device bundle to the registry, and opens encrypted storage at `db_path`.
///
/// `registry_url` may be null to use the baked-in default. Returns an opaque
/// handle, or null on failure (see `logoschat_last_error`). This performs
/// network I/O and can block for seconds; call it off the UI thread.
///
/// # Safety
/// `db_path` and `db_key` must be valid C strings; `registry_url` valid or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn logoschat_open(
    db_path: *const c_char,
    db_key: *const c_char,
    registry_url: *const c_char,
) -> *mut c_void {
    let Some(db_path) = cstr(db_path) else {
        set_last_error("db_path is null or not UTF-8");
        return ptr::null_mut();
    };
    let Some(db_key) = cstr(db_key) else {
        set_last_error("db_key is null or not UTF-8");
        return ptr::null_mut();
    };

    let mut cfg = LogosConfig::new(db_path, db_key);
    if let Some(url) = cstr(registry_url) {
        cfg.set_registry_url(url);
    }

    match open(cfg) {
        Ok((client, events)) => {
            let handle = Box::new(Handle {
                client,
                events: Some(events),
                pump: None,
            });
            Box::into_raw(handle) as *mut c_void
        }
        Err(e) => {
            set_last_error(format!("open failed: {e}"));
            ptr::null_mut()
        }
    }
}

/// Open a Logos chat client with a **persistent identity** loaded (or created)
/// from `identity_path`: a 64-byte file holding the account + delegate seeds.
///
/// On first call the file is absent → two fresh seeds are generated and written;
/// on every later call they are read back, so the client keeps the SAME address
/// across restarts. Otherwise identical to `logoschat_open`. Returns a handle or
/// null (see `logoschat_last_error`). In production, `identity_path` should be an
/// Android Keystore-encrypted blob; the raw-file form here is for the M0' smoke.
///
/// # Safety
/// `db_path`, `db_key`, `identity_path` valid C strings; `registry_url` valid or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn logoschat_open_persistent(
    db_path: *const c_char,
    db_key: *const c_char,
    registry_url: *const c_char,
    identity_path: *const c_char,
) -> *mut c_void {
    let (Some(db_path), Some(db_key), Some(id_path)) =
        (cstr(db_path), cstr(db_key), cstr(identity_path))
    else {
        set_last_error("db_path/db_key/identity_path null or not UTF-8");
        return ptr::null_mut();
    };

    // Load-or-create the two 32-byte seeds.
    let (account_seed, delegate_seed) = match std::fs::read(id_path) {
        Ok(bytes) if bytes.len() == 64 => {
            let mut a = [0u8; 32];
            let mut d = [0u8; 32];
            a.copy_from_slice(&bytes[..32]);
            d.copy_from_slice(&bytes[32..]);
            (a, d)
        }
        _ => {
            let (a, d) = generate_identity();
            let mut blob = Vec::with_capacity(64);
            blob.extend_from_slice(&a);
            blob.extend_from_slice(&d);
            if let Err(e) = std::fs::write(id_path, &blob) {
                set_last_error(format!("could not persist identity to {id_path}: {e}"));
                return ptr::null_mut();
            }
            (a, d)
        }
    };

    let mut cfg = LogosConfig::new(db_path, db_key);
    if let Some(url) = cstr(registry_url) {
        cfg.set_registry_url(url);
    }

    match open_persistent(cfg, account_seed, delegate_seed) {
        Ok((client, events)) => Box::into_raw(Box::new(Handle {
            client,
            events: Some(events),
            pump: None,
        })) as *mut c_void,
        Err(e) => {
            set_last_error(format!("open_persistent failed: {e}"));
            ptr::null_mut()
        }
    }
}

unsafe fn handle<'a>(h: *mut c_void) -> Option<&'a mut Handle> {
    if h.is_null() {
        None
    } else {
        Some(unsafe { &mut *(h as *mut Handle) })
    }
}

/// This client's account address (the hex peers paste to reach it). Caller frees.
///
/// # Safety
/// `h` must be a handle from `logoschat_open`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn logoschat_get_address(h: *mut c_void) -> *mut c_char {
    match unsafe { handle(h) } {
        Some(h) => to_c_string(h.client.addr().to_string()),
        None => {
            set_last_error("null handle");
            ptr::null_mut()
        }
    }
}

/// This client's installation (device) name. Caller frees.
///
/// # Safety
/// `h` must be a handle from `logoschat_open`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn logoschat_installation_name(h: *mut c_void) -> *mut c_char {
    match unsafe { handle(h) } {
        Some(h) => to_c_string(h.client.installation_name()),
        None => {
            set_last_error("null handle");
            ptr::null_mut()
        }
    }
}

/// Create a 1:1 conversation with `peer_address` (hex). Returns the conversation
/// id, or null on failure. Caller frees.
///
/// # Safety
/// `h` a valid handle; `peer_address` a valid C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn logoschat_create_conversation(
    h: *mut c_void,
    peer_address: *const c_char,
) -> *mut c_char {
    let Some(h) = (unsafe { handle(h) }) else {
        set_last_error("null handle");
        return ptr::null_mut();
    };
    let Some(peer) = cstr(peer_address) else {
        set_last_error("peer_address is null or not UTF-8");
        return ptr::null_mut();
    };
    match h.client.create_direct_conversation(peer) {
        Ok(id) => to_c_string(id.to_string()),
        Err(e) => {
            set_last_error(format!("create_conversation failed: {e}"));
            ptr::null_mut()
        }
    }
}

/// Create a GroupV2 (MLS) conversation. Returns the conversation id. Caller frees.
///
/// # Safety
/// `h` a valid handle; `name`/`desc` valid C strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn logoschat_create_group(
    h: *mut c_void,
    name: *const c_char,
    desc: *const c_char,
) -> *mut c_char {
    let Some(h) = (unsafe { handle(h) }) else {
        set_last_error("null handle");
        return ptr::null_mut();
    };
    let name = cstr(name).unwrap_or("");
    let desc = cstr(desc).unwrap_or("");
    match h
        .client
        .create_group_conversation(&[], GroupMetadata::new(name, desc))
    {
        Ok(id) => to_c_string(id.to_string()),
        Err(e) => {
            set_last_error(format!("create_group failed: {e}"));
            ptr::null_mut()
        }
    }
}

/// Add `peer_address` (hex) to group `convo_id`. Returns 0 on success, -1 else.
///
/// # Safety
/// `h` a valid handle; `convo_id`/`peer_address` valid C strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn logoschat_add_group_member(
    h: *mut c_void,
    convo_id: *const c_char,
    peer_address: *const c_char,
) -> c_int {
    let Some(h) = (unsafe { handle(h) }) else {
        set_last_error("null handle");
        return -1;
    };
    let (Some(convo), Some(peer)) = (cstr(convo_id), cstr(peer_address)) else {
        set_last_error("convo_id/peer_address null or not UTF-8");
        return -1;
    };
    match h.client.add_group_members(convo, &[peer]) {
        Ok(()) => 0,
        Err(e) => {
            set_last_error(format!("add_group_member failed: {e}"));
            -1
        }
    }
}

/// Leave group `convo_id` (remove SELF from its roster). Returns 0 on success,
/// -1 else. The removal is a de-mls consensus round: it is merged by the group's
/// next commit, not when this call returns. Fails while the group is mid-round,
/// and for a group from a previous session (GroupV2 cannot be rebuilt from
/// storage) — `logoschat_last_error` says which.
///
/// # Safety
/// `h` a valid handle; `convo_id` a valid C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn logoschat_leave_group(h: *mut c_void, convo_id: *const c_char) -> c_int {
    let Some(h) = (unsafe { handle(h) }) else {
        set_last_error("null handle");
        return -1;
    };
    let Some(convo) = cstr(convo_id) else {
        set_last_error("convo_id null or not UTF-8");
        return -1;
    };
    match h.client.leave_group(convo) {
        Ok(()) => 0,
        Err(e) => {
            set_last_error(format!("leave_group failed: {e}"));
            -1
        }
    }
}

/// List conversation ids as a JSON array string, e.g. `["id1","id2"]`. Caller frees.
///
/// # Safety
/// `h` must be a valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn logoschat_list_conversations(h: *mut c_void) -> *mut c_char {
    let Some(h) = (unsafe { handle(h) }) else {
        set_last_error("null handle");
        return ptr::null_mut();
    };
    match h.client.list_conversations() {
        Ok(ids) => {
            let ids: Vec<String> = ids.into_iter().map(|i| i.to_string()).collect();
            to_c_string(serde_json::to_string(&ids).unwrap_or_else(|_| "[]".into()))
        }
        Err(e) => {
            set_last_error(format!("list_conversations failed: {e}"));
            ptr::null_mut()
        }
    }
}

/// The group's shared metadata as a JSON object string,
/// `{"name":"…","desc":"…"}`. Caller frees.
///
/// The name/description are set at group creation and live in an MLS group
/// extension — part of the group state EVERY member holds, so a device that
/// JOINED the group reads the same values the creator set (they ride along in
/// the welcome). Either field may be empty.
///
/// Returns null (see `logoschat_last_error`) for a direct conversation, for a
/// legacy group that carries no metadata extension, and for an unknown
/// `convo_id` — an error, never an empty name.
///
/// # Safety
/// `h` a valid handle; `convo_id` a valid C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn logoschat_group_metadata(
    h: *mut c_void,
    convo_id: *const c_char,
) -> *mut c_char {
    let Some(h) = (unsafe { handle(h) }) else {
        set_last_error("null handle");
        return ptr::null_mut();
    };
    let Some(convo) = cstr(convo_id) else {
        set_last_error("convo_id is null or not UTF-8");
        return ptr::null_mut();
    };
    match h.client.group_metadata(convo) {
        Ok(meta) => to_c_string(format!(
            r#"{{"name":{},"desc":{}}}"#,
            json_str(&meta.name),
            json_str(&meta.desc)
        )),
        Err(e) => {
            set_last_error(format!("group_metadata failed: {e}"));
            ptr::null_mut()
        }
    }
}

/// Encrypt and send `content` (`len` bytes) to `convo_id`. 0 on success, -1 else.
///
/// # Safety
/// `h` a valid handle; `convo_id` a valid C string; `content` points to `len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn logoschat_send_message(
    h: *mut c_void,
    convo_id: *const c_char,
    content: *const u8,
    len: usize,
) -> c_int {
    let Some(h) = (unsafe { handle(h) }) else {
        set_last_error("null handle");
        return -1;
    };
    let Some(convo) = cstr(convo_id) else {
        set_last_error("convo_id null or not UTF-8");
        return -1;
    };
    let bytes = if content.is_null() || len == 0 {
        &[][..]
    } else {
        unsafe { std::slice::from_raw_parts(content, len) }
    };
    match h.client.send_message(convo, bytes) {
        Ok(()) => 0,
        Err(e) => {
            set_last_error(format!("send_message failed: {e}"));
            -1
        }
    }
}

fn event_json(ev: &Event) -> (c_int, String) {
    match ev {
        Event::ConversationStarted { convo_id, class } => (
            EVENT_CONVERSATION_STARTED,
            format!(
                r#"{{"convoId":{},"class":{}}}"#,
                json_str(convo_id),
                json_str(&format!("{class:?}"))
            ),
        ),
        Event::MessageReceived {
            convo_id,
            content,
            sender,
        } => {
            let text = String::from_utf8_lossy(content);
            (
                EVENT_MESSAGE_RECEIVED,
                format!(
                    r#"{{"convoId":{},"content":{},"senderAccount":{},"senderLocal":{}}}"#,
                    json_str(convo_id),
                    json_str(&text),
                    match &sender.account {
                        Some(a) => json_str(a.as_str()),
                        None => "null".to_string(),
                    },
                    json_str(sender.local_identity.as_str())
                ),
            )
        }
        Event::ConversationMembersChanged { convo_id } => (
            EVENT_MEMBERS_CHANGED,
            format!(r#"{{"convoId":{}}}"#, json_str(convo_id)),
        ),
        Event::InboundError { message } => (
            EVENT_INBOUND_ERROR,
            format!(r#"{{"message":{}}}"#, json_str(message)),
        ),
        _ => (0, "{}".to_string()),
    }
}

fn json_str(s: &str) -> String {
    serde_json::to_string(s).unwrap_or_else(|_| "\"\"".to_string())
}

/// Register the event callback. Spawns a background pump that drains the client's
/// event stream and invokes `cb(event_type, json, user_data)` for each event.
/// Call once per handle. Returns 0 on success, -1 if already registered / bad handle.
///
/// # Safety
/// `h` a valid handle; `cb` a valid function pointer; `user_data` is passed
/// through unchanged and must remain valid until `logoschat_shutdown`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn logoschat_set_event_callback(
    h: *mut c_void,
    cb: EventCallback,
    user_data: *mut c_void,
) -> c_int {
    let Some(h) = (unsafe { handle(h) }) else {
        set_last_error("null handle");
        return -1;
    };
    let Some(rx) = h.events.take() else {
        set_last_error("event callback already registered");
        return -1;
    };
    let sink = SendPtr { cb, user_data };
    let pump = thread::spawn(move || {
        // Move `sink` into the pump; it lives for the pump's lifetime.
        let sink = sink;
        while let Ok(ev) = rx.recv() {
            let (ty, json) = event_json(&ev);
            if let Ok(cjson) = CString::new(json) {
                unsafe { (sink.cb)(ty, cjson.as_ptr(), sink.user_data) };
            }
        }
    });
    h.pump = Some(pump);
    0
}

/// Shut down and free the client. After this the handle is invalid.
///
/// # Safety
/// `h` must be a handle from `logoschat_open` (or null). Must not be used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn logoschat_shutdown(h: *mut c_void) {
    if h.is_null() {
        return;
    }
    // Reconstitute the Box so the client (and its worker threads) drop. The
    // event Receiver drops with it, which ends the pump loop; we detach the
    // pump join to avoid blocking shutdown on a callback in flight.
    let handle = unsafe { Box::from_raw(h as *mut Handle) };
    drop(handle);
}
