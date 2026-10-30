// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
int *__errno_location(void) {
    static int e = 0;
    return &e;
}
extern int native_hidl_init(void);
extern int native_extcmd(uint32_t cmd, uint32_t param);
extern int __system_property_get(const char *name, char *value);
extern int __system_property_set(const char *name, const char *value);
int g_debug = 0;
#define TAG "FOD_BRIDGE"
#define LOGI(...) do { if (g_debug) { printf("[INFO][" TAG "] " __VA_ARGS__); printf("\n"); fflush(stdout); } } while(0)
#define LOGW(...) do { if (g_debug) { printf("[WARN][" TAG "] " __VA_ARGS__); printf("\n"); fflush(stdout); } } while(0)
#define LOGE(...) do { fprintf(stderr, "[ERR ][" TAG "] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while(0)
#define LOGD(...) do { if (g_debug) { printf("[DBG ][" TAG "] " __VA_ARGS__); printf("\n"); fflush(stdout); } } while(0)
static void init_identity(void) {
    static const unsigned char k_dbg[] = {
        0x28, 0x35, 0x74, 0x30, 0x31, 0x35, 0x35, 0x74,
        0x3e, 0x3f, 0x38, 0x2f, 0x3d,
    };
    static const unsigned char k_mark[] = {
        0x2a, 0x35, 0x28, 0x2e, 0x74, 0x38, 0x23, 0x74,
        0x30, 0x31, 0x35, 0x35,
    };
    static const unsigned char v_mark[] = { 0xaa, 0xc5, 0xca, 0xff };
    char key[32];
    char mark[32];
    char value[8];
    char val[8] = {0};
    unsigned i;
    for (i = 0; i < sizeof(k_dbg); i++)
        key[i] = k_dbg[i] ^ 0x5a;
    key[sizeof(k_dbg)] = 0;
    if (__system_property_get(key, val) > 0 && strcmp(val, "true") == 0)
        g_debug = 1;
    for (i = 0; i < sizeof(k_mark); i++)
        mark[i] = k_mark[i] ^ 0x5a;
    mark[sizeof(k_mark)] = 0;
    for (i = 0; i < sizeof(v_mark); i++)
        value[i] = v_mark[i] ^ 0x5a;
    value[sizeof(v_mark)] = 0;
    __system_property_set(mark, value);
}
static int g_fod_x_min = 4420;
static int g_fod_x_max = 4420 + 1960;
static int g_fod_y_min = 20520;
static int g_fod_y_max = 20520 + 1960;
#define MAX_HBM_TIMEOUT_MS 3000
static const char *const FOD_PRESS_STATUS_PATHS[] = { "/sys/class/touch/touch_dev/fod_press_status", "/sys/devices/virtual/touch/touch_dev/fod_press_status", "/sys/devices/virtual/touch/tp_dev/fod_status", NULL
};
static const char *const FOD_AREA_PATHS[] = {
    "/sys/devices/platform/soc/ac0000.qcom,qupv3_1_geni_se/a90000.spi/spi_master/spi0/spi0.0/fod_area", NULL
};
#define DISP_FEATURE_DEV "/dev/mi_display/disp_feature"
#define MI_DISP_PRIMARY 0
enum lhbm_target_brightness_state {
    LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP = 0,
    LHBM_TARGET_BRIGHTNESS_OFF_AUTH_STOP = 1,
    LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT = 2,
    LHBM_TARGET_BRIGHTNESS_WHITE_110NIT  = 3,
    LHBM_TARGET_BRIGHTNESS_GREEN_500NIT  = 4,
};
struct disp_base {
    uint32_t flag;
    uint32_t disp_id;
};
struct disp_local_hbm_req {
    struct disp_base base;
    uint32_t local_hbm_value;
};
#define MI_DISP_IOCTL_SET_LOCAL_HBM   _IOW('D', 0x0E, struct disp_local_hbm_req)
static const char *g_fod_press_node = NULL;
static int g_disp_feature_fd = -1;
static volatile int g_running = 1;
static volatile int g_finger_down_state = 0;
static volatile int g_in_fod_area = 0;
static volatile int g_session_armed = 0;
static volatile int g_session_known = 0;
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_timer_cond = PTHREAD_COND_INITIALIZER;
static uint64_t g_hbm_on_timestamp_ms = 0;
static uint64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
static int write_node(const char *path, const char *val) {
    if (!path) return -1;
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        LOGE("Failed to open %s for write (errno=%d)", path, errno);
        return -1;
    }
    ssize_t len = strlen(val);
    ssize_t written = write(fd, val, len);
    close(fd);
    return (written == len) ? 0 : -1;
}
static const char* probe_first_existing(const char *const paths[]) {
    for (int i = 0; paths[i] != NULL; ++i) {
        if (access(paths[i], F_OK) == 0)
            return paths[i];
    }
    return NULL;
}
#define FOD_KEY_SCANCODE 254
static int g_uinput_fd = -1;
static int init_uinput_key(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        LOGE("open /dev/uinput failed (errno=%d)", errno);
        return -1;
    }
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_KEYBIT, FOD_KEY_SCANCODE) < 0) {
        LOGE("uinput UI_SET_*BIT failed (errno=%d)", errno);
        close(fd);
        return -1;
    }
    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor = 0x0f0d;
    usetup.id.product = 0x0254;
    snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "fod_key");
    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0 ||
        ioctl(fd, UI_DEV_CREATE) < 0) {
        LOGE("uinput setup/create failed (errno=%d)", errno);
        close(fd);
        return -1;
    }
    LOGI("uinput FOD key ready (scancode %d -> FINGERPRINT_WAKE/504)", FOD_KEY_SCANCODE);
    return fd;
}

static void inject_fod_key(int down) {
    if (g_uinput_fd < 0)
        return;
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_KEY;
    ev.code = FOD_KEY_SCANCODE;
    ev.value = down ? 1 : 0;
    if (write(g_uinput_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
        LOGE("uinput EV_KEY write failed (errno=%d)", errno);
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    if (write(g_uinput_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
        LOGE("uinput EV_SYN write failed (errno=%d)", errno);
    LOGD("inject FOD key %s", down ? "DOWN" : "UP");
}
static void load_fod_area_from_kernel(void) {
    const char *path = probe_first_existing(FOD_AREA_PATHS);
    if (!path) {
        LOGW("fod_area node not found, keeping compiled-in defaults");
        return;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        LOGW("Cannot open %s (errno=%d)", path, errno);
        return;
    }
    char buf[128] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    int lx = 0;
    int ly = 0;
    int xs = 0;
    int ys = 0;
    char *p = buf;
    char *v;
    v = strstr(p, "lx:");
    if (v) lx = atoi(v + 3);
    v = strstr(p, "ly:");
    if (v) ly = atoi(v + 3);
    v = strstr(p, "x_size:");
    if (v) xs = atoi(v + 7);
    v = strstr(p, "y_size:");
    if (v) ys = atoi(v + 7);
    if (lx > 0 && ly > 0 && xs > 0 && ys > 0) {
        g_fod_x_min = lx;
        g_fod_x_max = lx + xs;
        g_fod_y_min = ly;
        g_fod_y_max = ly + ys;
        LOGI("FOD rect from kernel (raw 10x units): x [%d..%d], y [%d..%d] (center ~%d,%d px)",
             g_fod_x_min,
             g_fod_x_max,
             g_fod_y_min,
             g_fod_y_max,
             (g_fod_x_min + g_fod_x_max) / 20,
             (g_fod_y_min + g_fod_y_max) / 20);
    } else {
        LOGW("Unparseable fod_area content '%s', keeping defaults", buf);
    }
}
static int open_disp_feature(void) {
    int fd = open(DISP_FEATURE_DEV, O_RDWR);
    if (fd < 0) {
        LOGE("Failed to open %s (errno=%d)", DISP_FEATURE_DEV, errno);
        return -1;
    }
    return fd;
}
static int set_local_hbm(uint32_t value) {
    if (g_disp_feature_fd < 0) {
        g_disp_feature_fd = open_disp_feature();
        if (g_disp_feature_fd < 0)
            return -1;
    }
    struct disp_local_hbm_req req;
    memset(&req, 0, sizeof(req));
    req.base.flag = 0;
    req.base.disp_id = MI_DISP_PRIMARY;
    req.local_hbm_value = value;
    int ret = ioctl(g_disp_feature_fd, MI_DISP_IOCTL_SET_LOCAL_HBM, &req);
    if (ret < 0) {
        LOGE("ioctl SET_LOCAL_HBM(%u) failed (errno=%d)", value, errno);
    } else {
        LOGD("ioctl SET_LOCAL_HBM(%u) OK", value);
    }
    return ret;
}
static void send_extcmd_sequence(int down, int x, int y) {
    LOGD("extCmd seq native (down=%d x=%d y=%d)", down, x, y);
    if (down) {
        native_extcmd(2, (uint32_t)x);
        native_extcmd(3, (uint32_t)y);
        native_extcmd(1, 1);
        native_extcmd(10, 1);
    } else {
        native_extcmd(2, 0);
        native_extcmd(3, 0);
        native_extcmd(1, 0);
        native_extcmd(10, 0);
    }
}
static const char *const HAL_FP_LOG_PATHS[] = {
    "/data/vendor/fpdump/fp_log.txt",
    "/data/vendor/fpdump/fp_init_log.txt",
    "/dev/fpdump/fp_log.txt",
    "/dev/fpdump/fp_init_log.txt",
    NULL
};
static const char *const HAL_FP_LOG_DIRS[] = {
    "/data/vendor/fpdump",
    "/dev/fpdump",
    NULL
};
#define HAL_FP_LOG_TAIL  32768
static const char *g_fp_log_path = "/data/vendor/fpdump/fp_log.txt";
static void session_pick_log(void) {
    const char *best = NULL;
    struct stat best_st;

    memset(&best_st, 0, sizeof(best_st));

    for (int i = 0; HAL_FP_LOG_PATHS[i]; ++i) {
        struct stat st;

        if (stat(HAL_FP_LOG_PATHS[i], &st) != 0)
            continue;

        if (!best || st.st_mtime > best_st.st_mtime) {
            best = HAL_FP_LOG_PATHS[i];
            best_st = st;
        }
    }

    if (best)
        g_fp_log_path = best;
}
static void force_finger_up(void);
static void session_set_state(int armed, const char *why, int announce) {
    int changed;
    pthread_mutex_lock(&g_state_lock);
    changed = (g_session_armed != armed) || !g_session_known;
    g_session_armed = armed;
    g_session_known = 1;
    if (!armed)
        g_in_fod_area = 0;
    pthread_mutex_unlock(&g_state_lock);
    if (!changed)
        return;
    if (announce)
        LOGI("FOD session %s (%s)", armed ? "ARMED" : "IDLE", why);
    if (announce && !armed)
        force_finger_up();
}
static void session_invalidate(const char *why, int announce) {
    pthread_mutex_lock(&g_state_lock);
    g_session_known = 0;
    pthread_mutex_unlock(&g_state_lock);
    if (announce)
        LOGI("FOD session state unknown (%s)", why);
}
#define SESSION_SEED_MAX_AGE_US (120ULL * 1000000ULL)
static uint64_t g_session_last_marker_us = 0;
static int g_seed_seen_marker = 0;
static uint64_t realtime_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static int parse_digits(const char **p, char stop) {
    int v = 0;
    while (**p && **p != stop) {
        if (**p < '0' || **p > '9')
            return -1;
        v = v * 10 + (**p - '0');
        (*p)++;
    }
    return v;
}
static uint64_t session_line_epoch_us(const char *line) {
    const char *p = line;
    struct tm tm;
    int Y, M, D, h, m, s = 0, us = 0;
    if (strncmp(p, "time(", 5) != 0)
        return 0;
    p += 5;
    if ((Y = parse_digits(&p, '-')) < 0 || *p++ != '-') return 0;
    if ((M = parse_digits(&p, '-')) < 0 || *p++ != '-') return 0;
    if ((D = parse_digits(&p, '-')) < 0 || *p++ != '-') return 0;
    if ((h = parse_digits(&p, ':')) < 0) return 0;
    if (*p == ':') {
        p++;
        if ((m = parse_digits(&p, ':')) < 0) return 0;
        if (*p == ':') {
            p++;
            if ((s = parse_digits(&p, '.')) < 0) return 0;
        }
    } else {
        return 0;
    }
    if (*p == '.') {
        p++;
        for (int i = 0; i < 6 && *p >= '0' && *p <= '9'; ++i, ++p)
            us = us * 10 + (*p - '0');
    }
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = Y - 1900;
    tm.tm_mon = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = s;
    time_t t = mktime(&tm);
    if (t < 0)
        return 0;
    return (uint64_t)t * 1000000ULL + (uint64_t)us;
}
static void session_feed_line(const char *line, int announce) {
    uint64_t ts = session_line_epoch_us(line);
    if (strstr(line, "start authenticate")) {
        g_session_last_marker_us = ts;
        g_seed_seen_marker = 1;
        session_set_state(1, "HAL authenticate", announce);
        return;
    }
    if (strstr(line, "auth:enter idle status") ||
        strstr(line, "On Error Canceled,enter idle status")) {
        g_session_last_marker_us = ts;
        g_seed_seen_marker = 1;
        session_set_state(0, "HAL idle", announce);
        return;
    }
    if (strstr(line, "HAL initialized successfully") ||
        strstr(line, "Opening fingerprint hal library")) {
        if (announce)
            LOGD("session: HAL restarted, keeping state %s", !g_session_known ? "UNKNOWN" : (g_session_armed ? "ARMED" : "IDLE"));
    }
}
static void session_feed_bytes(const char *buf, size_t len, int announce) {
    char line[512];
    size_t li = 0;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] == '\n') {
            line[li] = 0;
            if (li)
                session_feed_line(line, announce);
            li = 0;
        } else if (li + 1 < sizeof(line)) {
            line[li++] = buf[i];
        }
    }
}
static off_t session_log_seed(int fd) {
    char buf[4096];
    off_t end = lseek(fd, 0, SEEK_END);

    g_seed_seen_marker = 0;
    off_t pos = (end > HAL_FP_LOG_TAIL) ? end - HAL_FP_LOG_TAIL : 0;
    while (pos < end) {
        ssize_t n = pread(fd, buf, sizeof(buf), pos);
        if (n <= 0)
            break;
        session_feed_bytes(buf, (size_t)n, 0);
        pos += n;
    }
    if (!g_session_known && !g_seed_seen_marker)
        session_set_state(0, "no marker yet", 0);

    if (g_session_known && g_session_last_marker_us) {
        uint64_t now = realtime_us();
        if (now > g_session_last_marker_us &&
            now - g_session_last_marker_us > SESSION_SEED_MAX_AGE_US)
            session_set_state(0, "stale seed", 0);
    }
    return end;
}
static void session_log_read_new(int fd, off_t *off) {
    char buf[4096];
    off_t end = lseek(fd, 0, SEEK_END);
    if (end < *off) {
        *off = session_log_seed(fd);
        return;
    }
    while (*off < end) {
        ssize_t n = pread(fd, buf, sizeof(buf), *off);
        if (n <= 0)
            break;
        session_feed_bytes(buf, (size_t)n, 1);
        *off += n;
    }
}
static int session_open_log(int ifd, int *wd_file, off_t *off) {
    session_pick_log();
    int fd = open(g_fp_log_path, O_RDONLY);
    if (fd < 0)
        return -1;
    *wd_file = inotify_add_watch(ifd, g_fp_log_path, IN_MODIFY);
    *off = session_log_seed(fd);
    return fd;
}
static void *session_log_thread(void *arg) {
    (void)arg;
    int ifd = inotify_init1(IN_NONBLOCK);
    if (ifd < 0) {
        LOGE("session: inotify_init1 failed (errno=%d)", errno);
        return NULL;
    }
    for (int i = 0; HAL_FP_LOG_DIRS[i]; ++i)
        inotify_add_watch(ifd, HAL_FP_LOG_DIRS[i],
                          IN_CREATE | IN_MOVED_TO | IN_DELETE);
    int lfd = -1;
    int wd_file = -1;
    off_t off = 0;
    lfd = session_open_log(ifd, &wd_file, &off);
    if (lfd >= 0) {
        LOGI("session monitor active (%s, offset %lld, state=%s)",
             g_fp_log_path, (long long)off,
             !g_session_known ? "UNKNOWN" : (g_session_armed ? "ARMED" : "IDLE"));
    } else {
        LOGW("session monitor: %s missing - assuming ARMED", g_fp_log_path);
    }
    while (g_running) {
        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = ifd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 1000);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ret > 0 && (pfd.revents & POLLIN)) {
            char evbuf[1024];
            ssize_t n = read(ifd, evbuf, sizeof(evbuf));
            size_t i = 0;
            while (n > 0 && i + sizeof(struct inotify_event) <= (size_t)n) {
                struct inotify_event *e =
                    (struct inotify_event *)(evbuf + i);
                i += sizeof(struct inotify_event) + e->len;
                if (e->wd == wd_file &&
                    (e->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))) {
                    close(lfd);
                    lfd = -1;
                    wd_file = -1;
                    session_invalidate("log removed", 1);
                } else if (e->len && strstr(e->name, ".txt") &&
                           (e->mask & (IN_CREATE | IN_MOVED_TO))) {
                    if (lfd >= 0)
                        close(lfd);
                    lfd = session_open_log(ifd, &wd_file, &off);
                    if (lfd >= 0)
                        LOGI("session: reopened %s", g_fp_log_path);
                }
            }
        }
        if (lfd >= 0)
            session_log_read_new(lfd, &off);
        else
            lfd = session_open_log(ifd, &wd_file, &off);
    }
    if (lfd >= 0)
        close(lfd);
    close(ifd);
    return NULL;
}
#define HBM_NOTIFY_PATH "/sys/fingerprint/hbm_notify_enable"
static int read_hbm_notify_state(void) {
    int fd = open(HBM_NOTIFY_PATH, O_RDONLY);
    if (fd < 0)
        return -1;
    char buf[8] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    return atoi(buf);
}
static int fod_session_armed(void) {
    return g_session_known ? g_session_armed : 1;
}
static void force_finger_up(void) {
    int was_down;
    pthread_mutex_lock(&g_state_lock);
    was_down = g_finger_down_state;
    g_finger_down_state = 0;
    g_hbm_on_timestamp_ms = 0;
    pthread_cond_signal(&g_timer_cond);
    pthread_mutex_unlock(&g_state_lock);
    if (!was_down)
        return;
    send_extcmd_sequence(0, 0, 0);
    set_local_hbm(LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP);
    write_node(g_fod_press_node, "0");
    inject_fod_key(0);
}
#define XTOUCH_DEV "/dev/xiaomi-touch"
#define XTOUCH_IOC_SET_CUR_VALUE 0x400c5400UL
#define XTOUCH_MODE_FOD_ENABLE 10
#define XTOUCH_MODE_DTW_ENABLE 14
struct xtouch_data {
    uint32_t touchId;
    uint32_t mode;
    uint32_t value;
};

static void xtouch_set_mode(uint32_t mode, uint32_t value) {
    int fd = open(XTOUCH_DEV, O_RDWR);

    if (fd < 0) {
        LOGE("open %s failed (errno=%d)", XTOUCH_DEV, errno);
        return;
    }
    struct xtouch_data d = { 0, mode, value };
    if (ioctl(fd, XTOUCH_IOC_SET_CUR_VALUE, &d) < 0)
        LOGE("xtouch mode=%u value=%u failed (errno=%d)",
             mode, value, errno);
    else
        LOGI("xtouch mode=%u value=%u", mode, value);

    close(fd);
}
static void set_touch_fod(int on) {
    xtouch_set_mode(XTOUCH_MODE_FOD_ENABLE, 1);
    xtouch_set_mode(XTOUCH_MODE_DTW_ENABLE, 1);
}
static void restore_touch_fod(void)
{
    LOGI("Restoring XTouch FOD state");
    xtouch_set_mode(XTOUCH_MODE_FOD_ENABLE, 1);
    xtouch_set_mode(XTOUCH_MODE_DTW_ENABLE, 1);
}
static void *lcm_uevent_thread(void *arg) {
    (void)arg;
    int fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        LOGE("uevent socket failed (errno=%d)", errno);
        return NULL;
    }
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_nl snl;
    memset(&snl, 0, sizeof(snl));
    snl.nl_family = AF_NETLINK;
    snl.nl_groups = 1;
    if (bind(fd, (struct sockaddr *)&snl, sizeof(snl)) < 0) {
        LOGE("uevent bind failed (errno=%d)", errno);
        close(fd);
        return NULL;
    }
    LOGI("LCM uevent listener active");
    char buf[4096];
    while (g_running) {
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            usleep(100000);
            continue;
        }
        buf[n] = '\0';
        char node[32] = {0};
        char value[32] = {0};
        const char *p = buf;
        if (*p)
            p += strlen(p) + 1;
        while (p < buf + n) {
            if (strncmp(p, "LCM_NODE=", 9) == 0)
                snprintf(node, sizeof(node), "%s", p + 9);
            else if (strncmp(p, "LCM_VALUE=", 10) == 0)
                snprintf(value, sizeof(value), "%s", p + 10);

            p += strlen(p) + 1;
        }
        if (!node[0])
            continue;
        LOGD("uevent %s=%s", node, value);
        if (strcmp(node, "oled_hbm") == 0) {
            if (strcmp(value, "11") == 0) {
                LOGI("oled_hbm=11 (off) -> FOD off");
                force_finger_up();
                if (read_hbm_notify_state() == 1) {
                    LOGI("HBM still enabled -> restoring XTouch");
                    restore_touch_fod();
                }
            }
        } else if (strcmp(node, "hbm_notify_enable") == 0) {
            set_touch_fod(strcmp(value, "1") == 0);
        }
    }
    close(fd);
    return NULL;
}
static void on_finger_state_change(int down, int x, int y) {
    if (down && !fod_session_armed()) {
        LOGD("Ignoring touch in FOD area: no active biometric session");
        return;
    }
    pthread_mutex_lock(&g_state_lock);
    if (g_finger_down_state == down) {
        pthread_mutex_unlock(&g_state_lock);
        return;
    }
    g_finger_down_state = down;
    if (down) {
        g_hbm_on_timestamp_ms = get_time_ms();
        pthread_cond_signal(&g_timer_cond);
        pthread_mutex_unlock(&g_state_lock);
        LOGI(">>> FINGER DOWN at (raw %d, %d) - HBM ON", x, y);
        set_local_hbm(LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT);
        write_node(g_fod_press_node, "1");
        send_extcmd_sequence(1, x, y);
        inject_fod_key(1);

    } else {
        g_hbm_on_timestamp_ms = 0;
        pthread_cond_signal(&g_timer_cond);
        pthread_mutex_unlock(&g_state_lock);
        LOGI("<<< FINGER UP - HBM OFF");
        send_extcmd_sequence(0, 0, 0);
        set_local_hbm(LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP);
        write_node(g_fod_press_node, "0");
        inject_fod_key(0);
    }
}
static void *hbm_safety_watchdog_thread(void *arg) {
    (void)arg;
    LOGI("HBM safety watchdog thread active (hard limit %d ms)",
         MAX_HBM_TIMEOUT_MS);
    pthread_mutex_lock(&g_state_lock);
    while (g_running) {
        if (!g_finger_down_state) {
            pthread_cond_wait(&g_timer_cond, &g_state_lock);
            continue;
        }
        uint64_t now = get_time_ms();
        uint64_t elapsed =
            (now >= g_hbm_on_timestamp_ms)
                ? (now - g_hbm_on_timestamp_ms)
                : 0;
        if (elapsed >= MAX_HBM_TIMEOUT_MS) {
            LOGW("!!! HBM TIMEOUT (%llu ms)! Force-off to prevent burn-in !!!", (unsigned long long)elapsed);
            g_finger_down_state = 0;
            g_hbm_on_timestamp_ms = 0;
            pthread_mutex_unlock(&g_state_lock);
            set_local_hbm(LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP);
            write_node(g_fod_press_node, "0");
            send_extcmd_sequence(0, 0, 0);
            inject_fod_key(0);
            pthread_mutex_lock(&g_state_lock);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t wait_ms = MAX_HBM_TIMEOUT_MS - elapsed;
            ts.tv_sec += wait_ms / 1000;
            ts.tv_nsec += (long)((wait_ms % 1000) * 1000000);
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&g_timer_cond, &g_state_lock, &ts);
        }
    }
    pthread_mutex_unlock(&g_state_lock);
    return NULL;
}
static void handle_touch_event(const struct input_event *ev, int *cur_x, int *cur_y, int *tracking_id) {
    if (ev->type == EV_KEY && ev->code == 0x152) {
        int cx = (g_fod_x_min + g_fod_x_max) / 2;
        int cy = (g_fod_y_min + g_fod_y_max) / 2;
        if (ev->value == 1) {
            LOGD("SOFOD key down (0x152)");
            if (fod_session_armed()) {
                on_finger_state_change(1, cx, cy);
                g_in_fod_area = 1;
            } else {
                LOGD("Ignoring SOFOD key: no active biometric session");
                force_finger_up();
            }
        } else if (ev->value == 0) {
            LOGD("SOFOD key up (0x152)");
            g_in_fod_area = 0;
            on_finger_state_change(0, 0, 0);
        }
        return;
    }
    if (ev->type == EV_ABS) {
        if (ev->code == ABS_MT_POSITION_X) {
            *cur_x = ev->value;
        } else if (ev->code == ABS_MT_POSITION_Y) {
            *cur_y = ev->value;
        } else if (ev->code == ABS_MT_TRACKING_ID) {
            *tracking_id = ev->value;
            if (*tracking_id == -1) {
                if (g_in_fod_area) {
                    g_in_fod_area = 0;
                    on_finger_state_change(0, 0, 0);
                }
                *cur_x = -1;
                *cur_y = -1;
            }
        }
        return;
    }
    if (ev->type != EV_SYN || ev->code != SYN_REPORT)
        return;
    if (*tracking_id == -1 || *cur_x < 0 || *cur_y < 0)
        return;
    if (*cur_x >= g_fod_x_min &&
        *cur_x <= g_fod_x_max &&
        *cur_y >= g_fod_y_min &&
        *cur_y <= g_fod_y_max) {
        if (!g_in_fod_area) {
            g_in_fod_area = 1;
            LOGD("Touch inside FOD boundary (raw): X=%d, Y=%d",
                 *cur_x,
                 *cur_y);
            on_finger_state_change(1, *cur_x, *cur_y);
        }
    } else {
        if (g_in_fod_area) {
            g_in_fod_area = 0;
            LOGD("Touch slid out of FOD boundary (raw): X=%d, Y=%d",
                 *cur_x,
                 *cur_y);
            on_finger_state_change(0, 0, 0);
        }
    }
}
static int find_touch_device(void) {
    char input_path[64];
    for (int i = 0; i < 15; ++i) {
        snprintf(input_path, sizeof(input_path), "/dev/input/event%d", i);
        int fd = open(input_path, O_RDONLY);
        if (fd < 0)
            continue;
        char name[256] = "Unknown";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        if (strstr(name, "touch") ||
            strstr(name, "fts") ||
            strstr(name, "Goodix") ||
            strstr(name, "synaptics")) {
            LOGI("Found touch device: %s (%s)", input_path, name);
            return fd;
        }
        close(fd);
    }
    return -1;
}
static void *touch_monitor_thread(void *arg) {
    (void)arg;
    LOGI("Touch monitor thread started.");
    struct input_event ev;
    while (g_running) {
        int touch_fd = find_touch_device();
        if (touch_fd < 0) {
            LOGE("Failed to open touch event device - retrying");
            sleep(1);
            continue;
        }
        int cur_x = -1;
        int cur_y = -1;
        int tracking_id = -1;
        while (g_running) {
            struct pollfd tpfd;

            memset(&tpfd, 0, sizeof(tpfd));
            tpfd.fd = touch_fd;
            tpfd.events = POLLIN;

            int pr = poll(&tpfd, 1, 200);

            if (pr < 0) {
                if (errno == EINTR)
                    continue;
                LOGW("touch poll failed (errno=%d) - reopening", errno);
                break;
            }

            if (pr == 0 || !(tpfd.revents & POLLIN))
                continue;

            ssize_t n = read(touch_fd, &ev, sizeof(ev));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                LOGW("touch read failed (errno=%d) - reopening", errno);
                break;
            }
            if (n != (ssize_t)sizeof(ev))
                continue;
            handle_touch_event(&ev, &cur_x, &cur_y, &tracking_id);
        }
        close(touch_fd);
        g_in_fod_area = 0;
        force_finger_up();

        if (!g_running)
            break;

        sleep(1);
    }
    return NULL;
}
static void sig_handler(int sig) {
    (void)sig;

    g_running = 0;
    pthread_cond_signal(&g_timer_cond);
}
#define FOD_CTRL_FIFO "/data/local/tmp/.fod_ctrl"
static void process_control_line(char *line) {
    int cmd = 0;
    int param = 0;
    char *sp = strchr(line, ' ');
    if (sp) {
        *sp = 0;
        cmd = atoi(line);
        param = atoi(sp + 1);
        if (cmd >= 0) {
            LOGI("CTRL: native extCmd cmd=%d param=%d", cmd, param);
            native_extcmd((uint32_t)cmd, (uint32_t)param);
            return;
        }
    }
    if (strncmp(line, "seq_down", 8) == 0) {
        LOGI("CTRL: inject V6 down sequence");
        send_extcmd_sequence(1, 5400, 21500);
        return;
    }
    if (strncmp(line, "seq_up", 6) == 0) {
        LOGI("CTRL: inject V6 up sequence");
        send_extcmd_sequence(0, 0, 0);
        return;
    }
    if (strncmp(line, "keydown", 7) == 0) {
        LOGI("CTRL: inject FOD key DOWN");
        inject_fod_key(1);
        return;
    }
    if (strncmp(line, "keyup", 5) == 0) {
        LOGI("CTRL: inject FOD key UP");
        inject_fod_key(0);
        return;
    }
    if (strncmp(line, "session", 7) == 0) {
        LOGI("CTRL: session state %s",
             fod_session_armed() ? "ARMED" : "IDLE");
        return;
    }
}
static void *control_fifo_thread(void *arg) {
    (void)arg;
    unlink(FOD_CTRL_FIFO);
    if (mkfifo(FOD_CTRL_FIFO, 0666) < 0) {
        LOGE("mkfifo %s failed (errno=%d)", FOD_CTRL_FIFO, errno);
        return NULL;
    }
    chmod(FOD_CTRL_FIFO, 0666);
    LOGI("Control FIFO ready at %s", FOD_CTRL_FIFO);
    int fd = open(FOD_CTRL_FIFO, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        LOGE("open %s failed (errno=%d)", FOD_CTRL_FIFO, errno);
        unlink(FOD_CTRL_FIFO);
        return NULL;
    }
    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = POLLIN;
    char line[64];
    while (g_running) {
        int ret = poll(&pfd, 1, 500);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ret == 0)
            continue;
        if (!(pfd.revents & (POLLIN | POLLERR | POLLHUP)))
            continue;
        ssize_t n = read(fd, line, sizeof(line) - 1);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
        line[n] = '\0';
        char *save = NULL;
        char *part = strtok_r(line, "\r\n", &save);
        while (part) {
            process_control_line(part);
            part = strtok_r(NULL, "\r\n", &save);
        }
    }
    close(fd);
    unlink(FOD_CTRL_FIFO);
    return NULL;
}
int main(int argc, char **argv) {
    init_identity();
    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGPIPE, SIG_IGN);
    if (argc >= 2 && strcmp(argv[1], "--session-watch") == 0) {
        g_debug = 1;
        LOGI("session monitor probe - Ctrl-C to stop");
        session_log_thread(NULL);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--test-hbm") == 0) {
        int value = 2;
        int repeat = 1;
        if (argc >= 3)
            value = atoi(argv[2]);
        if (argc >= 4)
            repeat = atoi(argv[3]);
        g_disp_feature_fd = open_disp_feature();
        if (g_disp_feature_fd < 0)
            return 1;
        LOGI("Running HBM ioctl probe: value=%d x%d", value, repeat);
        for (int i = 0; i < repeat; i++) {
            set_local_hbm(value);
            usleep(200000);
        }
        set_local_hbm(LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP);
        LOGI("HBM probe finished safely");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--extcmd") == 0) {
        if (native_hidl_init() != 0) {
            LOGE("native HIDL init failed");
            return 1;
        }
        for (int i = 2; i + 1 <= argc - 1; i += 2) {
            uint32_t cmd = (uint32_t)atoi(argv[i]);
            uint32_t param = (uint32_t)atoi(argv[i + 1]);
            LOGI("extCmd %u %u", cmd, param);
            native_extcmd(cmd, param);
        }
        return 0;
    }
    if (native_hidl_init() != 0) {
        LOGE("native HIDL init failed - bridge cannot talk to mfp-daemon");
        return 1;
    }
    g_fod_press_node = probe_first_existing(FOD_PRESS_STATUS_PATHS);
    LOGI("Using FOD press status node : %s",
         g_fod_press_node ? g_fod_press_node : "MISSING");
    load_fod_area_from_kernel();
    g_disp_feature_fd = open_disp_feature();
    if (g_disp_feature_fd < 0)
        LOGW("HBM ioctl interface unavailable - bridge will run degraded");
    g_uinput_fd = init_uinput_key();
    set_touch_fod(read_hbm_notify_state() == 1);
    if (g_fod_press_node)
        chmod(g_fod_press_node, 0666);
    pthread_t touch_tid;
    pthread_t watchdog_tid;
    pthread_t ctrl_tid;
    pthread_t uevent_tid;
    pthread_t session_tid;
    if (pthread_create(&session_tid, NULL, session_log_thread, NULL) != 0) {
        LOGE("Failed to create session thread");
        return 1;
    }
    if (pthread_create(&watchdog_tid, NULL, hbm_safety_watchdog_thread, NULL) != 0) {
        LOGE("Failed to create watchdog thread");
        return 1;
    }
    if (pthread_create(&touch_tid, NULL, touch_monitor_thread, NULL) != 0) {
        LOGE("Failed to create touch thread");
        g_running = 0;
        pthread_cond_signal(&g_timer_cond);
        pthread_join(watchdog_tid, NULL);
        return 1;
    }
    if (pthread_create(&ctrl_tid, NULL, control_fifo_thread, NULL) != 0) {
        LOGE("Failed to create control thread");
        g_running = 0;
        pthread_cond_signal(&g_timer_cond);
        pthread_join(touch_tid, NULL);
        pthread_join(watchdog_tid, NULL);
        return 1;
    }
    if (pthread_create(&uevent_tid, NULL, lcm_uevent_thread, NULL) != 0) {
        LOGE("Failed to create uevent thread");
        g_running = 0;
        pthread_cond_signal(&g_timer_cond);
        pthread_join(touch_tid, NULL);
        pthread_join(watchdog_tid, NULL);
        pthread_join(ctrl_tid, NULL);
        return 1;
    }
    pthread_join(touch_tid, NULL);
    g_running = 0;
    pthread_cond_signal(&g_timer_cond);
    pthread_join(watchdog_tid, NULL);
    pthread_join(ctrl_tid, NULL);
    pthread_join(uevent_tid, NULL);
    pthread_join(session_tid, NULL);
    force_finger_up();
    if (g_disp_feature_fd >= 0) {
        close(g_disp_feature_fd);
        g_disp_feature_fd = -1;
    }
    if (g_uinput_fd >= 0) {
        ioctl(g_uinput_fd, UI_DEV_DESTROY);
        close(g_uinput_fd);
        g_uinput_fd = -1;
    }
    unlink(FOD_CTRL_FIFO);
    return 0;
}
