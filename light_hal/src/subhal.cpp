// Standalone light sub-HAL for the AOSP multi-HAL 2.1 (HIDL sub-HAL) ABI.
// For MIUI 14 vendor for Xiaomi 13, OriginOS6.
#include <android/log.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "hidl_abi.h"
#include "light_engine.h"

using android::hardware::Return;
using android::hardware::Status;
using android::hardware::Void;
using android::hardware::sensors::V1_0::Event;
using android::hardware::sensors::V1_0::OperationMode;
using android::hardware::sensors::V1_0::Result;
using android::hardware::sensors::V2_1::SensorInfo;

#define LOG_TAG "sensors-light"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

namespace {
constexpr uint32_t kSubHalVersion = 0x02010000;
constexpr int32_t kLightHandle = 0x33;
constexpr int32_t kRawHandle = 0x83f;
constexpr int32_t kLightType = 5;
constexpr int32_t kRawType = 33171111;
constexpr int64_t kLightPostIntervalNs = 40LL * 1000000;

void* gHalProxyCallback = nullptr;
SscEngine gEngine;
std::mutex gStateLock;
bool gLightActive = false;
bool gRawActive = false;
int64_t gLastCalibratedNs = 0;
int64_t gLastLightPostNs = 0;

void incStrongCallback(void* cb) {
    if (cb == nullptr) return;
    void** vt = *(void***)cb;
    intptr_t vbase = (intptr_t)vt[-(intptr_t)light_hal::kVbaseOffsetSlot];
    if (vbase < -0x1000 || vbase > 0x1000) {
        LOGW("unexpected callback vbase offset %ld, callback may be short-lived",
             (long)vbase);
        return;
    }
    auto* refbase = reinterpret_cast<android::RefBase*>((char*)cb + vbase);
    refbase->incStrong(cb);
    LOGI("callback %p pinned (vbase=%ld)", cb, (long)vbase);
}
struct ScopedWakelock {
    void* vptr;
    void* refCounter;
    int64_t createdAtNs;
    bool locked;
    uint8_t pad[7];
    ~ScopedWakelock() {}
};
void postSensorEvent(int32_t handle, int32_t type, const float* values, int count, int64_t tsNs) {
    void* cb = gHalProxyCallback;
    if (cb == nullptr) return;
    void** vt = *(void***)cb;
    typedef ScopedWakelock (*CreateWakelockFn)(void*, bool);
    typedef void (*PostEventsFn)(void*, const void*, void*);
    CreateWakelockFn create = (CreateWakelockFn)vt[light_hal::kCallbackCreateScopedWakelock / 8];
    PostEventsFn post = (PostEventsFn)vt[light_hal::kCallbackPostEvents / 8];
    ScopedWakelock wakelock = create(cb, false);
    std::vector<Event> events(1);
    Event& e = events[0];
    memset(&e, 0, sizeof(e));
    e.timestamp = tsNs;
    e.sensorHandle = handle;
    e.sensorType = type;
    if (count > 16) count = 16;
    if (count < 0) count = 0;
    for (int i = 0; i < count; i++) e.u.data[i] = values[i];
    post(cb, &events, &wakelock);
}

void onEngineEvent(int32_t type, const float* values, int count, int64_t tsNs) {
    if (count <= 0) return;
    const int64_t now = tsNs;
    if (type == kLightType) {
        {
            std::lock_guard<std::mutex> lk(gStateLock);
            gLastCalibratedNs = now;
            if (!gLightActive || (now - gLastLightPostNs) < kLightPostIntervalNs) return;
            gLastLightPostNs = now;
        }
        postSensorEvent(kLightHandle, kLightType, values, 1, tsNs);
    } else if (type == kRawType) {
        {
            std::lock_guard<std::mutex> lk(gStateLock);
            if (!gRawActive) return;
        }
        postSensorEvent(kRawHandle, kRawType, values, count, tsNs);
    }
}
SensorInfo makeLightInfo() {
    SensorInfo si{};
    si.sensorHandle = kLightHandle;
    si.name = hidl_string("TCS3720ALSPRX Ambient Light Sensor Non-wakeup");
    si.vendor = hidl_string("xiaomi");
    si.version = 66048;
    si.type = kLightType;
    si.typeAsString = hidl_string("");
    si.maxRange = 40960.0f;
    si.resolution = 1.0f;
    si.power = 0.1f;
    si.minDelay = 0;
    si.fifoReservedEventCount = 0;
    si.fifoMaxEventCount = 10000;
    si.requiredPermission = hidl_string("");
    si.maxDelay = 0;
    si.flags = 0x02;
    return si;
}
SensorInfo makeRawInfo() {
    SensorInfo si{};
    si.sensorHandle = kRawHandle;
    si.name = hidl_string("TCS3720 ALSPRX Ambient Light Sensor Raw Data Non-wakeup");
    si.vendor = hidl_string("xiaomi");
    si.version = 66048;
    si.type = kRawType;
    si.typeAsString = hidl_string("xiaomi.sensor.ambientlight.raw");
    si.maxRange = 65535.0f;
    si.resolution = 1.0f;
    si.power = 0.1f;
    si.minDelay = 0;
    si.fifoReservedEventCount = 0;
    si.fifoMaxEventCount = 10000;
    si.requiredPermission = hidl_string("");
    si.maxDelay = 0;
    si.flags = 0x06;
    return si;
}

}  // namespace
extern "C" {

Return<void> subhal_debug(void* self, const hidl_handle* fd, const hidl_vec<hidl_string>* args) {
    return Void();
}
Return<Result> subhal_set_operation_mode(void* self, int32_t mode) {
    return Result::OK;
}
Return<Result> subhal_activate(void* self, int32_t handle, bool enable) {
    LOGI("activate handle=0x%x enable=%d", handle, (int)enable);
    if (handle == kLightHandle) {
        {
            std::lock_guard<std::mutex> lk(gStateLock);
            gLightActive = enable;
            gLastCalibratedNs = 0;
            gLastLightPostNs = 0;
        }
        gEngine.setSensorActive(kLightType, enable);
    } else if (handle == kRawHandle) {
        {
            std::lock_guard<std::mutex> lk(gStateLock);
            gRawActive = enable;
        }
        gEngine.setSensorActive(kRawType, enable);
    } else {
        return Result::BAD_VALUE;
    }
    return Result::OK;
}
Return<Result> subhal_batch(void* self, int32_t handle, int64_t periodNs, int64_t latencyNs) {
    return Result::OK;
}
Return<Result> subhal_flush(void* self, int32_t handle) {
    return Result::OK;
}
Return<void> subhal_get_sensors_list(void* self, const std::function<void(const hidl_vec<SensorInfo>&)>* cb) {
    std::vector<SensorInfo> list;
    list.push_back(makeLightInfo());
    list.push_back(makeRawInfo());
    hidl_vec<SensorInfo> vec(list);
    (*cb)(vec);
    return Void();
}
Return<void> subhal_register_direct_channel(void* self) { return Void(); }
Return<Result> subhal_unregister_direct_channel(void* self, int32_t handle) {
    return Result::INVALID_OPERATION;
}
Return<void> subhal_config_direct_report(void* self) { return Void(); }
Return<Result> subhal_inject_sensor_data(void* self) { return Result::INVALID_OPERATION; }
std::string subhal_get_name(void* self) { return std::string("sensors.light"); }
Return<Result> subhal_initialize(void* self, const void* callbackSp) {
    void* cb = (callbackSp != nullptr) ? *(void* const*)callbackSp : nullptr;
    LOGI("initialize callback=%p", cb);
    if (cb != nullptr) {
        incStrongCallback(cb);
        gHalProxyCallback = cb;
    }
    if (!gEngine.started()) {
        gEngine.setEventSink(onEngineEvent);
        gEngine.setErrorSink([](const std::string& what) {
            LOGW("engine error: %s", what.c_str());
            gEngine.rearm("ssc-error");
        });
        if (!gEngine.init()) {
            LOGE("engine init failed");
            return Result::NO_MEMORY;
        }
    } else {
        gEngine.rearm("framework-restart");
    }
    return Result::OK;
}
Return<Result> subhal_stub_result(void* self) { return Result::INVALID_OPERATION; }
Return<void> subhal_stub_void(void* self) { return Void(); }

struct SubHalVTable {
    void* typeinfo;
    void* offsetToTop;
    void* slots[28];
};
const SubHalVTable gVtable = {
        /* typeinfo */ nullptr,
        /* offsetToTop */ nullptr,
        {
                /*  0 interfaceChain            */ (void*)subhal_stub_void,
                /*  1 interfaceDescriptor       */ (void*)subhal_stub_void,
                /*  2 debug                     */ (void*)subhal_debug,
                /*  3 getHashChain              */ (void*)subhal_stub_void,
                /*  4 setHALInstrumentation     */ (void*)subhal_stub_void,
                /*  5 ping                      */ (void*)subhal_stub_void,
                /*  6 getDebugInfo              */ (void*)subhal_stub_void,
                /*  7 notifySyspropsChanged     */ (void*)subhal_stub_void,
                /*  8 linkToDeath               */ (void*)subhal_stub_void,
                /*  9 unlinkToDeath             */ (void*)subhal_stub_void,
                /* 10 (reserved IBase)          */ (void*)subhal_stub_void,
                /* 11 ~sub-HAL D1               */ (void*)subhal_stub_void,
                /* 12 ~sub-HAL D0               */ (void*)subhal_stub_void,
                /* 13 getSensorsList (1.0)      */ (void*)subhal_stub_void,
                /* 14 setOperationMode          */ (void*)subhal_set_operation_mode,
                /* 15 activate                  */ (void*)subhal_activate,
                /* 16 initialize (1.0)          */ (void*)subhal_stub_result,
                /* 17 batch                     */ (void*)subhal_batch,
                /* 18 flush                     */ (void*)subhal_flush,
                /* 19 injectSensorData (1.0)    */ (void*)subhal_stub_result,
                /* 20 registerDirectChannel     */ (void*)subhal_register_direct_channel,
                /* 21 unregisterDirectChannel   */ (void*)subhal_unregister_direct_channel,
                /* 22 configDirectReport        */ (void*)subhal_config_direct_report,
                /* 23 getSensorsList_2_1        */ (void*)subhal_get_sensors_list,
                /* 24 initialize_2_1            */ (void*)subhal_stub_result,
                /* 25 injectSensorData_2_1      */ (void*)subhal_inject_sensor_data,
                /* 26 getName                   */ (void*)subhal_get_name,
                /* 27 initialize(sp<cb>)        */ (void*)subhal_initialize,
        }};

struct SubHalModule {
    void** vptr;
};
SubHalModule gModule = {const_cast<void**>(&gVtable.slots[0])};

__attribute__((visibility("default"))) void* sensorsHalGetSubHal_2_1(uint32_t* version) {
    if (version != nullptr) *version = kSubHalVersion;
    return &gModule;
}
}  // extern "C"
