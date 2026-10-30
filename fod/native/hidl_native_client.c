// SPDX-License-Identifier: MIT
#include <dlfcn.h>
#include <stdint.h>
#include <stddef.h>
extern int g_debug;
void xwrite(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    __asm__ volatile(
        "mov x0, #1\n"
        "mov x1, %0\n"
        "mov x2, %1\n"
        "mov x8, #64\n"
        "svc #0\n"
        :: "r"(s), "r"(n) : "x0", "x1", "x2", "x8", "memory");
}
struct __ss_string {
    unsigned char flag_size;
    char data[23];
};
#define SYM_GETSERVICE \
    "_ZN6vendor6xiaomi8hardware20fingerprintextension4V1_018IXiaomiFingerprint10getServiceERKNSt3__112basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb"
#define SYM_EXTCMD \
    "_ZN6vendor6xiaomi8hardware20fingerprintextension4V1_021BpHwXiaomiFingerprint6extCmdEjj"
static const char *so_paths[] = {
    "/vendor/lib64/vendor.xiaomi.hardware.fingerprintextension@1.0.so",
    "/odm/lib64/vendor.xiaomi.hardware.fingerprintextension@1.0.so",
    "/system/lib64/vendor.xiaomi.hardware.fingerprintextension@1.0.so",
    NULL,
};
static void *g_bp = NULL;
static void *g_extcmd_fn = NULL;
static void *call_get_service(void *fn, const void *str, int retry) {
    static uint64_t slot __attribute__((aligned(16)));
    __asm__ volatile(
        "mov x8, %[out]\n"
        "mov x0, %[str]\n"
        "mov x1, %[retry]\n"
        "blr %[fn]\n"
        :
        : [out] "r"(&slot), [str] "r"(str), [retry] "r"((long)retry), [fn] "r"(fn)
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8",
          "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
          "x18", "memory");
    return (void *)slot;
}
static void call_extcmd(void *fn, void *bp, uint32_t cmd, uint32_t param) {
    static _Alignas(16) unsigned char ret_buf[0x60];
    for (int i = 0; i < (int)sizeof(ret_buf); i++) ret_buf[i] = 0;
    __asm__ volatile(
        "mov x8, %[retbuf]\n"
        "mov x0, %[bp]\n"
        "mov x1, %[cmd]\n"
        "mov x2, %[param]\n"
        "blr %[fn]\n"
        :
        : [retbuf] "r"(ret_buf), [bp] "r"(bp),
          [cmd] "r"((long)cmd), [param] "r"((long)param), [fn] "r"(fn)
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8",
          "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
          "x18", "memory");
}
static void make_ss_string(struct __ss_string *out, const char *name) {
    int len = 0;
    while (name[len]) len++;
    out->flag_size = (unsigned char)((len << 1) & 0xfe); /* is_long = 0 */
    for (int i = 0; i < 23; i++) out->data[i] = (i < len) ? name[i] : 0;
}
int native_hidl_init(void) {
    void *h = NULL;
    for (int i = 0; so_paths[i]; i++) {
        h = dlopen(so_paths[i], RTLD_NOW | RTLD_LOCAL);
        if (h) { if (g_debug) { xwrite("[N] dlopen: "); xwrite(so_paths[i]); xwrite("\n"); } break; }
    }
    if (!h) {
        xwrite("[N] FAILED dlopen fingerprintextension lib\n");
        return -1;
    }
    void *getService = dlsym(h, SYM_GETSERVICE);
    if (!getService) {
        xwrite("[N] FAILED dlsym getService\n");
        return -1;
    }
    struct __ss_string inst;
    make_ss_string(&inst, "default");
    g_bp = call_get_service(getService, &inst, 0);
    if (!g_bp) {
        xwrite("[N] getService returned NULL sp\n");
        return -1;
    }
    g_extcmd_fn = dlsym(h, SYM_EXTCMD);
    if (!g_extcmd_fn) {
        xwrite("[N] FAILED dlsym extCmd\n");
        return -1;
    }
    if (g_debug)
        xwrite("[N] OK service resolved, extCmd ready\n");
    return 0;
}
int native_extcmd(uint32_t cmd, uint32_t param) {
    if (!g_extcmd_fn || !g_bp) return -1;
    call_extcmd(g_extcmd_fn, g_bp, cmd, param);
    return 0;
}
