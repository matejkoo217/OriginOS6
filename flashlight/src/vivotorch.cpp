// SPDX-License-Identifier: MIT
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <android/log.h>
#define TAG "libvivotorch"
#define TORCH_NODE "/sys/class/leds/led:torch_0/brightness"
#define CAMERA_TORCH_VALUE 90
#define RAMP_MS  350
#define TICK_US  16000

static int map_gear(int level) {
    switch (level) {
        case 1:  return 90;
        case 2:  return 190;
        case 3:  return 320;
        case 4:  return 500;
        default: return CAMERA_TORCH_VALUE;
    }
}
static pthread_mutex_t g_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond  = PTHREAD_COND_INITIALIZER;
static int  g_from   = 0;
static int  g_target = 0;
static int  g_cur    = 0;
static long long g_start_ms = 0;
static bool g_active = false;
static bool g_worker = false;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void write_torch(int val) {
    int fd = open(TORCH_NODE, O_WRONLY);
    if (fd < 0) {
        // __android_log_print(ANDROID_LOG_ERROR, TAG, "open %s failed: %s", TORCH_NODE, strerror(errno));
        return;
    }
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d", val);
    if (write(fd, buf, n) != n)
        // __android_log_print(ANDROID_LOG_ERROR, TAG, "write %s failed: %s", TORCH_NODE, strerror(errno));
    close(fd);
}
static void *ramp_worker(void *) {
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_active)
            pthread_cond_wait(&g_cond, &g_lock);
        long long now = now_ms();
        double t = (double)(now - g_start_ms) / RAMP_MS;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        int val = (t >= 1.0) ? g_target
                             : (int)lround(g_from + (g_target - g_from) * t);
        g_cur = val;
        if (t >= 1.0)
            g_active = false;
        pthread_mutex_unlock(&g_lock);
        write_torch(val);
        if (t >= 1.0)
            continue;
        struct timespec ts = { 0, TICK_US * 1000L };
        nanosleep(&ts, NULL);
    }
    return NULL;
}
static void start_worker_locked(void) {
    if (g_worker)
        return;
    g_worker = true;
    pthread_t th;
    if (pthread_create(&th, NULL, ramp_worker, NULL) == 0)
        pthread_detach(th);
}
__attribute__((visibility("default")))
extern "C" void torchLevelChange(int level) {
    pthread_mutex_lock(&g_lock);
    start_worker_locked();
    if (level <= 0) {
        g_active = false;
        g_from = g_target = g_cur = 0;
        // __android_log_print(ANDROID_LOG_INFO, TAG, "torchLevelChange(%d) -> off/clear", level);
        pthread_mutex_unlock(&g_lock);
        return;
    }
    int target = map_gear(level);
    g_from      = (g_cur == 0) ? target : g_cur;
    g_target    = target;
    g_start_ms  = now_ms();
    g_active    = true;
    // __android_log_print(ANDROID_LOG_INFO, TAG, "torchLevelChange(%d) -> %d (from %d)", level, target, g_from);
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
}
