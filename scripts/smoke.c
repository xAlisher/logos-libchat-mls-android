// M0' on-device smoke driver for liblogoschat (arm64).
//
// dlopen the wrapper .so, then:
//   1. logoschat_gen_address()  — network-free; MUST print a 64-hex address.
//   2. (if argv[1]=="open") logoschat_open() + get_address + gen twice, to test
//      the full delivery-node stack and address stability.
//
// Build with the NDK clang for aarch64; run from /data/local/tmp with the
// delivery .so's beside it (LD_LIBRARY_PATH=.).
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef char* (*gen_fn)(void);
typedef void  (*free_fn)(char*);
typedef void* (*open_fn)(const char*, const char*, const char*);
typedef char* (*addr_fn)(void*);
typedef const char* (*err_fn)(void);
typedef void  (*shutdown_fn)(void*);

static int is_hex64(const char* s) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (n != 64) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return 0;
    }
    return 1;
}

int main(int argc, char** argv) {
    const char* path = (argc > 2) ? argv[2] : "./liblogoschat.so";
    void* h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { printf("SMOKE FAIL: dlopen %s: %s\n", path, dlerror()); return 1; }

    gen_fn gen   = (gen_fn) dlsym(h, "logoschat_gen_address");
    free_fn xfree= (free_fn)dlsym(h, "logoschat_free_string");
    if (!gen || !xfree) { printf("SMOKE FAIL: missing symbols: %s\n", dlerror()); return 1; }

    // 1. network-free address mint (the core M0' assertion)
    char* a1 = gen();
    printf("gen_address #1: %s\n", a1 ? a1 : "(null)");
    if (!is_hex64(a1)) { printf("SMOKE FAIL: not a 64-hex address\n"); return 1; }
    char* a2 = gen();
    printf("gen_address #2: %s\n", a2 ? a2 : "(null)");
    printf("ephemeral (fresh each mint) = %s\n",
           (a1 && a2 && strcmp(a1,a2)!=0) ? "yes (expected for gen_address)" : "no");
    xfree(a1); xfree(a2);
    printf("SMOKE PASS: gen_address prints a 64-hex account address on arm64\n");

    // 2. optional full-stack open (needs network + delivery node)
    if (argc > 1 && strcmp(argv[1], "open") == 0) {
        open_fn xopen = (open_fn)dlsym(h, "logoschat_open");
        addr_fn xaddr = (addr_fn)dlsym(h, "logoschat_get_address");
        err_fn  xerr  = (err_fn) dlsym(h, "logoschat_last_error");
        shutdown_fn xsh = (shutdown_fn)dlsym(h, "logoschat_shutdown");
        printf("--- full open() attempt ---\n");
        void* c = xopen("/data/local/tmp/logoschat-smoke/eph.db", "smokekey0123456789", NULL);
        if (!c) { printf("open() returned null: %s\n", xerr ? xerr() : "?"); return 0; }
        char* addr = xaddr(c);
        printf("open() address: %s  hex64=%d\n", addr?addr:"(null)", is_hex64(addr));
        if (addr) xfree(addr);
        if (xsh) xsh(c);
    }

    // 3. persistent identity: open_persistent twice; address MUST be stable.
    if (argc > 1 && strcmp(argv[1], "persist") == 0) {
        typedef void* (*openp_fn)(const char*, const char*, const char*, const char*);
        openp_fn xopenp = (openp_fn)dlsym(h, "logoschat_open_persistent");
        addr_fn xaddr = (addr_fn)dlsym(h, "logoschat_get_address");
        err_fn  xerr  = (err_fn) dlsym(h, "logoschat_last_error");
        shutdown_fn xsh = (shutdown_fn)dlsym(h, "logoschat_shutdown");
        if (!xopenp) { printf("PERSIST FAIL: no logoschat_open_persistent symbol\n"); return 1; }
        const char* idp = (argc > 2) ? argv[2] : "/data/local/tmp/logoschat-smoke/identity.bin";
        char first[128] = {0};
        for (int run = 1; run <= 2; run++) {
            printf("--- persistent open run #%d ---\n", run);
            void* c = xopenp("/data/local/tmp/logoschat-smoke/persist.db",
                             "smokekey0123456789", NULL, idp);
            if (!c) { printf("PERSIST FAIL: open_persistent null: %s\n", xerr?xerr():"?"); return 1; }
            char* addr = xaddr(c);
            printf("persistent address #%d: %s  hex64=%d\n", run, addr?addr:"(null)", is_hex64(addr));
            if (run == 1 && addr) strncpy(first, addr, sizeof(first)-1);
            else if (run == 2 && addr) {
                printf("address stable across restarts = %s\n",
                       strcmp(first, addr)==0 ? "YES" : "NO");
                if (strcmp(first, addr)==0)
                    printf("PERSIST PASS: same address across two open_persistent runs\n");
                else printf("PERSIST FAIL: address rotated\n");
            }
            if (addr) xfree(addr);
            if (xsh) xsh(c);
        }
    }
    return 0;
}
