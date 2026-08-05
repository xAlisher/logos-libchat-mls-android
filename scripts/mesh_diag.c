// mesh_diag.c — #211 deep diag: does a fresh Core node, subscribed to the chat
// shard, actually GRAFT into the gossipsub mesh for that shard? A node can hold N
// connected peers yet have 0 MESH peers on a given shard — in which case shard
// traffic (our chat) silently never flows even though "connectivity is fine".
// dlopens liblogosdelivery.so directly (holds the ctx), so it can call the
// exported waku_relay_* peer/mesh verbs with no app/Rust rebuild.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void (*FFICallBack)(int, const char *, size_t, void *);
typedef void *(*create_fn)(const char *, FFICallBack, void *);
typedef int (*op_fn)(void *, FFICallBack, void *);
typedef int (*sub_fn)(void *, FFICallBack, void *, const char *);   // topic
typedef int (*peers_fn)(void *, FFICallBack, void *, const char *); // pubsubTopic

static char g_buf[65536];
static int g_ret = -999;
static volatile int g_done = 0;
static void cb(int ret, const char *msg, size_t len, void *ud) {
  (void)ud; g_ret = ret;
  size_t n = (msg && len) ? (len < sizeof(g_buf) - 1 ? len : sizeof(g_buf) - 1) : 0;
  if (n) memcpy(g_buf, msg, n);
  g_buf[n] = 0; g_done = 1;
}
static int wait_cb(void) { for (int i = 0; i < 200 && !g_done; i++) usleep(50 * 1000); return g_ret; }
static void call(peers_fn f, void *ctx, const char *arg, const char *label) {
  if (!f) { printf("  %-40s <symbol missing>\n", label); return; }
  g_done = 0; f(ctx, cb, NULL, arg); wait_cb();
  printf("  %-40s ret=%d -> %s\n", label, g_ret, g_buf);
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  const char *lib = argc > 1 ? argv[1] : "./liblogosdelivery.so";
  // The chat content topic seen on shard rs/2/7 in the node logs.
  const char *ctopic = argc > 2 ? argv[2] : "/kym/1/d3795c1d4f3306850dd1f422a3c962d2/proto";
  void *h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
  if (!h) { printf("dlopen fail: %s\n", dlerror()); return 1; }
  create_fn create = (create_fn)dlsym(h, "logosdelivery_create_node");
  op_fn start = (op_fn)dlsym(h, "logosdelivery_start_node");
  sub_fn sub = (sub_fn)dlsym(h, "logosdelivery_subscribe");
  peers_fn nconn = (peers_fn)dlsym(h, "waku_relay_get_num_connected_peers");
  peers_fn nmesh = (peers_fn)dlsym(h, "waku_relay_get_num_peers_in_mesh");
  if (!create || !start) { printf("dlsym fail: %s\n", dlerror()); return 1; }

  const char *cfg = "{\"logLevel\":\"ERROR\",\"mode\":\"Core\",\"preset\":\"logos.dev\","
                    "\"tcpPort\":60140,\"discv5UdpPort\":60140}";
  g_done = 0; void *ctx = create(cfg, cb, NULL);
  printf("create_node ctx=%p ret=%d\n", ctx, g_ret);
  if (!ctx) return 2;
  g_done = 0; start(ctx, cb, NULL); printf("start_node ret=%d\n", wait_cb());

  // Subscribe to the chat content topic so we should graft into its shard mesh.
  if (sub) { g_done = 0; sub(ctx, cb, NULL, ctopic); printf("subscribe(%s) ret=%d %s\n", ctopic, wait_cb(), g_buf); }

  // Poll connected vs mesh peers over time. "" = all/total.
  for (int t = 0; t < 6; t++) {
    printf("--- t=%ds ---\n", t * 15);
    call(nconn, ctx, "", "num_connected_peers(all)");
    call(nmesh, ctx, "/waku/2/rs/2/7", "num_peers_in_mesh(rs/2/7 = chat)");
    call(nmesh, ctx, "/waku/2/rs/2/0", "num_peers_in_mesh(rs/2/0 = meta)");
    sleep(15);
  }
  return 0;
}
