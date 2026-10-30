// SPDX-License-Identifier: MIT
#include <jni.h>
#include <lsplant.hpp>
#include <dobby.h>
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <elf.h>
#include "hooker_dex.h"
#define LOG_TAG "abouthook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
extern "C" JNIEXPORT jstring JNICALL
Java_abouthook_Hook_nativeGetProp(JNIEnv* env, jclass, jstring jkey, jstring jdef) {
    if (jkey == nullptr) return jdef ? jdef : env->NewStringUTF("");
    const char* key = env->GetStringUTFChars(jkey, nullptr);
    if (key == nullptr) return jdef ? jdef : env->NewStringUTF("");
    char value[PROP_VALUE_MAX];
    value[0] = '\0';
    int len = __system_property_get(key, value);
    env->ReleaseStringUTFChars(jkey, key);
    if (len <= 0) return jdef ? jdef : env->NewStringUTF("");
    return env->NewStringUTF(value);
}
class ArtSymbols {
public:
    void ensure() {
        if (loaded_) return;
        loaded_ = true;
        dl_iterate_phdr(phdr_cb, this);
        if (base_ == 0 || path_[0] == '\0') {
            LOGE("libart.so not found in maps");
            return;
        }
        LOGI("libart base %p path %s", (void*)base_, path_);
        int fd = open(path_, O_RDONLY | O_CLOEXEC);
        if (fd < 0) { LOGE("open %s failed", path_); return; }
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return; }
        buf_.resize((size_t)st.st_size);
        ssize_t rd = read(fd, buf_.data(), buf_.size());
        close(fd);
        if (rd != (ssize_t)buf_.size()) { LOGE("short read of %s", path_); return; }
        parse();
        LOGI("libart symbols: %zu", list_.size());
    }
    void* exact(std::string_view name) {
        ensure();
        auto it = exact_.find(name);
        if (it == exact_.end()) return nullptr;
        return (void*)(base_ + it->second);
    }
    void* prefix(std::string_view name) {
        ensure();
        for (const auto& kv : list_) {
            if (kv.first.size() >= name.size() &&
                memcmp(kv.first.data(), name.data(), name.size()) == 0) {
                return (void*)(base_ + kv.second);
            }
        }
        return nullptr;
    }
private:
    static int phdr_cb(struct dl_phdr_info* info, size_t, void* arg) {
        ArtSymbols* self = (ArtSymbols*)arg;
        const char* n = info->dlpi_name;
        if (n != nullptr && strstr(n, "libart.so") != nullptr) {
            self->base_ = (uintptr_t)info->dlpi_addr;
            snprintf(self->path_, sizeof(self->path_), "%s", n);
            return 1;
        }
        return 0;
    }
    void parse() {
        const uint8_t* d = (const uint8_t*)buf_.data();
        if (buf_.size() < sizeof(Elf64_Ehdr)) return;
        const Elf64_Ehdr* eh = (const Elf64_Ehdr*)d;
        if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
            eh->e_ident[EI_CLASS] != ELFCLASS64) return;
        if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf64_Shdr)) return;
        const Elf64_Shdr* sh = (const Elf64_Shdr*)(d + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum; i++) {
            if (sh[i].sh_type != SHT_SYMTAB && sh[i].sh_type != SHT_DYNSYM) continue;
            if (sh[i].sh_link >= eh->e_shnum) continue;
            if (sh[i].sh_offset + sh[i].sh_size > buf_.size()) continue;
            const char* strtab = (const char*)(d + sh[sh[i].sh_link].sh_offset);
            size_t count = sh[i].sh_size / sizeof(Elf64_Sym);
            const Elf64_Sym* syms = (const Elf64_Sym*)(d + sh[i].sh_offset);
            for (size_t j = 0; j < count; j++) {
                if (syms[j].st_name == 0 || syms[j].st_value == 0) continue;
                std::string_view name(strtab + syms[j].st_name);
                if (exact_.find(name) != exact_.end()) continue;
                exact_.emplace(name, syms[j].st_value);
                list_.emplace_back(name, syms[j].st_value);
            }
        }
    }
    bool loaded_ = false;
    uintptr_t base_ = 0;
    char path_[512] = {0};
    std::vector<char> buf_;
    std::unordered_map<std::string_view, uint64_t> exact_;
    std::vector<std::pair<std::string_view, uint64_t>> list_;
};
static ArtSymbols g_art;
static jobject g_hookIqoo = nullptr;
static jobject g_hookSeries = nullptr;
static jobject g_hookPublicity = nullptr;
static jobject g_hookPropGet1 = nullptr;
static jobject g_hookPropGet2 = nullptr;
static jobject g_hookMarketName = nullptr;
static jobject make_hooker(JNIEnv* env, jclass hookClass, jmethodID ctorId, const char* kind) {
    jobject hooker = env->NewObject(hookClass, ctorId);
    if (hooker == nullptr) return nullptr;
    jfieldID kindField = env->GetFieldID(hookClass, "kind", "Ljava/lang/String;");
    if (kindField != nullptr) {
        env->SetObjectField(hooker, kindField, env->NewStringUTF(kind));
    }
    return env->NewGlobalRef(hooker);
}
static bool load_hooker_class(JNIEnv* env, jobject parentLoader, jclass* hookClassOut, jmethodID* ctorOut, jobject* callbackOut) {
    jclass classLoaderCls = env->FindClass("java/lang/ClassLoader");
    jclass byteBufferCls = env->FindClass("java/nio/ByteBuffer");
    jmethodID wrap = env->GetStaticMethodID(byteBufferCls, "wrap", "([B)Ljava/nio/ByteBuffer;");
    jbyteArray dexArray = env->NewByteArray((jsize)hooker_dex_len);
    env->SetByteArrayRegion(dexArray, 0, (jsize)hooker_dex_len, (const jbyte*)hooker_dex);
    jobject buffer = env->CallStaticObjectMethod(byteBufferCls, wrap, dexArray);
    jclass loaderCls = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    jmethodID loaderCtor = env->GetMethodID(loaderCls, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    jobject loader = env->NewObject(loaderCls, loaderCtor, buffer, parentLoader);
    if (loader == nullptr) { LOGE("InMemoryDexClassLoader failed"); return false; }
    jmethodID loadClass = env->GetMethodID(classLoaderCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env->NewStringUTF("abouthook.Hook");
    jclass hookClass = (jclass)env->CallObjectMethod(loader, loadClass, name);
    if (hookClass == nullptr) { LOGE("abouthook.Hook not found"); return false; }
    jmethodID ctor = env->GetMethodID(hookClass, "<init>", "()V");
    jmethodID callbackId = env->GetMethodID(hookClass, "callback", "([Ljava/lang/Object;)Ljava/lang/Object;");
    if (ctor == nullptr || callbackId == nullptr) {
        LOGE("hooker ctor/callback missing");
        return false;
    }
    jobject callback = env->ToReflectedMethod(hookClass, callbackId, JNI_FALSE);
    if (callback == nullptr) { LOGE("callback ToReflectedMethod failed"); return false; }
    static const JNINativeMethod natives[] = {
        { "nativeGetProp", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
          (void*)Java_abouthook_Hook_nativeGetProp },
    };
    if (env->RegisterNatives(hookClass, natives, 1) != JNI_OK) {
        LOGE("RegisterNatives(nativeGetProp) failed");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    *hookClassOut = (jclass)env->NewGlobalRef(hookClass);
    *ctorOut = ctor;
    *callbackOut = env->NewGlobalRef(callback);
    return true;
}
static jobject get_app_classloader(JNIEnv* env) {
    jclass atClass = env->FindClass("android/app/ActivityThread");
    if (atClass == nullptr) return nullptr;
    jmethodID currentAT = env->GetStaticMethodID(atClass, "currentActivityThread", "()Landroid/app/ActivityThread;");
    if (currentAT == nullptr) return nullptr;
    jobject at = env->CallStaticObjectMethod(atClass, currentAT);
    if (at == nullptr) return nullptr;
    jmethodID getApp = env->GetMethodID(atClass, "getApplication", "()Landroid/app/Application;");
    if (getApp == nullptr) return nullptr;
    jobject app = env->CallObjectMethod(at, getApp);
    if (app == nullptr) return nullptr;
    jclass ctxClass = env->FindClass("android/content/Context");
    jmethodID getCL = env->GetMethodID(ctxClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
    if (getCL == nullptr) return nullptr;
    jobject cl = env->CallObjectMethod(app, getCL);
    return cl ? env->NewGlobalRef(cl) : nullptr;
}
static jclass load_app_class(JNIEnv* env, jobject loader, const char* name) {
    jclass loaderCls = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClass = env->GetMethodID(loaderCls, "loadClass",
                                           "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring jname = env->NewStringUTF(name);
    jclass cls = (jclass)env->CallObjectMethod(loader, loadClass, jname);
    return cls;
}
static bool hook_method(JNIEnv* env, jclass hookClass, jmethodID ctorId, jobject callbackMethod,
                        jobject targetMember, const char* kind, jobject* hookerOut) {
    if (targetMember == nullptr) { LOGE("null target member for %s", kind); return false; }
    jobject hooker = make_hooker(env, hookClass, ctorId, kind);
    if (hooker == nullptr) { LOGE("hooker alloc %s failed", kind); return false; }
    jobject backup = lsplant::Hook(env, targetMember, hooker, callbackMethod);
    if (backup == nullptr) { LOGE("hook failed %s", kind); return false; }
    jfieldID backupField = env->GetFieldID(hookClass, "backup", "Ljava/lang/Object;");
    if (backupField != nullptr) env->SetObjectField(hooker, backupField, backup);
    *hookerOut = hooker;
    LOGI("hooked %s (backup=%p)", kind, backup);
    return true;
}
static bool hook_static_method(JNIEnv* env, jclass hookClass, jmethodID ctorId,
                               jobject callbackMethod, jclass cls, const char* name,
                               const char* sig, const char* kind, jobject* hookerOut) {
    jmethodID mid = env->GetStaticMethodID(cls, name, sig);
    if (mid == nullptr) { LOGE("static %s%s not found", name, sig); return false; }
    jobject reflected = env->ToReflectedMethod(cls, mid, JNI_TRUE);
    if (reflected == nullptr) { LOGE("reflect %s failed", name); return false; }
    return hook_method(env, hookClass, ctorId, callbackMethod, reflected, kind, hookerOut);
}
static bool hook_declared_method(JNIEnv* env, jclass hookClass, jmethodID ctorId,
                                 jobject callbackMethod, jclass cls, const char* name,
                                 const char* sig, const char* kind, jobject* hookerOut) {
    jmethodID mid = env->GetMethodID(cls, name, sig);
    if (mid == nullptr) { LOGE("method %s%s not found", name, sig); return false; }
    jobject reflected = env->ToReflectedMethod(cls, mid, JNI_FALSE);
    if (reflected == nullptr) { LOGE("reflect %s failed", name); return false; }
    jclass methodCls = env->FindClass("java/lang/reflect/Method");
    jmethodID setAccessible = env->GetMethodID(methodCls, "setAccessible", "(Z)V");
    if (setAccessible != nullptr) env->CallVoidMethod(reflected, setAccessible, JNI_TRUE);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return hook_method(env, hookClass, ctorId, callbackMethod, reflected, kind, hookerOut);
}
static void install_prop_hooks(JNIEnv* env, jclass hookClass, jmethodID ctorId, jobject callbackMethod) {
    jclass sp = env->FindClass("android/os/SystemProperties");
    if (sp == nullptr) {
        LOGE("android.os.SystemProperties not found");
        env->ExceptionClear();
        return;
    }
    hook_static_method(env, hookClass, ctorId, callbackMethod, sp, "get",
                       "(Ljava/lang/String;)Ljava/lang/String;", "prop", &g_hookPropGet1);
    hook_static_method(env, hookClass, ctorId, callbackMethod, sp, "get",
                       "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", "prop",
                       &g_hookPropGet2);
    LOGI("prop hooks: get1=%p get2=%p", g_hookPropGet1, g_hookPropGet2);
}
static bool install_hooks(JNIEnv* env, jclass hookClass, jmethodID ctorId,
                          jobject callbackMethod) {
    jobject appCL = get_app_classloader(env);
    if (appCL == nullptr) return false;
    jclass deviceUtils = load_app_class(env, appCL, "com.vivo.settings.utils.DeviceUtils");
    jclass controller = load_app_class(
            env, appCL,
            "com.vivo.settings.aboutphone.deviceinfo.VivoDeviceInfoProductModelController");
    if (deviceUtils == nullptr || controller == nullptr) return false;
    jmethodID iqooMid = env->GetStaticMethodID(deviceUtils, "isIqooDevices", "()Z");
    if (iqooMid != nullptr) {
        jobject reflected = env->ToReflectedMethod(deviceUtils, iqooMid, JNI_TRUE);
        hook_method(env, hookClass, ctorId, callbackMethod, reflected, "iqoo", &g_hookIqoo);
    } else {
        LOGE("isIqooDevices not found");
    }
    jmethodID seriesMid = env->GetStaticMethodID(deviceUtils, "getVivoSeries", "()Ljava/lang/String;");
    if (seriesMid != nullptr) {
        jobject reflected = env->ToReflectedMethod(deviceUtils, seriesMid, JNI_TRUE);
        hook_method(env, hookClass, ctorId, callbackMethod, reflected, "series", &g_hookSeries);
    } else {
        LOGE("getVivoSeries not found");
    }
    jclass classCls = env->FindClass("java/lang/Class");
    jmethodID getDeclaredMethod = env->GetMethodID(
            classCls, "getDeclaredMethod",
            "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
    jclass stringCls = env->FindClass("java/lang/String");
    jobjectArray params = env->NewObjectArray(1, classCls, stringCls);
    jstring mname = env->NewStringUTF("getPublicityName");
    jobject pubMethod = env->CallObjectMethod(controller, getDeclaredMethod, mname, params);
    if (pubMethod != nullptr) {
        jclass methodCls = env->FindClass("java/lang/reflect/Method");
        jmethodID setAccessible = env->GetMethodID(methodCls, "setAccessible", "(Z)V");
        env->CallVoidMethod(pubMethod, setAccessible, JNI_TRUE);
        hook_method(env, hookClass, ctorId, callbackMethod, pubMethod, "publicity",
                    &g_hookPublicity);
    } else {
        LOGE("getPublicityName not found");
    }
    jclass deviceNameImpl = load_app_class(
            env, appCL, "com.vivo.settings.aboutphone.deviceinfo.VivoDeviceNameImpl");
    if (deviceNameImpl != nullptr) {
        hook_declared_method(env, hookClass, ctorId, callbackMethod, deviceNameImpl,
                             "getDeviceReleaseName", "()Ljava/lang/String;",
                             "marketname", &g_hookMarketName);
    } else {
        LOGE("VivoDeviceNameImpl not found");
    }
    return true;
}
static void* worker(void*) {
    typedef jint (*GetVMsFn)(JavaVM**, jsize, jsize*);
    GetVMsFn getVMs = (GetVMsFn)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
    if (getVMs == nullptr) {
        const char* candidates[] = {
            "/apex/com.android.art/lib64/libnativehelper.so",
            "/apex/com.android.art/lib64/libart.so",
            "libnativehelper.so",
            "libart.so",
        };
        for (const char* candidate : candidates) {
            void* handle = dlopen(candidate, RTLD_NOW);
            if (handle == nullptr) continue;
            getVMs = (GetVMsFn)dlsym(handle, "JNI_GetCreatedJavaVMs");
            if (getVMs != nullptr) { LOGI("JavaVM getter from %s", candidate); break; }
        }
    }
    if (getVMs == nullptr) { LOGE("JNI_GetCreatedJavaVMs not found"); return nullptr; }
    JavaVM* vm = nullptr;
    jsize count = 0;
    for (int i = 0; i < 300; i++) {
        if (getVMs(&vm, 1, &count) == JNI_OK && count > 0 && vm != nullptr) break;
        usleep(200000);
    }
    if (vm == nullptr) { LOGE("no JavaVM"); return nullptr; }
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("AttachCurrentThread failed");
            return nullptr;
        }
    }

    lsplant::InitInfo info;
    info.inline_hooker = [](void* target, void* hooker) -> void* {
        void* backup = nullptr;
        if (DobbyHook(target, (dobby_dummy_func_t)hooker,
                      (dobby_dummy_func_t*)&backup) != 0) {
            LOGE("DobbyHook failed on %p", target);
            return nullptr;
        }
        return backup;
    };
    info.inline_unhooker = [](void* func) -> bool {
        return DobbyDestroy(func) == 0;
    };
    info.art_symbol_resolver = [](std::string_view symbol) -> void* {
        return g_art.exact(symbol);
    };
    info.art_symbol_prefix_resolver = [](std::string_view prefix) -> void* {
        return g_art.prefix(prefix);
    };
    if (!lsplant::Init(env, info)) {
        LOGE("lsplant init failed");
        return nullptr;
    }
    LOGI("lsplant init ok");
    jclass classLoaderCls = env->FindClass("java/lang/ClassLoader");
    jmethodID getSystem = env->GetStaticMethodID(classLoaderCls, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    jobject parent = env->CallStaticObjectMethod(classLoaderCls, getSystem);
    jclass hookClass = nullptr;
    jmethodID hookCtor = nullptr;
    jobject callbackMethod = nullptr;
    if (!load_hooker_class(env, parent, &hookClass, &hookCtor, &callbackMethod)) {
        return nullptr;
    }
    install_prop_hooks(env, hookClass, hookCtor, callbackMethod);
    for (int attempt = 0; attempt < 120; attempt++) {
        if (env->PushLocalFrame(64) == 0) {
            bool ok = install_hooks(env, hookClass, hookCtor, callbackMethod);
            env->PopLocalFrame(nullptr);
            if (ok && g_hookIqoo != nullptr) {
                LOGI("install done: iqoo=%p series=%p publicity=%p marketname=%p",
                     g_hookIqoo, g_hookSeries, g_hookPublicity, g_hookMarketName);
                return nullptr;
            }
        }
        usleep(500000);
    }
    LOGE("gave up waiting for Settings classes");
    return nullptr;
}
static pthread_once_t g_start_once = PTHREAD_ONCE_INIT;
static void start_once(void) {
    pthread_t thread;
    if (pthread_create(&thread, nullptr, worker, nullptr) != 0) {
        LOGE("pthread_create failed");
    }
}
extern "C" __attribute__((visibility("default"))) void abouthook_start(void) {
    LOGI("abouthook_start (pid %d)", getpid());
    pthread_once(&g_start_once, start_once);
}
__attribute__((constructor)) static void abouthook_init(void) {
    LOGI("libabouthook loaded in pid %d", getpid());
    abouthook_start();
}
