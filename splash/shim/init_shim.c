// SPDX-License-Identifier: GPL-2.0
// fuxi init shim

#define SYS_openat          56
#define SYS_close           57
#define SYS_write           64
#define SYS_execve         221
#define SYS_exit            93
#define SYS_finit_module   273
#define SYS_mkdirat         34
#define SYS_mknodat         33
#define SYS_mount           40
#define AT_FDCWD           (-100)
#define O_RDONLY              0
#define O_WRONLY              1
#define NOVAL          0x7fffffff
#define KO_PATH  "/splash.ko"
#define RGB_PATH "/splash.rgb"
#define INIT_PATH "/init.real"

static inline long syscall3(long n, long a, long b, long c)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
	return x0;
}
static inline long syscall5(long n, long a, long b, long c, long d, long e)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
	return x0;
}
static inline long syscall4(long n, long a, long b, long c, long d)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
	return x0;
}
static void ensure_dev(void)
{
	static const char devdir[] = "/dev";
	static const char devtmp[] = "devtmpfs";
	static const char kmsg[] = "/dev/kmsg";
	static const char cons[] = "/dev/console";
	long fd;
	syscall3(SYS_mkdirat, AT_FDCWD, (long)devdir, 0755);
	syscall5(SYS_mount, (long)devtmp, (long)devdir, (long)devtmp, 0, 0);
	fd = syscall4(SYS_openat, AT_FDCWD, (long)kmsg, O_WRONLY, 0);
	if (fd >= 0) {
		syscall3(SYS_close, fd, 0, 0);
	} else {
		syscall4(SYS_mknodat, AT_FDCWD, (long)kmsg, 020600, 0x10b);
	}
	fd = syscall4(SYS_openat, AT_FDCWD, (long)cons, O_WRONLY, 0);
	if (fd >= 0) {
		syscall3(SYS_close, fd, 0, 0);
	} else {
		syscall4(SYS_mknodat, AT_FDCWD, (long)cons, 020600, 0x501);
	}
}
static void klog(const char *msg, long val)
{
	static const char pfx[] = "fuxi-shim: ";
	char buf[96];
	char digits[24];
	unsigned n = 0, i;
	unsigned long v;
	long fd;
	for (i = 0; pfx[i] && n < 64; i++)
		buf[n++] = pfx[i];
	for (i = 0; msg[i] && n < 72; i++)
		buf[n++] = msg[i];
	if (val != NOVAL) {
		buf[n++] = '=';
		if (val < 0) {
			buf[n++] = '-';
			v = (unsigned long)(-val);
		} else {
			v = (unsigned long)val;
		}
		i = 0;
		do {
			digits[i++] = (char)('0' + (v % 10));
			v /= 10;
		} while (v && i < sizeof(digits));
		while (i && n < 90)
			buf[n++] = digits[--i];
	}
	buf[n++] = '\n';
	fd = syscall4(SYS_openat, AT_FDCWD, (long)"/dev/kmsg", O_WRONLY, 0);
	if (fd >= 0) {
		syscall3(SYS_write, fd, (long)buf, (long)n);
		syscall3(SYS_close, fd, 0, 0);
		return;
	}
	fd = syscall4(SYS_openat, AT_FDCWD, (long)"/dev/console", O_WRONLY, 0);
	if (fd >= 0) {
		syscall3(SYS_write, fd, (long)buf, (long)n);
		syscall3(SYS_close, fd, 0, 0);
		return;
	}
	syscall3(SYS_write, 2, (long)buf, (long)n);
}
static void load_splash_module(void)
{
    char param[80];
    long fd, rc;
    unsigned i;
    ensure_dev();
    klog("start", NOVAL);
    static const char key[] = "path=";
    for (i = 0; key[i]; i++)
        param[i] = key[i];
    for (i = 0; RGB_PATH[i] && i < sizeof(param) - 6; i++)
        param[5 + i] = RGB_PATH[i];
    param[5 + i] = '\0';
    fd = syscall4(SYS_openat, AT_FDCWD, (long)KO_PATH, O_RDONLY, 0);
    if (fd < 0) {
        klog("ko open errno", fd);
        return;
    }
    klog("loading splash module", NOVAL);
    rc = syscall3(SYS_finit_module, fd, (long)param, 0);
    klog("finit_module rc", rc);
    syscall3(SYS_close, fd, 0, 0);
}
void shim_main(void *stack)
{
	long *sp = (long *)stack;
	long argc = sp[0];
	char **argv = (char **)&sp[1];
	char **envp = (char **)&sp[2 + argc];
	long rc;
	load_splash_module();
	klog("exec", NOVAL);
	rc = syscall3(SYS_execve, (long)INIT_PATH, (long)argv, (long)envp);
	klog("execve errno", rc);
	syscall3(SYS_exit, 127, 0, 0);
	for (;;)
		;
}
