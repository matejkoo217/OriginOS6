/* SPDX-License-Identifier: MIT */
/*
 * wm_client.c - read WindowManager keyguard getters straight from libbinder.
 *   isKeyguardLocked()                 = android.view.IWindowManager tx  32
 *   isKeyguardShowingAndNotOccluded()  = android.view.IWindowManager tx 104
*/

#include "wm_client.h"
#include <stdint.h>
#include <stddef.h>

extern void *dlopen(const char *name, int flags);
extern void *dlsym(void *handle, const char *name);

typedef int32_t  wm_s32;
typedef uint32_t wm_u32;
typedef uint64_t wm_u64;

#define LIBBINDER_NDK_SYM_CHECK_SERVICE "AServiceManager_checkService"
#define LIBBINDER_NDK_SYM_TO_PLATFORM   "_Z25AIBinder_toPlatformBinderP8AIBinder"
#define LIBBINDER_SYM_PARCEL_CTOR       "_ZN7android6ParcelC1Ev"
#define LIBBINDER_SYM_PARCEL_DTOR       "_ZN7android6ParcelD1Ev"
#define LIBBINDER_SYM_WRITE_TOKEN       "_ZN7android6Parcel19writeInterfaceTokenEPKDsm"
#define LIBBINDER_SYM_READ_INT32        "_ZNK7android6Parcel9readInt32EPi"
#define LIBBINDER_SYM_BPBINDER_TRANSACT "_ZN7android8BpBinder8transactEjRKNS_6ParcelEPS1_j"
#define TX_IS_KEYGUARD_LOCKED        32u
#define TX_IS_KEYGUARD_SHOWING_NOT_OCCLUDED 104u
#define WM_DESCRIPTOR   "android.view.IWindowManager"
#define WM_SERVICE_NAME "window"
#define WM_PARCEL_BYTES 512
#define RTLD_NOW_ 2

typedef void  *(*wm_fn_check_service)(const char *name);
typedef void   (*wm_fn_parcel_ctor)(void *parcel);
typedef void   (*wm_fn_parcel_dtor)(void *parcel);
typedef wm_s32 (*wm_fn_write_token)(void *parcel, const uint16_t *str, size_t len);
typedef wm_s32 (*wm_fn_read_int32)(const void *parcel, wm_s32 *out);
typedef wm_s32 (*wm_fn_transact)(void *binder, wm_u32 code, const void *data, void *reply, wm_u32 flags);

static struct {
    int    state;
    void  *ndk_handle;
    void  *binder_handle;
    wm_fn_check_service  check_service;
    wm_fn_parcel_ctor    parcel_ctor;
    wm_fn_parcel_dtor    parcel_dtor;
    wm_fn_write_token    write_token;
    wm_fn_read_int32     read_int32;
    wm_fn_transact       transact;
    void                *transact_raw;
    void                *to_platform;
    void  *binder;
    wm_u64 req[WM_PARCEL_BYTES / 8] __attribute__((aligned(16)));
    wm_u64 rep[WM_PARCEL_BYTES / 8] __attribute__((aligned(16)));
    int    req_built;
} g_wm;
static void *wm_sp_from_aibinder(void *fn, void *aibinder)
{
    static void *slot[2] __attribute__((aligned(16)));
    slot[0] = 0;
    slot[1] = 0;
    __asm__ volatile(
        "mov x8, %[out]\n"
        "mov x0, %[in]\n"
        "blr %[fn]\n"
        :
        : [out] "r"(slot), [in] "r"(aibinder), [fn] "r"(fn)
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
          "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "memory");
    return slot[0];
}
static void *wm_open_first(const char *const *paths)
{
    for (int i = 0; paths[i]; i++) {
        void *h = dlopen(paths[i], RTLD_NOW_);
        if (h) return h;
    }
    return 0;
}
static size_t wm_utf16(uint16_t *dst, const char *src)
{
    size_t n = 0;
    while (src[n]) { dst[n] = (uint16_t)(unsigned char)src[n]; n++; }
    dst[n] = 0;
    return n;
}

static void wm_drop(void)
{
    if (g_wm.req_built && g_wm.parcel_dtor) g_wm.parcel_dtor(g_wm.req);
    g_wm.binder = 0;
    g_wm.req_built = 0;
    g_wm.state = 0;
}

static int wm_init(void)
{
    static const char *const ndk_paths[] = {
        "/system/lib64/libbinder_ndk.so",
        "libbinder_ndk.so",
        "/vendor/lib64/libbinder_ndk.so",
        "/odm/lib64/libbinder_ndk.so",
        0
    };
    static const char *const binder_paths[] = {
        "/system/lib64/libbinder.so",
        "libbinder.so",
        "/vendor/lib64/libbinder.so",
        "/odm/lib64/libbinder.so",
        0
    };
    if (g_wm.state != 0) return g_wm.state == 1 ? 0 : -1;
    if (!g_wm.ndk_handle) {
        g_wm.ndk_handle = wm_open_first(ndk_paths);
        if (!g_wm.ndk_handle) { g_wm.state = -1; return -1; }
        g_wm.binder_handle = wm_open_first(binder_paths);
#define WM_RESOLVE(field, name)                                                   \
        do {                                                                      \
            *(void **)(&g_wm.field) = dlsym(g_wm.ndk_handle, name);                \
            if (!g_wm.field && g_wm.binder_handle)                                 \
                *(void **)(&g_wm.field) = dlsym(g_wm.binder_handle, name);         \
        } while (0)
        WM_RESOLVE(check_service, LIBBINDER_NDK_SYM_CHECK_SERVICE);
        g_wm.to_platform = dlsym(g_wm.ndk_handle, LIBBINDER_NDK_SYM_TO_PLATFORM);
        WM_RESOLVE(parcel_ctor, LIBBINDER_SYM_PARCEL_CTOR);
        WM_RESOLVE(parcel_dtor, LIBBINDER_SYM_PARCEL_DTOR);
        WM_RESOLVE(write_token, LIBBINDER_SYM_WRITE_TOKEN);
        WM_RESOLVE(read_int32, LIBBINDER_SYM_READ_INT32);
        WM_RESOLVE(transact_raw, LIBBINDER_SYM_BPBINDER_TRANSACT);
#undef WM_RESOLVE
        if (!g_wm.check_service || !g_wm.to_platform || !g_wm.parcel_ctor ||
            !g_wm.parcel_dtor || !g_wm.write_token || !g_wm.read_int32 ||
            !g_wm.transact_raw) {
            g_wm.state = -1;
            return -1;
        }
    }
    void *aibinder = (void *)g_wm.check_service(WM_SERVICE_NAME);
    if (!aibinder) { g_wm.state = -1; return -1; }
    void *binder = wm_sp_from_aibinder(g_wm.to_platform, aibinder);
    if (!binder) { g_wm.state = -1; return -1; }
    void **vt = *(void ***)binder;
    if (!vt) { g_wm.state = -1; return -1; }
    void *fn = 0;
    for (int i = 0; i < 16; i++) {
        if (vt[i] == g_wm.transact_raw) { fn = vt[i]; break; }
    }
    if (!fn) { g_wm.state = -1; return -1; }
    g_wm.transact = (wm_fn_transact)fn;
    static uint16_t desc[128];
    size_t dlen = wm_utf16(desc, WM_DESCRIPTOR);
    g_wm.parcel_ctor(g_wm.req);
    g_wm.write_token(g_wm.req, desc, dlen);
    g_wm.req_built = 1;
    g_wm.binder = binder;
    g_wm.state = 1;
    return 0;
}
static int wm_call(wm_u32 code, wm_s32 *out)
{
    if (!g_wm.transact || !g_wm.binder) return -1;
    g_wm.parcel_ctor(g_wm.rep);
    wm_s32 rc = g_wm.transact(g_wm.binder, code, g_wm.req, g_wm.rep, 0);
    wm_s32 exc = -1;
    wm_s32 val = -1;
    int ok = 0;
    if (rc == 0 &&
        g_wm.read_int32(g_wm.rep, &exc) == 0 && exc == 0 &&
        g_wm.read_int32(g_wm.rep, &val) == 0) {
        ok = 1;
    }
    g_wm.parcel_dtor(g_wm.rep);
    if (!ok) return -1;
    *out = val;
    return 0;
}
int wm_keyguard_state(int *locked, int *showing_and_not_occluded)
{
    wm_s32 v_locked = -1;
    wm_s32 v_not_occluded = -1;
    if (g_wm.state != 1 && wm_init() != 0) return -1;
    if (wm_call(TX_IS_KEYGUARD_LOCKED, &v_locked) != 0 ||
        wm_call(TX_IS_KEYGUARD_SHOWING_NOT_OCCLUDED, &v_not_occluded) != 0) {
        wm_drop();
        return -1;
    }
    if ((v_locked & ~1) || (v_not_occluded & ~1)) {
        wm_drop();
        return -1;
    }
    if (locked) *locked = (int)v_locked;
    if (showing_and_not_occluded) *showing_and_not_occluded = (int)v_not_occluded;
    return 0;
}
void wm_keyguard_state_reset(void)
{
    wm_drop();
}
