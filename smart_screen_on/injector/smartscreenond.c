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
#include <android/log.h>
#define LOG_TAG "smartscreenond"
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

static int find_process(const char* name) {
    DIR* proc = opendir("/proc");
    if (proc == NULL) return -1;
    struct dirent* entry;
    int found = -1;
    while ((entry = readdir(proc)) != NULL) {
        if (entry->d_type != DT_DIR) continue;
        int pid = atoi(entry->d_name);
        if (pid <= 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char cmdline[256];
        ssize_t len = read(fd, cmdline, sizeof(cmdline) - 1);
        close(fd);
        if (len <= 0) continue;
        cmdline[len] = '\0';
        if (strcmp(cmdline, name) == 0) { found = pid; break; }
    }
    closedir(proc);
    return found;
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
    if (waitpid(pid, &status, 0) < 0) { LOGE("waitpid: %s", strerror(errno)); return 0; }
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
    waitpid(pid, &status, 0);
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
        waitpid(pid, &st, 0);
        struct user_pt_regs64 s2;
        if (get_regs(pid, &s2) == 0) {
            const char* payload = paths[path_count - 1];
            const char* slash = strrchr(payload, '/');
            const char* base_name = slash ? slash + 1 : payload;
            char payload_path[512] = {0};
            uint64_t base = lib_base(pid, base_name, payload_path, sizeof(payload_path));
            uint64_t off = elf_symbol_offset(payload, "smartscreenon_start");
            if (base != 0 && off != 0) {
                uint64_t fn = base + off;
                LOGI("calling smartscreenon_start @ 0x%llx (base 0x%llx)", (unsigned long long)fn,
                     (unsigned long long)base);
                remote_call(pid, &s2, fn, NULL, 0);
            } else {
                LOGE("smartscreenon_start not found (base=0x%llx off=0x%llx)",
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
static long long proc_age_ms(int pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE* f = fopen(path, "re");
    if (f == NULL) return -1;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return -1;
    buf[n] = '\0';
    char* p = strrchr(buf, ')');
    if (p == NULL) return -1;
    p++;
    unsigned long long starttime = 0;
    for (int i = 0; i < 20; i++) {
        while (*p == ' ') p++;
        if (*p == '\0') return -1;
        if (i == 0) {
            while (*p != ' ' && *p != '\0') p++;
            continue;
        }
        char* end = NULL;
        unsigned long long v = strtoull(p, &end, 10);
        if (end == p) return -1;
        if (i == 19) { starttime = v; break; }
        p = end;
    }
    FILE* up = fopen("/proc/uptime", "re");
    if (up == NULL) return -1;
    double uptime = 0;
    if (fscanf(up, "%lf", &uptime) != 1) { fclose(up); return -1; }
    fclose(up);
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    long long age = (long long)(uptime * 1000.0) - (long long)(starttime * 1000ULL / (unsigned long long)hz);
    return age < 0 ? 0 : age;
}
static int lib_loaded(int pid, const char* needle) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* maps = fopen(path, "re");
    if (maps == NULL) return 0;
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), maps) != NULL) {
        if (strstr(line, needle) != NULL) { found = 1; break; }
    }
    fclose(maps);
    return found;
}
#define MIN_PROC_AGE_MS 60
int main(int argc, char** argv) {
    const char* target = argc > 1 ? argv[1] : "com.bbk.facewake";
    if (argc < 3) {
        LOGE("usage: %s [target] lib [lib...]", argv[0]);
        return 1;
    }
    int hooked_pid = -1;
    LOGI("smartscreenond watching %s (%d libs)", target, argc - 2);
    for (;;) {
        if (hooked_pid > 0) {
            if (pid_alive(hooked_pid)) {
                usleep(500000);
                continue;
            }
            LOGI("pid %d gone, waiting for a new one", hooked_pid);
            hooked_pid = -1;
        }
        int pid = find_process(target);
        if (pid > 0) {
            long long age = proc_age_ms(pid);
            if (age < 0) { usleep(10000); continue; }
            if (age < MIN_PROC_AGE_MS) { usleep(5000); continue; }
            if (!pid_alive(pid)) { usleep(10000); continue; }
            if (!lib_loaded(pid, "libart.so")) { usleep(10000); continue; }
            LOGI("pid %d age %lldms, injecting", pid, age);
            int rc = inject_libraries(pid, &argv[2], argc - 2);
            if (rc == 0) {
                hooked_pid = pid;
                LOGI("injected pid %d", pid);
            } else {
                LOGE("injection failed for pid %d, will retry", pid);
                usleep(50000);
            }
        } else {
            usleep(20000);
        }
    }
    return 0;
}
