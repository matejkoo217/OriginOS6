// SSC light-engine probe
// For MIUI 14 vendor for Xiaomi 13, OriginOS6.
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <cstring>
#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>
#include "light_engine.h"

static std::atomic<bool> gRun{true};
static std::atomic<int> gCount{0};
static void onEvent(int32_t type, const float* values, int count, int64_t ts) {
    int n = gCount.fetch_add(1) + 1;
    printf("[%6d] type=%d count=%d ts=%lld", n, type, count, (long long)ts);
    for (int i = 0; i < count && i < 8; i++) printf(" %.2f", values[i]);
    printf("\n");
    fflush(stdout);
}
int main(int argc, char** argv) {
    int seconds = (argc > 1) ? atoi(argv[1]) : 15;
    printf("light engine probe: running %d s\n", seconds);
    SscEngine engine;
    engine.setEventSink(onEvent);
    engine.setErrorSink([](const std::string& what) { printf("error: %s\n", what.c_str()); });
    if (!engine.init()) {
        printf("engine init failed\n");
        return 1;
    }
    const char* only = getenv("LIGHT_HAL_ONLY");
    if (only == nullptr || strcmp(only, "light") == 0) engine.setSensorActive(5, true);
    if (only == nullptr || strcmp(only, "raw") == 0) engine.setSensorActive(33171111, true);
    for (int i = 0; i < seconds; i++) std::this_thread::sleep_for(std::chrono::seconds(1));
    engine.shutdown();
    printf("total events: %d\n", gCount.load());
    return 0;
}
