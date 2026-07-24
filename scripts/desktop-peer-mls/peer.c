// peer.c — headless desktop counterpart for the NEW MLS/address libchat.
//
// dlopens the HOST (x86_64) build of this repo's wrapper (liblogoschat.so — the
// exact C ABI include/liblogoschat.h, wrapping the pure-Rust libchat facade the
// desktop Basecamp chat_module also links). Persistent identity, stable hex
// address, 1:1 conversations AND MLS groups — the scriptable interop rig for the
// app's reverse-leg 1:1 and group tests.
//
// It opens a PERSISTENT identity + encrypted store under a temp dir, prints its
// address, registers the event callback (every event → a timestamped JSON line
// on stdout), then reads line commands on stdin:
//
//   address                       -> print my hex address again
//   newconvo <addr> [nick]        -> create_conversation(addr) (nick is a local label, ignored by the lib) -> print convoId
//   send <convoId> <utf8...>      -> send_message(convoId, bytes)         (1:1)
//   newgroup <name> | <desc>      -> create_group(name, desc) -> print groupId   (desc after a literal " | ")
//   addmember <groupId> <addr>    -> add_group_member(groupId, addr)
//   groupsend <groupId> <utf8...> -> send_message(groupId, bytes)         (same verb as send; kept for clarity)
//   list                          -> list_conversations() -> print JSON array
//   quit                          -> shutdown + exit
//
// Content is RAW UTF-8 bytes now (the old hex codec is gone).
//
// Build + run: see peer.sh (sets LD_LIBRARY_PATH to the host delivery .so dir so
// the wrapper's NEEDED liblogosdelivery.so resolves).
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef char *(*openp_fn)(void); // placeholder, real sigs below
typedef void *(*open_persistent_fn)(const char *, const char *, const char *, const char *);
typedef char *(*addr_fn)(void *);
typedef char *(*strv_fn)(void *);
typedef char *(*create_convo_fn)(void *, const char *);
typedef char *(*create_group_fn)(void *, const char *, const char *);
typedef int (*add_member_fn)(void *, const char *, const char *);
typedef int (*send_fn)(void *, const char *, const unsigned char *, size_t);
typedef char *(*list_fn)(void *);
typedef const char *(*err_fn)(void);
typedef void (*free_fn)(char *);
typedef void (*shutdown_fn)(void *);
typedef void (*event_cb)(int, const char *, void *);
typedef int (*set_event_cb_fn)(void *, event_cb, void *);

static void ts(char *buf, size_t n) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  struct tm tm;
  localtime_r(&tv.tv_sec, &tm);
  snprintf(buf, n, "%02d:%02d:%02d.%03ld", tm.tm_hour, tm.tm_min, tm.tm_sec,
           tv.tv_usec / 1000);
}

// Persistent event callback: copy nothing (json is NUL-terminated + valid for
// the call), print a timestamped JSON line, never re-enter the lib.
static void on_event(int event_type, const char *json, void *ud) {
  (void)ud;
  char t[32];
  ts(t, sizeof(t));
  const char *name = event_type == 1   ? "conversation_started"
                     : event_type == 2 ? "message_received"
                     : event_type == 3 ? "members_changed"
                     : event_type == 4 ? "inbound_error"
                                       : "unknown";
  printf("[EVT %s] type=%d %s json=%s\n", t, event_type, name, json ? json : "(null)");
  fflush(stdout);
}

static void *H;
static free_fn xfree;
static err_fn xerr;

static void die(const char *m) {
  fprintf(stderr, "PEER FAIL: %s (%s)\n", m, xerr ? xerr() : dlerror());
  exit(1);
}

int main(int argc, char **argv) {
  const char *lib = argc > 1 ? argv[1] : "./liblogoschat.so";
  const char *workdir = argc > 2 ? argv[2] : "/tmp/logoschat-peer";
  const char *registry = argc > 3 ? argv[3] : NULL; // NULL = baked-in default

  void *h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
  if (!h) {
    fprintf(stderr, "PEER FAIL: dlopen %s: %s\n", lib, dlerror());
    return 2;
  }
  printf("dlopen OK %s\n", lib);

  open_persistent_fn f_open = (open_persistent_fn)dlsym(h, "logoschat_open_persistent");
  addr_fn f_addr = (addr_fn)dlsym(h, "logoschat_get_address");
  strv_fn f_name = (strv_fn)dlsym(h, "logoschat_installation_name");
  create_convo_fn f_newconvo = (create_convo_fn)dlsym(h, "logoschat_create_conversation");
  create_group_fn f_newgroup = (create_group_fn)dlsym(h, "logoschat_create_group");
  add_member_fn f_addmember = (add_member_fn)dlsym(h, "logoschat_add_group_member");
  send_fn f_send = (send_fn)dlsym(h, "logoschat_send_message");
  list_fn f_list = (list_fn)dlsym(h, "logoschat_list_conversations");
  set_event_cb_fn f_setev = (set_event_cb_fn)dlsym(h, "logoschat_set_event_callback");
  shutdown_fn f_shutdown = (shutdown_fn)dlsym(h, "logoschat_shutdown");
  xfree = (free_fn)dlsym(h, "logoschat_free_string");
  xerr = (err_fn)dlsym(h, "logoschat_last_error");
  if (!f_open || !f_addr || !f_newconvo || !f_newgroup || !f_addmember || !f_send ||
      !f_setev || !xfree) {
    fprintf(stderr, "PEER FAIL: dlsym: %s\n", dlerror());
    return 3;
  }

  char db_path[1024], id_path[1024];
  { char cmd[1100]; snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", workdir); if (system(cmd)) {} }
  snprintf(db_path, sizeof(db_path), "%s/peer-store.db", workdir);
  snprintf(id_path, sizeof(id_path), "%s/peer-identity.bin", workdir);

  printf("opening (persistent) store=%s identity=%s registry=%s ...\n", db_path,
         id_path, registry ? registry : "(default)");
  fflush(stdout);
  H = f_open(db_path, "peerkey0123456789abcdef0123456789", registry, id_path);
  if (!H) die("open_persistent returned null");

  // Register the event pump BEFORE announcing readiness (events buffer on the
  // wrapper's crossbeam channel, so there is no early-loss window).
  if (f_setev(H, on_event, NULL) != 0) die("set_event_callback failed");

  char *addr = f_addr(H);
  char *name = f_name ? f_name(H) : NULL;
  printf("PEER ADDRESS: %s\n", addr ? addr : "(null)");
  printf("installation: %s\n", name ? name : "(null)");
  if (addr) xfree(addr);
  if (name) xfree(name);
  printf("READY\n");
  fflush(stdout);

  char line[65536];
  while (fgets(line, sizeof(line), stdin)) {
    line[strcspn(line, "\r\n")] = 0;
    if (line[0] == 0) continue;
    if (strcmp(line, "quit") == 0) break;

    if (strcmp(line, "address") == 0) {
      char *a = f_addr(H);
      printf("ADDRESS: %s\n", a ? a : "(null)");
      if (a) xfree(a);

    } else if (strcmp(line, "list") == 0) {
      char *j = f_list ? f_list(H) : NULL;
      printf("CONVERSATIONS: %s\n", j ? j : "(null)");
      if (j) xfree(j);

    } else if (strncmp(line, "newconvo ", 9) == 0) {
      char *addr_arg = line + 9;
      char *sp = strchr(addr_arg, ' ');
      if (sp) *sp = 0; // ignore the optional nick — a local label only
      char *id = f_newconvo(H, addr_arg);
      if (!id) printf("ERR newconvo: %s\n", xerr ? xerr() : "?");
      else { printf("CONVO: %s\n", id); xfree(id); }

    } else if (strncmp(line, "newgroup ", 9) == 0) {
      char *name_arg = line + 9;
      char *desc = "";
      char *bar = strstr(name_arg, " | ");
      if (bar) { *bar = 0; desc = bar + 3; }
      char *id = f_newgroup(H, name_arg, desc);
      if (!id) printf("ERR newgroup: %s\n", xerr ? xerr() : "?");
      else { printf("GROUP: %s\n", id); xfree(id); }

    } else if (strncmp(line, "addmember ", 10) == 0) {
      char *gid = line + 10;
      char *sp = strchr(gid, ' ');
      if (!sp) { printf("ERR usage: addmember <groupId> <addr>\n"); continue; }
      *sp = 0;
      char *peer = sp + 1;
      int rc = f_addmember(H, gid, peer);
      printf("ADDMEMBER rc=%d%s\n", rc, rc != 0 && xerr ? xerr() : "");

    } else if (strncmp(line, "send ", 5) == 0 || strncmp(line, "groupsend ", 10) == 0) {
      int off = (line[0] == 'g') ? 10 : 5;
      char *cid = line + off;
      char *sp = strchr(cid, ' ');
      if (!sp) { printf("ERR usage: send <convoId> <text>\n"); continue; }
      *sp = 0;
      char *text = sp + 1;
      int rc = f_send(H, cid, (const unsigned char *)text, strlen(text));
      printf("SEND rc=%d%s\n", rc, rc != 0 && xerr ? xerr() : "");

    } else {
      printf("ERR unknown command: %s\n", line);
    }
    fflush(stdout);
  }

  if (f_shutdown) f_shutdown(H);
  printf("BYE\n");
  return 0;
}
