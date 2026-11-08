// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <elf.h>
#include <stdint.h>
#include <time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <linux/netlink.h>
#include <linux/connector.h>
#include <linux/cn_proc.h>
#include <android/log.h>
#define LOG_TAG "abouthookd"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define RTLD_NOW 2
#define ANDROID_DLEXT_USE_LIBRARY_FD 0x10ULL
#define SYS_OPENAT 56
#define AT_FDCWD (-100)
struct user_pt_regs64 { uint64_t regs[31]; uint64_t sp; uint64_t pc; uint64_t pstate; };
struct dlextinfo {
    uint64_t flags;
    uint64_t reserved_addr;
    uint64_t reserved_size;
    int32_t relro_fd;
    int32_t library_fd;
    uint64_t library_fd_offset;
    uint64_t library_namespace;
};
#define NETLINK_CONNECTOR 11
#define PENDING_MAX 8
#define PENDING_TTL_NS (3LL * 1000000000LL)
#define PENDING_FIRST_CHECK_NS (20LL * 1000000LL)
#define PENDING_RECHECK_NS (100LL * 1000000LL)
#define FULL_RESCAN_NS (30LL * 1000000000LL)
#define IDLE_SLEEP_MIN_US 20000
#define IDLE_SLEEP_MAX_US 250000
#define ATTACH_TIMEOUT_MS 5000
#define SAFETY_SCAN_NS (5LL * 1000000000LL)

static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static int cmdline_is(int pid, const char* name) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char cmdline[256];
    ssize_t len = read(fd, cmdline, sizeof(cmdline) - 1);
    close(fd);
    if (len <= 0) return 0;
    cmdline[len] = '\0';
    return strcmp(cmdline, name) == 0;
}
static int is_frozen(int pid) {
    char path[64], buf[64];
    snprintf(path, sizeof(path), "/proc/%d/wchan", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return strstr(buf, "freezer") != NULL;
}
static int wait_stopped(int pid, int* status_out) {
    for (int waited_ms = 0; waited_ms < ATTACH_TIMEOUT_MS; waited_ms += 10) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) { *status_out = st; return 1; }
        if (r < 0 && errno == ECHILD) return 0;
        usleep(10000);
    }
    return 0;
}
static int gLastPid = 0;
static int64_t gLastFullScanNs = 0;
static int find_process(const char* name, int reset_on_match) {
    const int64_t now = mono_ns();
    const int full = (now - gLastFullScanNs) >= FULL_RESCAN_NS;
    if (full) gLastFullScanNs = now;
    DIR* proc = opendir("/proc");
    if (proc == NULL) return -1;
    struct dirent* entry;
    int found = -1;
    while ((entry = readdir(proc)) != NULL) {
        if (entry->d_type != DT_DIR) continue;
        char* end = NULL;
        long pid = strtol(entry->d_name, &end, 10);
        if (pid <= 0 || end == entry->d_name || *end != '\0') continue;
        if (!full && pid <= gLastPid) continue;
        if (pid > gLastPid) gLastPid = (int)pid;
        if (cmdline_is((int)pid, name)) { found = (int)pid; break; }
    }
    closedir(proc);
    if (found > 0) {
        gLastPid = 0;
        gLastFullScanNs = 0;
    }
    return found;
}
static int gCnFd = -1;
static struct { int pid; int64_t deadline_ns; int64_t next_check_ns; } gPending[PENDING_MAX];

static int cn_open(void) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR);
    if (fd < 0) return -1;
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_pid = (unsigned)getpid();
    sa.nl_groups = CN_IDX_PROC;
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) { close(fd); return -1; }
    struct {
        struct nlmsghdr nl;
        struct cn_msg cn;
        int op;
    } __attribute__((packed)) req;
    memset(&req, 0, sizeof(req));
    req.nl.nlmsg_len = NLMSG_LENGTH(sizeof(struct cn_msg) + sizeof(int));
    req.nl.nlmsg_type = NLMSG_DONE;
    req.cn.id.idx = CN_IDX_PROC;
    req.cn.id.val = CN_VAL_PROC;
    req.cn.len = sizeof(int);
    req.op = PROC_CN_MCAST_LISTEN;
    memcpy(req.cn.data, &req.op, sizeof(int));
    struct sockaddr_nl dst;
    memset(&dst, 0, sizeof(dst));
    dst.nl_family = AF_NETLINK;
    if (sendto(fd, &req, req.nl.nlmsg_len, 0, (struct sockaddr*)&dst, sizeof(dst)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}
static void pending_add(int pid) {
    if (pid <= 0) return;
    const int64_t dl = mono_ns() + PENDING_TTL_NS;
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < PENDING_MAX; i++) {
        if (gPending[i].pid == pid) { gPending[i].deadline_ns = dl; return; }
        if (gPending[i].pid == 0 && free_slot < 0) free_slot = i;
        if (gPending[i].deadline_ns < gPending[oldest].deadline_ns) oldest = i;
    }
    int slot = (free_slot >= 0) ? free_slot : oldest;
    gPending[slot].pid = pid;
    gPending[slot].deadline_ns = dl;
    gPending[slot].next_check_ns = mono_ns() + PENDING_FIRST_CHECK_NS;
}
static void pending_drop(int pid) {
    for (int i = 0; i < PENDING_MAX; i++) if (gPending[i].pid == pid) gPending[i].pid = 0;
}
static void cn_drain(int timeout_ms) {
    if (gCnFd < 0) return;
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(gCnFd, &rf);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    if (select(gCnFd + 1, &rf, NULL, NULL, &tv) <= 0) return;
    static char buf[8192];
    ssize_t n = recv(gCnFd, buf, sizeof(buf), 0);
    if (n <= 0) return;
    struct nlmsghdr* nl = (struct nlmsghdr*)buf;
    for (; NLMSG_OK(nl, (unsigned)n); nl = NLMSG_NEXT(nl, n)) {
        if (nl->nlmsg_type != NLMSG_DONE) continue;
        struct cn_msg* cn = (struct cn_msg*)NLMSG_DATA(nl);
        struct proc_event* ev = (struct proc_event*)cn->data;
        switch (ev->what) {
            case PROC_EVENT_FORK:
                pending_add(ev->event_data.fork.child_pid);
                break;
            case PROC_EVENT_EXEC:
                pending_add(ev->event_data.exec.process_pid);
                break;
            case PROC_EVENT_EXIT:
                pending_drop(ev->event_data.exit.process_pid);
                break;
            default:
                break;
        }
    }
}
static int pending_pick(const char* name) {
    const int64_t now = mono_ns();
    for (int i = 0; i < PENDING_MAX; i++) {
        if (gPending[i].pid == 0) continue;
        if (now > gPending[i].deadline_ns) { gPending[i].pid = 0; continue; }
        if (now < gPending[i].next_check_ns) continue;
        if (cmdline_is(gPending[i].pid, name)) return gPending[i].pid;
        gPending[i].next_check_ns = now + PENDING_RECHECK_NS;
    }
    return -1;
}

static uint64_t lib_base(int pid, const char* needle, char* path_out, size_t path_len) {
    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    FILE* maps = fopen(maps_path, "re");
    if (maps == NULL) return 0;
    char line[1024];
    char best_path[512] = {0};
    uint64_t best_start = 0;
    while (fgets(line, sizeof(line), maps) != NULL) {
        unsigned long long start = 0, end = 0;
        char perms[8] = {0};
        char path[512] = {0};
        if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %511s", &start, &end, perms, path) != 4) continue;
        if (strstr(path, needle) == NULL) continue;
        if (best_start == 0 || start < best_start) {
            best_start = start;
            snprintf(best_path, sizeof(best_path), "%s", path);
        }
    }
    fclose(maps);
    if (best_start == 0) return 0;
    snprintf(path_out, path_len, "%s", best_path);
    return best_start;
}
//🐥
static uint64_t elf_symbol_offset(const char* path, const char* symbol) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return 0; }
    size_t size = (size_t)st.st_size;
    uint8_t* data = (uint8_t*)malloc(size);
    if (data == NULL) { close(fd); return 0; }
    if (read(fd, data, size) != (ssize_t)size) { free(data); close(fd); return 0; }
    close(fd);
    uint64_t result = 0;
    Elf64_Ehdr* eh = (Elf64_Ehdr*)data;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64) {
        free(data); return 0;
    }
    Elf64_Shdr* shdrs = (Elf64_Shdr*)(data + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum && result == 0; i++) {
        Elf64_Shdr* sh = &shdrs[i];
        if (sh->sh_type != SHT_DYNSYM || sh->sh_link >= eh->e_shnum) continue;
        Elf64_Shdr* strsh = &shdrs[sh->sh_link];
        Elf64_Sym* syms = (Elf64_Sym*)(data + sh->sh_offset);
        size_t count = sh->sh_size / sizeof(Elf64_Sym);
        const char* strtab = (const char*)(data + strsh->sh_offset);
        for (size_t j = 0; j < count; j++) {
            if (syms[j].st_name == 0) continue;
            if (strcmp(strtab + syms[j].st_name, symbol) == 0) { result = syms[j].st_value; break; }
        }
    }
    free(data);
    return result;
}
static int poke_data(int pid, uint64_t addr, const void* data, size_t len) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) { LOGE("open mem: %s", strerror(errno)); return -1; }
    ssize_t written = pwrite(fd, data, len, (off_t)addr);
    close(fd);
    if (written != (ssize_t)len) { LOGE("pwrite mem: %s", strerror(errno)); return -1; }
    return 0;
}
static int get_regs(int pid, struct user_pt_regs64* regs) {
    struct iovec iov = { regs, sizeof(*regs) };
    return ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov);
}
static int set_regs(int pid, struct user_pt_regs64* regs) {
    struct iovec iov = { regs, sizeof(*regs) };
    return ptrace(PTRACE_SETREGSET, pid, (void*)NT_PRSTATUS, &iov);
}
static uint64_t remote_call(int pid, const struct user_pt_regs64* saved, uint64_t pc, const uint64_t* args, int nargs) {
    struct user_pt_regs64 regs = *saved;
    for (int i = 0; i < nargs && i < 8; i++) regs.regs[i] = args[i];
    regs.regs[30] = 0;
    regs.pc = pc;
    if (set_regs(pid, &regs) != 0) { LOGE("set_regs: %s", strerror(errno)); return 0; }
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) != 0) { LOGE("CONT: %s", strerror(errno)); return 0; }
    int status = 0;
    if (!wait_stopped(pid, &status)) { LOGE("remote call on %d hung", pid); return 0; }
    struct user_pt_regs64 after;
    if (get_regs(pid, &after) != 0) { LOGE("get_regs: %s", strerror(errno)); return 0; }
    set_regs(pid, (struct user_pt_regs64*)saved);
    return after.regs[0];
}

static int inject_libraries(int pid, char** paths, int path_count) {
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0) {
        LOGE("PTRACE_ATTACH %d: %s", pid, strerror(errno));
        return -1;
    }
    int status = 0;
    if (!wait_stopped(pid, &status)) {
        LOGE("target %d did not stop within %dms (frozen?), abandoning", pid,
             ATTACH_TIMEOUT_MS);
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return -1;
    }
    if (!WIFSTOPPED(status)) {
        LOGE("target not stopped after attach");
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return -1;
    }
    struct user_pt_regs64 saved;
    if (get_regs(pid, &saved) != 0) {
        LOGE("get regs: %s", strerror(errno));
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return -1;
    }
    char dl_path[512] = {0};
    uint64_t libdl_base = lib_base(pid, "/libdl.so", dl_path, sizeof(dl_path));
    char libc_path[512] = {0};
    uint64_t libc_base = lib_base(pid, "/bionic/libc.so", libc_path, sizeof(libc_path));
    if (libdl_base == 0 || libc_base == 0) {
        LOGE("libdl/libc not found");
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return -1;
    }
    uint64_t dlopen_ext = libdl_base + elf_symbol_offset(dl_path, "android_dlopen_ext");
    uint64_t syscall_fn = libc_base + elf_symbol_offset(libc_path, "syscall");
    if (dlopen_ext == libdl_base || syscall_fn == libc_base) {
        LOGE("android_dlopen_ext/syscall not found (dl=%s libc=%s)", dl_path, libc_path);
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return -1;
    }
    LOGI("pid %d: libdl=%s dlopen_ext=0x%llx syscall=0x%llx", pid, dl_path,
         (unsigned long long)dlopen_ext, (unsigned long long)syscall_fn);
    uint64_t scratch = (saved.sp - 0x2000) & ~0xFULL;
    for (int i = 0; i < path_count; i++) {
        const char* lib_path = paths[i];
        uint64_t path_addr = scratch + (uint64_t)i * 0x100;
        uint64_t info_addr = scratch + 0x1000;
        if (poke_data(pid, path_addr, lib_path, strlen(lib_path) + 1) != 0) break;
        uint64_t open_args[4] = { SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD, path_addr, 0 };
        uint64_t fd = remote_call(pid, &saved, syscall_fn, open_args, 4);
        if ((int64_t)fd < 0) {
            LOGE("openat(%s) failed: %lld", lib_path, (long long)(int64_t)fd);
            continue;
        }
        struct dlextinfo info;
        memset(&info, 0, sizeof(info));
        info.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
        info.library_fd = (int32_t)fd;
        if (poke_data(pid, info_addr, &info, sizeof(info)) != 0) break;
        uint64_t dl_args[3] = { path_addr, RTLD_NOW, info_addr };
        uint64_t handle = remote_call(pid, &saved, dlopen_ext, dl_args, 3);
        LOGI("dlopen_ext(fd %lld, %s) -> handle=0x%llx", (long long)(int64_t)fd, lib_path,
             (unsigned long long)handle);
    }
    set_regs(pid, &saved);
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    LOGI("injection done for pid %d", pid);
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) == 0) {
        int st = 0;
        if (!wait_stopped(pid, &st)) {
            LOGE("second attach on %d timed out", pid);
            ptrace(PTRACE_DETACH, pid, NULL, NULL);
            return -1;
        }
        struct user_pt_regs64 s2;
        if (get_regs(pid, &s2) == 0) {
            const char* payload = paths[path_count - 1];
            const char* slash = strrchr(payload, '/');
            const char* base_name = slash ? slash + 1 : payload;
            char payload_path[512] = {0};
            uint64_t base = lib_base(pid, base_name, payload_path, sizeof(payload_path));
            uint64_t off = elf_symbol_offset(payload, "abouthook_start");
            if (base != 0 && off != 0) {
                uint64_t fn = base + off;
                LOGI("calling abouthook_start @ 0x%llx (base 0x%llx)", (unsigned long long)fn,
                     (unsigned long long)base);
                remote_call(pid, &s2, fn, NULL, 0);
            } else {
                LOGE("abouthook_start not found (base=0x%llx off=0x%llx)",
                     (unsigned long long)base, (unsigned long long)off);
            }
            set_regs(pid, &s2);
        }
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
    }
    return 0;
}
static int pid_alive(int pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    return access(path, F_OK) == 0;
}
int main(int argc, char** argv) {
    const char* target = argc > 1 ? argv[1] : "com.android.settings";
    if (argc < 3) {
        LOGE("usage: %s [target] lib [lib...]", argv[0]);
        return 1;
    }
    int injected[256] = {0};
    int injected_count = 0;

    gCnFd = cn_open();
    LOGI("abouthookd watching %s (%d libs), process events: %s", target, argc - 2,
         gCnFd >= 0 ? "CN_PROC" : "unavailable, /proc scan fallback");
    int candidate = find_process(target, 1);
    useconds_t idle_us = IDLE_SLEEP_MIN_US;
    int64_t last_scan_ns = mono_ns();

    for (;;) {
        if (candidate <= 0) {
            if (gCnFd >= 0) {
                cn_drain(1000);
                candidate = pending_pick(target);
                if (candidate <= 0) {
                    if (mono_ns() - last_scan_ns >= SAFETY_SCAN_NS) {
                        last_scan_ns = mono_ns();
                        candidate = find_process(target, 0);
                    }
                    if (candidate <= 0) continue;
                }
            } else {
                candidate = find_process(target, 1);
                if (candidate <= 0) {
                    usleep(idle_us);
                    if (idle_us < IDLE_SLEEP_MAX_US) idle_us *= 2;
                    continue;
                }
            }
            idle_us = IDLE_SLEEP_MIN_US;
        }
        int seen = 0;
        for (int i = 0; i < injected_count; i++) {
            if (injected[i] == candidate) { seen = 1; break; }
        }
        if (seen) { candidate = -1; usleep(300000); continue; }
        usleep(500000);
        if (!pid_alive(candidate)) { candidate = -1; continue; }
        if (is_frozen(candidate)) { usleep(100000); continue; }
        int rc = inject_libraries(candidate, &argv[2], argc - 2);
        if (rc == 0) {
            if (injected_count < 256) injected[injected_count++] = candidate;
            LOGI("injected pid %d (%d total)", candidate, injected_count);
            candidate = -1;
        } else {
            LOGE("injection failed for pid %d, will retry", candidate);
            usleep(300000);
        }
    }
    return 0;
}
