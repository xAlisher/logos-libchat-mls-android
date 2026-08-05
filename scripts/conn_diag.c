// conn_diag.c — dlopen liblogosdelivery.so directly, start a node, and report
// whether it connects to peers on the logos.dev fleet. Diagnostic for #211.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void (*FFICallBack)(int, const char *, size_t, void *);
typedef void *(*create_fn)(const char *, FFICallBack, void *);
typedef int (*op_fn)(void *, FFICallBack, void *);
typedef int (*info_fn)(void *, FFICallBack, void *, const char *);

static char g_buf[65536];
static int g_ret = -999;
static volatile int g_done = 0;

static void cb(int ret, const char *msg, size_t len, void *ud) {
  (void)ud;
  g_ret = ret;
  if (msg && len) {
    size_t n = len < sizeof(g_buf) - 1 ? len : sizeof(g_buf) - 1;
    memcpy(g_buf, msg, n);
    g_buf[n] = 0;
  } else {
    g_buf[0] = 0;
  }
  g_done = 1;
}

static int wait_cb(void) {
  for (int i = 0; i < 200 && !g_done; i++) usleep(50 * 1000);
  return g_ret;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  const char *lib = argc > 1 ? argv[1] : "./liblogosdelivery.so";
  void *h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
  if (!h) { printf("dlopen fail: %s\n", dlerror()); return 1; }

  create_fn create = (create_fn)dlsym(h, "logosdelivery_create_node");
  op_fn start = (op_fn)dlsym(h, "logosdelivery_start_node");
  info_fn getinfo = (info_fn)dlsym(h, "logosdelivery_get_node_info");
  op_fn getids = (op_fn)dlsym(h, "logosdelivery_get_available_node_info_ids");
  if (!create || !start) { printf("dlsym fail: %s\n", dlerror()); return 1; }

  const char *cfg =
      "{\"logLevel\":\"INFO\",\"mode\":\"Core\",\"preset\":\"logos.dev\","
      "\"tcpPort\":60130,\"discv5UdpPort\":60130}";
  g_done = 0;
  void *ctx = create(cfg, cb, NULL);
  printf("create_node ctx=%p ret=%d msg=%.200s\n", ctx, g_ret, g_buf);
  if (!ctx) return 2;

  g_done = 0;
  start(ctx, cb, NULL);
  printf("start_node ret=%d msg=%.200s\n", wait_cb(), g_buf);

  if (getids) {
    g_done = 0; getids(ctx, cb, NULL); wait_cb();
    printf("available_node_info_ids: %s\n", g_buf);
  }

  if (getinfo) {
    g_done = 0; getinfo(ctx, cb, NULL, "MyMultiaddresses"); wait_cb();
    printf("MyMultiaddresses: %.400s\n", g_buf);
  }
  for (int t = 0; t < 9; t++) {
    if (getinfo) {
      g_done = 0; getinfo(ctx, cb, NULL, "Metrics"); wait_cb();
      // Extract libp2p peer gauges from the prometheus metrics blob.
      char *p = g_buf;
      printf("--- t=%ds peer metrics ---\n", t * 10);
      while ((p = strstr(p, "libp2p_peers")) != NULL) {
        char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        if (p[0] != '#') printf("  %.*s\n", n, p);
        p += 12;
      }
      char *cp = strstr(g_buf, "nim_libp2p_peers");
      if (cp) { char *nl = strchr(cp, '\n'); printf("  %.*s\n", nl?(int)(nl-cp):20, cp); }
    }
    sleep(10);
  }
  return 0;
}
