// SSC light engine
// For MIUI 14 vendor for Xiaomi 13, OriginOS6.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct sensor_uid {
    uint64_t low;
    uint64_t high;
};

class SscEngine {
  public:
    using EventSink = std::function<void(int32_t type, const float* values, int count, int64_t ts_ns)>;
    using ErrorSink = std::function<void(const std::string& what)>;
    SscEngine();
    ~SscEngine();
    bool init();
    void setEventSink(EventSink sink) { mEventSink = std::move(sink); }
    void setErrorSink(ErrorSink sink) { mErrorSink = std::move(sink); }
    void setSensorActive(int32_t type, bool active);
    void rearm(const char* why);
    void shutdown();
    bool started() const { return mStarted; }

  private:
    static constexpr int32_t kLightType = 5;
    static constexpr int32_t kRawType = 33171111; // 0x1FA26A7 xiaomi raw ALS
    enum Source { kSourceLight = 0, kSourceRaw = 1, kSourceCount = 2 };
    struct SourceState {
        sensor_uid suid{0, 0};
        bool active = false;
        bool configured = false;
        int64_t lastEventNs = 0;
        int64_t lastSendNs = 0;
        int64_t fastUntilNs = 0;
        int64_t quietSends = 0;
        int64_t eventCount = 0;
        float lastRaw = 0.0f;
        float lastLux = 0.0f;
    };
    void watchdogLoop();
    void rebuildInterface();
    void onResponse(uint32_t msgId, uint64_t ts, Source src);
    void onError(int error, Source src);
    void onEvent(const uint8_t* data, size_t size, uint64_t ts, Source src);
    bool sendConfig(Source src, bool enable);
    bool sendRequest(Source src, const std::string& msg);
    void maybeEnableFallback(Source stalled);
    void configureSource(Source src, bool active, const char* why);
    static std::string buildRequest(const sensor_uid& suid, uint32_t msgId, bool withRate, float rateHz);
    std::string decodeError() const;
    void* mLibHandle = nullptr;
    void* mIface = nullptr;
    void* mCreateFn = nullptr;
    void* mRegisterCbFn = nullptr;
    void* mSendRequestFn = nullptr;
    std::function<void(uint32_t, uint64_t)> mRespCb[kSourceCount];
    std::function<void(int)> mErrCb[kSourceCount];
    std::function<void(const uint8_t*, size_t, uint64_t)> mEvtCb[kSourceCount];
    SourceState mSources[kSourceCount];
    EventSink mEventSink;
    ErrorSink mErrorSink;
    bool mLightClient = false;
    bool mRawClient = false;
    std::mutex mLock;
    std::thread mWatchdog;
    std::atomic<bool> mRun{false};
    std::atomic<bool> mStarted{false};
    int64_t lastWatchdogLog = 0;
};
