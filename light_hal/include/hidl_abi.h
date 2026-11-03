// Minimal ABI-exact subset of libhidlbase + android.hardware.sensors@2.1 types.
// For MIUI 14 vendor for Xiaomi 13, OriginOS6.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace android {
namespace hardware {

class Status {
  public:
    enum Exception : int32_t { EX_NONE = 0 };
    Status() : mException(EX_NONE), mErrorCode(0), mMessage() {}
    Status(int32_t exception, int32_t errorCode = 0)
        : mException(exception), mErrorCode(errorCode), mMessage() {}
    bool isOk() const { return mException == EX_NONE; }
  private:
    int32_t mException;
    int32_t mErrorCode;
    std::string mMessage;
};

namespace details {

class return_status {
  protected:
    Status mStatus{};
    mutable bool mCheckedStatus = true;
  public:
    return_status() {}
    return_status(const Status& s) : mStatus(s) {}
    return_status(const return_status&) = delete;
    return_status& operator=(const return_status&) = delete;
    return_status(return_status&& other) noexcept
        : mStatus(other.mStatus), mCheckedStatus(other.mCheckedStatus) {}
    return_status& operator=(return_status&& other) noexcept {
        mStatus = other.mStatus;
        mCheckedStatus = other.mCheckedStatus;
        return *this;
    }
    ~return_status() = default;

    bool isOkUnchecked() const { return mStatus.isOk(); }
    bool isOk() const {
        mCheckedStatus = true;
        return mStatus.isOk();
    }
    void assertOk() const {}
};
}  // namespace details

template <typename T>
class Return : public details::return_status {
  private:
    T mVal{};
  public:
    Return(T v) : details::return_status(), mVal(v) {}
    Return(Status s) : details::return_status(s) {}
    Return(Return&&) = default;
    Return& operator=(Return&&) = default;
    ~Return() = default;

    operator T() const { return mVal; }
    T withDefault(T t) const { return isOk() ? mVal : t; }
};
template <>
class Return<void> : public details::return_status {
  public:
    Return() : details::return_status() {}
    Return(const Status& s) : details::return_status(s) {}
    Return(Return&&) = default;
    Return& operator=(Return&&) = default;
    ~Return() = default;
};
static inline Return<void> Void() { return Return<void>(); }

}  // namespace hardware
}  // namespace android

struct hidl_string {
    const char* mBuffer;
    uint32_t mSize;
    bool mOwnsBuffer;
    uint8_t mPad[3];
    hidl_string() : mBuffer(nullptr), mSize(0), mOwnsBuffer(false) { memset(mPad, 0, 3); }
    hidl_string(const char* s) : hidl_string() {
        if (s != nullptr) {
            mSize = strlen(s);
            char* buf = new char[mSize + 1];
            memcpy(buf, s, mSize + 1);
            mBuffer = buf;
            mOwnsBuffer = true;
        }
    }
    hidl_string(const std::string& s) : hidl_string(s.c_str()) {}
    hidl_string(const hidl_string& o) : hidl_string(o.mBuffer) {}
    hidl_string(hidl_string&& o) noexcept : hidl_string() {
        mBuffer = o.mBuffer;
        mSize = o.mSize;
        mOwnsBuffer = o.mOwnsBuffer;
        o.mBuffer = nullptr;
        o.mSize = 0;
        o.mOwnsBuffer = false;
    }
    ~hidl_string() {
        if (mOwnsBuffer) delete[] mBuffer;
    }
    hidl_string& operator=(const hidl_string& o) {
        if (this != &o) {
            if (mOwnsBuffer) delete[] mBuffer;
            mBuffer = nullptr;
            mSize = 0;
            mOwnsBuffer = false;
            if (o.mBuffer != nullptr) {
                mSize = o.mSize;
                char* buf = new char[mSize + 1];
                memcpy(buf, o.mBuffer, mSize + 1);
                mBuffer = buf;
                mOwnsBuffer = true;
            }
        }
        return *this;
    }
    const char* c_str() const { return mBuffer ? mBuffer : ""; }
    size_t size() const { return mSize; }
    bool empty() const { return mSize == 0; }
};
static_assert(sizeof(hidl_string) == 16, "hidl_string ABI");

template <typename T>
struct hidl_vec {
    T* mBuffer;
    uint32_t mSize;
    bool mOwnsBuffer;
    uint8_t mPad[3];
    hidl_vec() : mBuffer(nullptr), mSize(0), mOwnsBuffer(false) { memset(mPad, 0, 3); }
    hidl_vec(const std::vector<T>& v) : hidl_vec() { *this = v; }
    hidl_vec(const hidl_vec& o) : hidl_vec() { *this = o; }
    hidl_vec(hidl_vec&& o) noexcept : hidl_vec() {
        mBuffer = o.mBuffer;
        mSize = o.mSize;
        mOwnsBuffer = o.mOwnsBuffer;
        o.mBuffer = nullptr;
        o.mSize = 0;
        o.mOwnsBuffer = false;
    }
    ~hidl_vec() {
        if (mOwnsBuffer) delete[] mBuffer;
    }
    hidl_vec& operator=(const hidl_vec& o) {
        if (this != &o) {
            if (mOwnsBuffer) delete[] mBuffer;
            mSize = o.mSize;
            if (o.mSize > 0) {
                mBuffer = new T[o.mSize];
                for (uint32_t i = 0; i < o.mSize; i++) mBuffer[i] = o.mBuffer[i];
                mOwnsBuffer = true;
            } else {
                mBuffer = nullptr;
                mOwnsBuffer = false;
            }
        }
        return *this;
    }
    hidl_vec& operator=(const std::vector<T>& v) {
        if (mOwnsBuffer) delete[] mBuffer;
        mSize = (uint32_t)v.size();
        if (mSize > 0) {
            mBuffer = new T[mSize];
            for (uint32_t i = 0; i < mSize; i++) mBuffer[i] = v[i];
            mOwnsBuffer = true;
        } else {
            mBuffer = nullptr;
            mOwnsBuffer = false;
        }
        return *this;
    }
    T* data() { return mBuffer; }
    const T* data() const { return mBuffer; }
    size_t size() const { return mSize; }
};
static_assert(sizeof(hidl_vec<float>) == 16, "hidl_vec ABI");

namespace android {
namespace hardware {
namespace sensors {
namespace V1_0 {

enum Result : int32_t {
    OK = 0,
    PERMISSION_DENIED = -1,
    NO_MEMORY = -12,
    BAD_VALUE = -22,
    INVALID_OPERATION = -38,
};
enum class OperationMode : int32_t {
    NORMAL = 0,
    DATA_INJECTION = 1,
};
enum class RateLevel : int32_t {
    STOP = 0,
    NORMAL = 1,
    FAST = 2,
    VERY_FAST = 3,
};
enum class SensorType : int32_t {
    META_DATA = 0,
    ACCELEROMETER = 1,
    MAGNETIC_FIELD = 2,
    ORIENTATION = 3,
    GYROSCOPE = 4,
    LIGHT = 5,
    PRESSURE = 6,
    TEMPERATURE = 7,
    PROXIMITY = 8,
    GRAVITY = 9,
    LINEAR_ACCELERATION = 10,
    ROTATION_VECTOR = 11,
    RELATIVE_HUMIDITY = 12,
    AMBIENT_TEMPERATURE = 13,
    MAGNETIC_FIELD_UNCALIBRATED = 14,
    GAME_ROTATION_VECTOR = 15,
    GYROSCOPE_UNCALIBRATED = 16,
    SIGNIFICANT_MOTION = 17,
    STEP_DETECTOR = 18,
    STEP_COUNTER = 19,
    GEOMAGNETIC_ROTATION_VECTOR = 20,
    HEART_RATE = 21,
    TILT_DETECTOR = 22,
    WAKE_GESTURE = 23,
    GLANCE_GESTURE = 24,
    PICK_UP_GESTURE = 25,
    WRIST_TILT_GESTURE = 26,
    DEVICE_ORIENTATION = 27,
    POSE_6DOF = 28,
    STATIONARY_DETECT = 29,
    MOTION_DETECT = 30,
    HEART_BEAT = 31,
    DYNAMIC_SENSOR_META = 32,
    ADDITIONAL_INFO = 33,
    LOW_LATENCY_OFFBODY_DETECT = 34,
    ACCELEROMETER_UNCALIBRATED = 35,
};
enum class SensorFlagBits : uint32_t {
    WAKE_UP = 1,
    CONTINUOUS_MODE = 0,
    ON_CHANGE_MODE = 2,
    ONE_SHOT_MODE = 4,
    SPECIAL_REPORTING_MODE = 6,
    DATA_INJECTION = 0x10,
    DYNAMIC_SENSOR = 0x20,
    ADDITIONAL_INFO = 0x40,
    DIRECT_CHANNEL_ASHMEM = 0x400,
    DIRECT_CHANNEL_GRALLOC = 0x800,
};
struct Event {
    int64_t timestamp;
    int32_t sensorHandle;
    int32_t sensorType;
    union EventPayload {
        float scalar;
        uint64_t stepCount;
        float data[16];
        uint8_t raw[64];
    } u;
};
static_assert(sizeof(Event) == 80, "Event ABI");
}  // namespace V1_0

namespace V2_1 {

enum class SensorType : int32_t {
    HINGE_ANGLE = 36,
    LIGHT = 5,
};
struct SensorInfo {
    int32_t sensorHandle;
    hidl_string name;
    hidl_string vendor;
    int32_t version;
    int32_t type;
    hidl_string typeAsString;
    float maxRange;
    float resolution;
    float power;
    int32_t minDelay;
    uint32_t fifoReservedEventCount;
    uint32_t fifoMaxEventCount;
    hidl_string requiredPermission;
    int32_t maxDelay;
    uint32_t flags;
};
static_assert(sizeof(SensorInfo) == 112, "SensorInfo ABI");

struct Event {
    int64_t timestamp;
    int32_t sensorHandle;
    int32_t sensorType;
    union EventPayload {
        float scalar;
        uint64_t stepCount;
        float data[16];
        uint8_t raw[64];
    } u;
};
static_assert(sizeof(Event) == 80, "V2_1::Event ABI");

}  // namespace V2_1

struct SharedMemInfo {
    uint8_t opaque[64];
};

}  // namespace sensors
}  // namespace hardware
}  // namespace android

struct hidl_handle {
    void* mHandle;
    bool mOwnsHandle;
    uint8_t mPad[7];
};

namespace android {
class RefBase {
  public:
    void incStrong(const void* id) const;
    void decStrong(const void* id) const;
};
}  // namespace android

namespace light_hal {

enum : size_t {
    kCallbackPostEvents = 0x80,
    kCallbackCreateScopedWakelock = 0x88,
};
enum : size_t { kScopedWakelockStorage = 64 };
enum : size_t { kVbaseOffsetSlot = 3 };

}  // namespace light_hal
static_assert(sizeof(android::hardware::Return<android::hardware::sensors::V1_0::Result>) == 40, "Return<Result> ABI");
static_assert(sizeof(android::hardware::Return<void>) == 40, "Return<void> ABI");
