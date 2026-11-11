// For MIUI 14 vendor for Xiaomi 13, OriginOS6.
#include "light_engine.h"
#include <android/log.h>
#include <sys/system_properties.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <chrono>
#include <cstddef>
#include <cstring>

#define LOG_TAG "sensors-light"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

namespace {
constexpr char kCreateSymbol[] = "_ZN13ssc_interface6createE19ssc_connection_type15ssc_conn_config";
constexpr char kDestroySymbol[] = "_ZN13ssc_interfaceD0Ev";
constexpr sensor_uid kLightSuid = {1103175175223494815ULL, 14038004250674766477ULL};
constexpr sensor_uid kRawSuid = {17238055035138963352ULL, 6315905935778584253ULL};
constexpr uint32_t kMsgSensorConfig = 513;
constexpr uint32_t kMsgOnChangeConfig = 514;
constexpr uint32_t kMsgDisable = 10;
constexpr uint32_t kMsgStdSensorEvent = 1025;
constexpr uint32_t kMsgStdSensorConfigEvent = 513;
constexpr float kStreamRateHz = 20.0f;

uint32_t gLightMsgId = kMsgOnChangeConfig;
uint32_t gRawMsgId = kMsgOnChangeConfig;
uint32_t gDelivery = 1;

void loadEnvOverrides() {
    const char* v;
    if ((v = getenv("LIGHT_HAL_MSGID")) != nullptr) gLightMsgId = (uint32_t)atoi(v);
    if ((v = getenv("LIGHT_HAL_RAW_MSGID")) != nullptr) gRawMsgId = (uint32_t)atoi(v);
    if ((v = getenv("LIGHT_HAL_DELIVERY")) != nullptr) gDelivery = (uint32_t)atoi(v);
}
int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
}
struct LuxPoint {
    float count;
    float lux;
};
constexpr LuxPoint kLuxTable[] = {
#include "lux_table.inc"
};
float luxFromCount(float count) {
    constexpr size_t n = sizeof(kLuxTable) / sizeof(kLuxTable[0]);
    if (count <= kLuxTable[0].count) return kLuxTable[0].lux;
    if (count >= kLuxTable[n - 1].count) return kLuxTable[n - 1].lux;
    const float x = log10f(count);
    size_t i = 0;
    while (i + 2 < n && kLuxTable[i + 1].count < count) i++;
    const float x0 = log10f(kLuxTable[i].count);
    const float x1 = log10f(kLuxTable[i + 1].count);
    const float t = (x - x0) / (x1 - x0);
    return kLuxTable[i].lux + t * (kLuxTable[i + 1].lux - kLuxTable[i].lux);
}
constexpr float kDefaultScaleMul = 1.0f;
constexpr int64_t kScaleCacheNs = 2000000000LL;

float gScaleMul = kDefaultScaleMul;
float gLuxOverride = -1.0f;
int64_t gScaleReadNs = 0;
float gScaleLogged = -1.0f;
float gOverrideLogged = -2.0f;

float readScaleProp(const char* name, float fallback) {
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get(name, buf) > 0) {
        float v = strtof(buf, nullptr);
        if (v > 0.0f && v <= 100.0f) return v;
    }
    return fallback;
}
void refreshScales() {
    const int64_t now = nowNs();
    if (now - gScaleReadNs <= kScaleCacheNs) return;
    gScaleReadNs = now;
    gScaleMul = readScaleProp("persist.vendor.light_hal.scale_mul", kDefaultScaleMul);
    {
        char buf[PROP_VALUE_MAX] = {0};
        float ov = -1.0f;
        if (__system_property_get("persist.vendor.light_hal.lux_override", buf) > 0) {
            ov = strtof(buf, nullptr);
            if (ov < 0.0f || ov > 20000.0f) ov = -1.0f;
        }
        gLuxOverride = ov;
    }
    if (gScaleMul != gScaleLogged || gLuxOverride != gOverrideLogged) {
        gScaleLogged = gScaleMul;
        gOverrideLogged = gLuxOverride;
        LOGI("mapping: scale_mul=%f override=%f", gScaleMul, gLuxOverride);
    }
}
constexpr int64_t kBootstrapNs = 2500LL * 1000000;
constexpr int64_t kFastHeartbeatNs = 200LL * 1000000;
constexpr int64_t kSlowHeartbeatNs = 500LL * 1000000;
constexpr int64_t kHealthRecreateQuietSends = 8;
constexpr int64_t kWatchdogTickNs = 250LL * 1000000;
constexpr int64_t kMinHealthEvents = 5;
constexpr int64_t kQuietWindowNs = 10LL * 1000000000;
constexpr int kMaxRebuilds = 3;
constexpr int64_t kRebuildMinIntervalNs = 60LL * 1000000000;
constexpr int64_t kDeadRetryNs = 300LL * 1000000000;

void putVarint(std::string& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back((char)((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out.push_back((char)v);
}
void putTag(std::string& out, uint32_t field, uint32_t wire) {
    putVarint(out, ((uint64_t)field << 3) | wire);
}
void putBytes(std::string& out, uint32_t field, const void* data, size_t len) {
    putTag(out, field, 2);
    putVarint(out, len);
    out.append((const char*)data, len);
}
void putString(std::string& out, uint32_t field, const std::string& s) {
    putBytes(out, field, s.data(), s.size());
}
void putFixed32(std::string& out, uint32_t field, uint32_t v) {
    putTag(out, field, 5);
    char b[4];
    memcpy(b, &v, 4);
    out.append(b, 4);
}
void putFloat(std::string& out, uint32_t field, float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    putFixed32(out, field, v);
}
void putFixed64(std::string& out, uint32_t field, uint64_t v) {
    putTag(out, field, 1);
    char b[8];
    memcpy(b, &v, 8);
    out.append(b, 8);
}
struct PbCursor {
    const uint8_t* p;
    const uint8_t* end;
    bool eof() const { return p >= end; }
    bool varint(uint64_t& v) {
        v = 0;
        int shift = 0;
        while (p < end && shift < 64) {
            uint8_t b = *p++;
            v |= (uint64_t)(b & 0x7f) << shift;
            if (!(b & 0x80)) return true;
            shift += 7;
        }
        return false;
    }
    bool key(uint32_t& field, uint32_t& wire) {
        uint64_t k;
        if (!varint(k)) return false;
        field = (uint32_t)(k >> 3);
        wire = (uint32_t)(k & 7);
        return true;
    }
    bool bytes(const uint8_t*& data, size_t& len) {
        uint64_t l;
        if (!varint(l)) return false;
        if ((uint64_t)(end - p) < l) return false;
        data = p;
        len = (size_t)l;
        p += l;
        return true;
    }
    bool skip(uint32_t wire) {
        uint64_t v;
        switch (wire) {
            case 0:
                return varint(v);
            case 1:
                if (end - p < 8) return false;
                p += 8;
                return true;
            case 2: {
                const uint8_t* d;
                size_t l;
                return bytes(d, l);
            }
            case 5:
                if (end - p < 4) return false;
                p += 4;
                return true;
            default:
                return false;
        }
    }
};
bool decodeStdSensorEvent(const uint8_t* data, size_t len, std::vector<float>& out) {
    out.clear();
    PbCursor c{data, data + len};
    while (!c.eof()) {
        uint32_t field, wire;
        if (!c.key(field, wire)) return false;
        if (field == 1 && wire == 5) {
            if (c.end - c.p < 4) return false;
            float f;
            memcpy(&f, c.p, 4);
            c.p += 4;
            out.push_back(f);
        } else if (field == 1 && wire == 2) {
            const uint8_t* d;
            size_t l;
            if (!c.bytes(d, l)) return false;
            if (l % 4 != 0) return false;
            for (size_t i = 0; i < l; i += 4) {
                float f;
                memcpy(&f, d + i, 4);
                out.push_back(f);
            }
        } else if (!c.skip(wire)) {
            return false;
        }
    }
    return true;
}
} //namespace end

SscEngine::SscEngine() {
    mSources[kSourceLight].suid = kLightSuid;
    mSources[kSourceRaw].suid = kRawSuid;
}
SscEngine::~SscEngine() { shutdown(); }
std::string SscEngine::buildRequest(const sensor_uid& suid, uint32_t msgId, bool withRate, float rateHz) {
    std::string suidMsg;
    putFixed64(suidMsg, 1, suid.low);
    putFixed64(suidMsg, 2, suid.high);
    std::string req;
    {
        std::string batching;
        putTag(batching, 1, 0);
        putVarint(batching, 0);
        putBytes(req, 1, batching.data(), batching.size());
    }
    if (withRate) {
        std::string cfg;
        putFloat(cfg, 1, rateHz);
        putBytes(req, 2, cfg.data(), cfg.size());
    }
    std::string susp;
    {
        putTag(susp, 1, 0);
        putVarint(susp, 1);
        putTag(susp, 2, 0);
        putVarint(susp, gDelivery);
    }
    std::string msg;
    putBytes(msg, 1, suidMsg.data(), suidMsg.size());
    putFixed32(msg, 2, msgId);
    putBytes(msg, 3, susp.data(), susp.size());
    putBytes(msg, 4, req.data(), req.size());
    return msg;
}
void SscEngine::registerCallbacksLocked(void* iface, uint32_t gen) {
    void** vt = *(void***)iface;
    mRegisterCbFn = vt[0];
    mSendRequestFn = vt[1];
    typedef void (*RegisterCbFn)(void*, sensor_uid, std::function<void(uint32_t, uint64_t)>, std::function<void(int)>, std::function<void(const uint8_t*, size_t, uint64_t)>);
    for (int i = 0; i < kSourceCount; i++) {
        Source src = (Source)i;
        mRespCb[i] = [this, src, gen](uint32_t msgId, uint64_t ts) {
            if (gen != mGeneration.load(std::memory_order_acquire)) return;
            onResponse(msgId, ts, src);
        };
        mErrCb[i] = [this, src, gen](int error) {
            if (gen != mGeneration.load(std::memory_order_acquire)) return;
            onError(error, src);
        };
        mEvtCb[i] = [this, src, gen](const uint8_t* data, size_t size, uint64_t ts) {
            if (gen != mGeneration.load(std::memory_order_acquire)) return;
            onEvent(data, size, ts, src);
        };
        ((RegisterCbFn)mRegisterCbFn)(iface, mSources[i].suid, mRespCb[i], mErrCb[i], mEvtCb[i]);
    }
}
bool SscEngine::init() {
    if (mStarted) return true;
    loadEnvOverrides();
    mLibHandle = dlopen("libssc.so", RTLD_NOW | RTLD_LOCAL);
    if (mLibHandle == nullptr) {
        mLibHandle = dlopen("/vendor/lib64/libssc.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (mLibHandle == nullptr) {
        LOGE("dlopen libssc.so failed: %s", dlerror());
        return false;
    }
    mCreateFn = dlsym(mLibHandle, kCreateSymbol);
    if (mCreateFn == nullptr) {
        LOGE("dlsym create failed: %s", dlerror());
        return false;
    }
    mDestroyFn = dlsym(mLibHandle, kDestroySymbol);
    if (mDestroyFn == nullptr) {
        LOGW("dlsym %s failed: %s -- rebuilds will leak ssc sessions", kDestroySymbol,
             dlerror());
    }
    typedef void* (*CreateFn)(uint32_t, void*);
    mIface = ((CreateFn)mCreateFn)(0, nullptr);
    if (mIface == nullptr) {
        LOGE("ssc_interface::create returned null");
        return false;
    }
    LOGI("ssc_interface created: %p", mIface);
    void** vt = *(void***)mIface;
    mRegisterCbFn = vt[0];
    mSendRequestFn = vt[1];
    registerCallbacksLocked(mIface, mGeneration.fetch_add(1) + 1);
    mRun = true;
    mStarted = true;
    mWatchdog = std::thread([this] { watchdogLoop(); });
    LOGI("engine started (light=%016llx/%016llx raw=%016llx/%016llx)",
         (unsigned long long)kLightSuid.low, (unsigned long long)kLightSuid.high,
         (unsigned long long)kRawSuid.low, (unsigned long long)kRawSuid.high);
    return true;
}
bool SscEngine::sendRequest(Source src, const std::string& msg) {    if (mIface == nullptr || mSendRequestFn == nullptr) return false;
    typedef void (*SendRequestFn)(void*, sensor_uid, bool, std::string);
    ((SendRequestFn)mSendRequestFn)(mIface, mSources[src].suid, true, msg);
    return true;
}
bool SscEngine::sendConfigLocked(Source src, bool enable) {
    uint32_t msgId = enable ? (src == kSourceLight ? gLightMsgId : gRawMsgId) : kMsgDisable;
    if (getenv("LIGHT_HAL_ATTR") != nullptr) msgId = 1;
    bool withRate = enable && msgId == kMsgSensorConfig;
    std::string msg = buildRequest(mSources[src].suid, msgId, withRate, kStreamRateHz);
    bool ok = sendRequest(src, msg);
    LOGI("sendConfig src=%d enable=%d -> %s (%zu bytes)", (int)src, (int)enable, ok ? "ok" : "fail",
         msg.size());
    return ok;
}
bool SscEngine::sendConfig(Source src, bool enable) {
    std::lock_guard<std::mutex> lk(mSendLock);
    return sendConfigLocked(src, enable);
}
void SscEngine::kickLocked() {
    mKick = true;
    mWake.notify_one();
}
void SscEngine::updateDesiredLocked(const char* why) {
    const bool light = mLightClient;
    const bool raw = mRawClient || mLightClient;
    SourceState& l = mSources[kSourceLight];
    if (l.active != light) {
        l.active = light;
        l.healthFails = 0;
        if (light) {
            mRebuilds.store(0, std::memory_order_relaxed);
        }
        LOGI("configureSource src=0 active=%d (%s)", (int)light, why);
    }
    SourceState& r = mSources[kSourceRaw];
    if (r.active != raw) {
        r.active = raw;
        r.healthFails = 0;
        LOGI("configureSource src=1 active=%d (%s)", (int)raw,
             (raw && !mRawClient) ? "raw-for-light" : why);
    }
    kickLocked();
}
void SscEngine::setSensorActive(int32_t type, bool active) {
    std::lock_guard<std::mutex> lk(mLock);
    if (type == kLightType) {
        mLightClient = active;
        updateDesiredLocked("light-client");
    } else if (type == kRawType) {
        mRawClient = active;
        updateDesiredLocked("raw-client");
    }
}
void SscEngine::rearm(const char* why) {
    std::lock_guard<std::mutex> lk(mLock);
    for (int i = 0; i < kSourceCount; i++) {
        if (!mSources[i].active) continue;
        mSources[i].configured = false;
        mSources[i].lastSendNs = 0;
    }
    LOGI("rearm (%s)", why);
    kickLocked();
}
void SscEngine::maybeEnableFallback(Source stalled) {
    if (stalled != kSourceRaw) return;
    bool expected = false;
    if (!mFallback.compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(mLock);
    LOGW("enabling light-source lux fallback (light client=%d)", (int)mLightClient);
    updateDesiredLocked("raw-fallback");
}

bool SscEngine::applySource(Source src, bool enable) {
    const bool ok = sendConfig(src, enable);
    std::lock_guard<std::mutex> lk(mLock);
    SourceState& s = mSources[src];
    if (ok) {
        s.configured = enable;
        if (enable) {
            const int64_t now = nowNs();
            s.lastEventNs = now;
            s.lastSendNs = now;
            s.fastUntilNs = now + kBootstrapNs;
        }
    } else {
        s.configured = !enable;
    }
    return ok;
}
void SscEngine::logStatsLocked(int64_t now) {
    LOGI("stats: light(%s) events=%lld last=%lldms raw=%.0f lux=%.1f (mul %.3f ovr %.1f) | raw(%s) events=%lld last=%lldms ago",
         mSources[kSourceLight].active ? "on" : "off",
         (long long)mSources[kSourceLight].eventCount,
         (long long)(mSources[kSourceLight].lastEventNs
                             ? (now - mSources[kSourceLight].lastEventNs) / 1000000
                             : -1),
         (double)mSources[kSourceLight].lastRaw,
         (double)mSources[kSourceLight].lastLux, (double)gScaleMul,
         (double)gLuxOverride,
         mSources[kSourceRaw].active ? "on" : "off",
         (long long)mSources[kSourceRaw].eventCount,
         (long long)(mSources[kSourceRaw].lastEventNs
                             ? (now - mSources[kSourceRaw].lastEventNs) / 1000000
                             : -1));
    LOGI("state: light(act=%d cfg=%d fails=%d) raw(act=%d cfg=%d fails=%d) fallback=%d rebuilds=%d lastRebuildAgo=%lldms",
         mSources[kSourceLight].active, mSources[kSourceLight].configured,
         mSources[kSourceLight].healthFails,
         mSources[kSourceRaw].active, mSources[kSourceRaw].configured,
         mSources[kSourceRaw].healthFails,
         mFallback.load(std::memory_order_relaxed), mRebuilds.load(),
         mLastRebuildNs.load() ? (long long)((now - mLastRebuildNs.load()) / 1000000) : -1LL);
}
void SscEngine::onResponse(uint32_t msgId, uint64_t ts, Source src) {
    LOGD("resp src=%d msg=%u ts=%llu", (int)src, msgId, (unsigned long long)ts);
    std::lock_guard<std::mutex> lk(mLock);
}
void SscEngine::onError(int error, Source src) {
    LOGW("ssc error src=%d err=%d", (int)src, error);
    {
        std::lock_guard<std::mutex> lk(mLock);
        mSources[src].configured = false;
    }
    if (mErrorSink) mErrorSink("ssc-error");
}
static FILE* gDumpFile = nullptr;
void SscEngine::onEvent(const uint8_t* data, size_t size, uint64_t ts, Source src) {
    const int64_t now = nowNs();
    if (gDumpFile == nullptr) {
        const char* f = getenv("LIGHT_HAL_DUMPFILE");
        if (f != nullptr) gDumpFile = fopen(f, "wb");
    }
    if (gDumpFile != nullptr && size > 0) {
        uint8_t hdr[12];
        uint32_t s = (uint32_t)size;
        uint32_t st = (uint32_t)src;
        memcpy(hdr, &s, 4);
        memcpy(hdr + 4, &st, 4);
        memcpy(hdr + 8, &ts, 4);
        fwrite(hdr, 1, 12, gDumpFile);
        fwrite(data, 1, size, gDumpFile);
        fflush(gDumpFile);
    }
    bool gotSample = false;
    std::vector<float> values;
    {
        std::lock_guard<std::mutex> lk(mLock);
        mSources[src].lastEventNs = now;
        mSources[src].eventCount++;
        mSources[src].healthFails = 0;
            if (mSources[src].eventCount <= 6) {
            char hex[3 * 48 + 1];
            size_t n = size < 48 ? size : 48;
            for (size_t i = 0; i < n; i++) snprintf(hex + 3 * i, 4, "%02x ", data[i]);
            hex[3 * n] = 0;
            LOGI("event src=%d ts=%llu size=%zu: %s", (int)src, (unsigned long long)ts, size, hex);
        }
    }
    PbCursor c{data, data + size};
    while (!c.eof()) {
        uint32_t field, wire;
        if (!c.key(field, wire)) break;
        if (field == 2 && wire == 2) {
            const uint8_t* ev;
            size_t evLen;
            if (!c.bytes(ev, evLen)) break;
            PbCursor e{ev, ev + evLen};
            uint32_t msgId = 0;
            uint64_t evTs = 0;
            const uint8_t* payload = nullptr;
            size_t payloadLen = 0;
            while (!e.eof()) {
                uint32_t f, w;
                if (!e.key(f, w)) break;
                if (f == 1 && w == 5) {
                    if (e.end - e.p < 4) break;
                    memcpy(&msgId, e.p, 4);
                    e.p += 4;
                } else if (f == 2 && w == 1) {
                    if (e.end - e.p < 8) break;
                    memcpy(&evTs, e.p, 8);
                    e.p += 8;
                } else if (f == 3 && w == 2) {
                    if (!e.bytes(payload, payloadLen)) break;
                } else if (!e.skip(w)) {
                    break;
                }
            }
            if (msgId == kMsgStdSensorEvent && payload != nullptr &&
                decodeStdSensorEvent(payload, payloadLen, values)) {
                gotSample = !values.empty();
                if (mSources[src].eventCount <= 6) {
                    char hex[3 * 32 + 1];
                    size_t n = payloadLen < 32 ? payloadLen : 32;
                    for (size_t i = 0; i < n; i++) snprintf(hex + 3 * i, 4, "%02x ", payload[i]);
                    hex[3 * n] = 0;
                    LOGI("payload src=%d len=%zu: %s", (int)src, payloadLen, hex);
                }
            } else if (msgId == kMsgStdSensorEvent) {
                LOGD("event src=%d msg=%u payload=%zu (decode failed)", (int)src, msgId,
                     payloadLen);
            } else if (mSources[src].eventCount <= 6 || getenv("LIGHT_HAL_ATTR") != nullptr) {
                char hex[3 * 64 + 1];
                size_t n = payloadLen < 400 ? payloadLen : 400;
                for (size_t i = 0; i < n; i++) snprintf(hex + 3 * i, 4, "%02x ", payload ? payload[i] : 0);
                hex[3 * n] = 0;
                LOGI("event src=%d msg=%u payload=%zu: %s", (int)src, msgId, payloadLen, hex);
            }
            continue;
        }
        if (!c.skip(wire)) break;
    }
    if (!gotSample || mEventSink == nullptr) return;
    if (src == kSourceLight) {
        if (!mFallback.load(std::memory_order_relaxed)) return;
        const float lux = values[0];
        if (lux <= 0.05f || lux > 20000.0f) {
            LOGD("fallback sample rejected: lux=%f", lux);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mLock);
            mSources[kSourceLight].lastRaw = values[0];
            mSources[kSourceLight].lastLux = lux;
        }
        mEventSink(kLightType, &lux, 1, (int64_t)ts);
        return;
    }
    mEventSink(kRawType, values.data(), (int)values.size(), (int64_t)ts);
    refreshScales();
    float lux = (gLuxOverride >= 0.0f) ? gLuxOverride : luxFromCount(values[0]) * gScaleMul;
    {
        std::lock_guard<std::mutex> lk(mLock);
        mSources[kSourceLight].lastRaw = values[0];
        mSources[kSourceLight].lastLux = lux;
        if (mRebuilds.load(std::memory_order_relaxed) != 0) {
            mRebuilds.store(0, std::memory_order_relaxed);
        }
    }
    if (mFallback.exchange(false, std::memory_order_relaxed)) {
        LOGI("raw stream recovered, disabling light-source fallback");
        updateDesiredLocked("raw-recovered");
    }
    mEventSink(kLightType, &lux, 1, (int64_t)ts);
}
void SscEngine::watchdogLoop() {
    int64_t lastStats = nowNs();
    struct Job {
        Source src;
        bool enable;
        bool stateChange;
    };
    while (true) {
        Job jobs[kSourceCount];
        int nJobs = 0;
        bool rebuild = false;
        bool rearmNeeded = false;
        {
            std::unique_lock<std::mutex> lk(mLock);
            mWake.wait_for(lk, std::chrono::nanoseconds(kWatchdogTickNs),
                           [this] { return mKick || !mRun; });
            mKick = false;
            if (!mRun) break;
            const int64_t now = nowNs();
            if (now - lastStats > 30LL * 1000000000) {
                lastStats = now;
                logStatsLocked(now);
            }
            for (int i = 0; i < kSourceCount; i++) {
                SourceState& s = mSources[i];
                if (s.configured != s.active) {
                    if (s.needsDisable) {
                        jobs[nJobs].src = (Source)i;
                        jobs[nJobs].enable = false;
                        jobs[nJobs].stateChange = true;
                        nJobs++;
                        s.needsDisable = false;
                        continue;
                    }
                    jobs[nJobs].src = (Source)i;
                    jobs[nJobs].enable = s.active;
                    jobs[nJobs].stateChange = true;
                    nJobs++;
                    continue;
                }
                if (!s.active || !s.configured) continue;
                const int64_t interval =
                        (now < s.fastUntilNs) ? kFastHeartbeatNs : kSlowHeartbeatNs;
                if (now - s.lastSendNs < interval) continue;
                jobs[nJobs].src = (Source)i;
                jobs[nJobs].enable = true;
                jobs[nJobs].stateChange = false;
                nJobs++;
                s.lastSendNs = now;
                if (i == kSourceRaw && s.active && now - s.lastEventNs > kQuietWindowNs) {
                    const int64_t silent_ms = (now - s.lastEventNs) / 1000000;
                    s.lastEventNs = now;
                    if (s.healthFails == 0) {
                        s.healthFails = 1;
                        rearmNeeded = true;
                        s.needsDisable = true;
                        s.configured = false;
                        s.lastSendNs = 0;
                        LOGW("src=%d: no samples for %lld ms (%lld total), resubscribing",
                             (int)i, (long long)silent_ms, (long long)s.eventCount);
                    } else {
                        const int rebuilds = mRebuilds.load();
                        const int64_t lastNs = mLastRebuildNs.load();
                        const int64_t since = (lastNs != 0) ? (now - lastNs) : INT64_MAX;
                        const bool rebuildOk =
                                (rebuilds < kMaxRebuilds && since >= kRebuildMinIntervalNs) ||
                                (rebuilds >= kMaxRebuilds && since >= kDeadRetryNs);
                        if (rebuildOk) {
                            if (rebuilds >= kMaxRebuilds) {
                                LOGW("src=%d: still dead after budget (%lld ms), retrying rebuild anyway",
                                     (int)i, (long long)since / 1000000);
                            }
                            s.healthFails = 2;
                            rebuild = true;
                        } else if (s.healthFails < 3) {
                            s.healthFails = 3;
                            LOGE("src=%d: stream dead after %d rebuild(s), last %lld ms ago; "
                                 "falling back to light source, rebuild retries every %lld s",
                                 (int)i, rebuilds,
                                 lastNs ? (long long)since / 1000000 : -1LL,
                                 kDeadRetryNs / 1000000000);
                            maybeEnableFallback((Source)i);
                        }
                    }
                    continue;
                }
            }
        }
        for (int j = 0; j < nJobs; j++) {
            if (jobs[j].stateChange) {
                applySource(jobs[j].src, jobs[j].enable);
            } else {
                sendConfig(jobs[j].src, jobs[j].enable);
            }
        }
        if (rearmNeeded) rearm("quiet-source");
        if (rebuild) rebuildInterface();
    }
}
void SscEngine::destroyInterface(void* iface) {
    if (iface == nullptr) return;
    if (mDestroyFn == nullptr) {
        LOGW("destroy: no ssc_interface destructor resolved, leaking %p", iface);
        return;
    }
    typedef void (*DestroyFn)(void*);
    ((DestroyFn)mDestroyFn)(iface);
    LOGI("released ssc_interface %p", iface);
}
void SscEngine::rebuildInterface() {
    if (mRebuilds.load() >= kMaxRebuilds) return;
    std::lock_guard<std::mutex> send(mSendLock);
    if (mRebuilds.load() >= kMaxRebuilds) return;
    const int64_t now = nowNs();
    typedef void* (*CreateFn)(uint32_t, void*);
    void* iface = ((CreateFn)mCreateFn)(0, nullptr);
    if (iface == nullptr) {
        LOGE("rebuildInterface: create failed");
        std::lock_guard<std::mutex> lk(mLock);
        for (int k = 0; k < kSourceCount; k++) {
            if (mSources[k].active) mSources[k].configured = false;
        }
        return;
    }
    const int n = mRebuilds.fetch_add(1) + 1;
    mLastRebuildNs.store(now);
    void* previous = mIface;
    mIface = iface;
    const uint32_t gen = mGeneration.fetch_add(1) + 1;
    registerCallbacksLocked(iface, gen);
    destroyInterface(previous);
    LOGI("rebuild %d/%d: new ssc_interface %p (gen %u), previous %p released",
         n, (int)kMaxRebuilds, iface, gen, previous);
    for (int k = 0; k < kSourceCount; k++) {
        bool active;
        {
            std::lock_guard<std::mutex> lk(mLock);
            active = mSources[k].active;
        }
        if (!active) continue;
        const bool ok = sendConfigLocked((Source)k, true);
        std::lock_guard<std::mutex> lk(mLock);
        mSources[k].configured = ok;
        if (ok) {
            mSources[k].lastSendNs = nowNs();
            mSources[k].lastEventNs = nowNs();
            mSources[k].fastUntilNs = nowNs() + kBootstrapNs;
        }
    }
}
void SscEngine::shutdown() {
    if (!mStarted) return;
    {
        std::lock_guard<std::mutex> lk(mLock);
        mRun = false;
        mWake.notify_all();
    }
    if (mWatchdog.joinable()) mWatchdog.join();
    for (int i = 0; i < kSourceCount; i++) {
        if (mSources[i].configured) sendConfig((Source)i, false);
    }
    void* iface = mIface;
    mIface = nullptr;
    mSendRequestFn = nullptr;
    destroyInterface(iface);
    mStarted = false;
}
