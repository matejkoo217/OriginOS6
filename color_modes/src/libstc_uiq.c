// SPDX-License-Identifier: MIT
// libstc-uiq.so vivo colour-scheme mapping inside the miui QTI display composer.
// For MIUI 14 vendor for Xiaomi 13, OriginOS6.
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define LOG_TAG "stc_uiq"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define PROP_INTENT "persist.vendor.color.intent"
#define PROP_OFF    "persist.vendor.color.off"
#define PROP_AUTO   "persist.vendor.color.auto"
#define PROP_PCC    "persist.vendor.color.pcc"
#define PROP_TRUETONE "persist.vendor.color.truetone"
#define SETTINGS_DIR	"/data/system/users/0"
#define SYSTEM_XML	SETTINGS_DIR "/settings_system.xml"
#define SECURE_XML	SETTINGS_DIR "/settings_secure.xml"
#define KEY_MODE	"vivo_display_color_mode"
#define KEY_CCT		"vivo_night_display_color_temperature"
#define NODE_GAMUT	"/sys/lcm/colour_gamut"
#define NODE_FPS	"/sys/class/mi_display/disp-DSI-0/dynamic_fps"
#define DFSET_BIN	"/vendor/bin/dfset"
#define CCT_PATH	"/data/vendor/display/light_cct"
#define GAME_PCC_PATH	"/data/vendor/display/game_pcc"
#define MODE_STANDARD		0
#define MODE_PROFESSIONAL	1
#define MODE_BRIGHT		2
#define MODE_ZEISS		3
#define INTENT_STANDARD		258	/* vivid, neutral            */
#define INTENT_PROFESSIONAL	268	/* DCI-P3, wide              */
#define INTENT_BRIGHT		269	/* auto, neutral             */
#define INTENT_ZEISS		269	/* auto + PCC: true tone     */
#define ZEISS_B			235
#define ZEISS_SAT		(-20)

/*
 * True tone (ZEISS only): ambient -> white point, done with PCC.
 *
 * The curve is interpolated in mireds (1e6/K): colour temperature is
 * perceptually uniform in mired, not in kelvin, equal kelvin steps would
 * look lopsided (and the curve would be very steep near the warm end).
 * Outside the outer breakpoints the delta is clamped.
 *
 *   ambient        warmth   PCC pushed (G 255, S -20)
 *   <=2700 K       +1.00    R 255  B 209    full warm   (clamped below)
 *    3000 K        +0.83    R 255  B 213
 *    5000 K        +0.21    R 255  B 229
 *    6500 K         0.00    R 255  B 235    = the ZEISS base, unchanged
 *    8000 K        -0.54    R 245  B 246
 *   >=10000 K      -1.00    R 237  B 255    full cool   (clamped above)
 *
 * These numbers are the whole mapping -- change them here and nowhere else:
 */
#define TT_K_WARM	2700.0f	/* <= this K: full warm shift              */
#define TT_K_NEUTRAL	6500.0f	/* no shift at all: the ZEISS base PCC     */
#define TT_K_COOL	10000.0f /* >= this K: full cool shift             */
#define TT_WARM_DR	   0
#define TT_WARM_DB	 (-26)
#define TT_COOL_DR	 (-18)
#define TT_COOL_DB	 (+20)
#define TT_GAIN_MIN	150
#define TT_GAIN_MAX	255
#define TT_STEP		2
#define TT_MIN_MS	350
#define CM_SRGB 7u

/* ------------------------------------------------------------------------- *
 * miui vendor composer (878136 B, md5 e5f786b05124c93677170359fea9327d)
 * HWCDisplayBuiltIn::SetColorModeWithRenderIntent
 * ------------------------------------------------------------------------- */
#define COMPOSER_NAME "vendor.qti.hardware.display.composer-service"
#define OFF_SETCMWI 0x08e5f4u   /* HWCDisplayBuiltIn::SetColorModeWithRenderIntent */
#define VERIFY_INSNS 24u
#define PATCH_INSNS  23u

static const uint32_t native_insns[VERIFY_INSNS] = {
	0xd503233fu, 0xd10103ffu, 0xa9017bfdu, 0x910043fdu, 0xa90257f6u, 0xa9034ff4u,
	0xf940cc15u, 0xd503201fu, 0x101e83a9u, 0x5280002au, 0x5280042bu, 0x2a0203f3u,
	0xf94002a8u, 0xaa0003f6u, 0x2a0103f4u, 0xcb090109u, 0xd1004129u, 0x93c91529u,
	0x1ac9214au, 0xf100153fu, 0x0a0b0149u, 0x7a409924u, 0x54000480u, 0xf9403908u,
};

/*
 * The patch.  Branch offsets are fixed (assembled for this exact layout):
 *   cbz w9, call -> +6 ; b.eq force -> +2 ; cbnz w20, call -> +3 ;
 *   b 0x8e650    -> +3 (word 20 -> word 23).
 * Word 10 (adrp) is substituted at install time; every other word is fixed.
 */
static const uint32_t patch_words[PATCH_INSNS] = {
	0xd503233fu,	/* paciasp                              */
	0xd10103ffu,	/* sub  sp, sp, #64                     */
	0xa9017bfdu,	/* stp  x29, x30, [sp, #16]             */
	0x910043fdu,	/* add  x29, sp, #16                    */
	0xa90257f6u,	/* stp  x22, x21, [sp, #32]             */
	0xa9034ff4u,	/* stp  x20, x19, [sp, #48]             */
	0xf940cc15u,	/* ldr  x21, [x0, #408]  color_mgr_     */
	0xaa0003f6u,	/* mov  x22, x0         (the tail needs it) */
	0x2a0103f4u,	/* mov  w20, w1         mode            */
	0x2a0203f3u,	/* mov  w19, w2         intent          */
	0x90000009u,	/* adrp x9, <word page>  (imm at runtime) */
	0xf9000920u,	/* str  x0, [x9, #16]    remember `this` */
	0xb9400129u,	/* ldr  w9, [x9]         (word at offset 0) */
	0x340000c9u,	/* cbz  w9, call         not armed -> stock */
	/* --- force (mode 7, intent) for the SDR desktop modes -------- */
	0x71001e9fu,	/* cmp  w20, #7   SRGB                  */
	0x54000040u,	/* b.eq force                           */
	0x35000074u,	/* cbnz w20, call NATIVE falls through  */
	0x528000f4u,	/* force: mov w20, #7                   */
	0x2a0903f3u,	/*        mov w19, w9                   */
	0xf94002a8u,	/* call: ldr x8, [x21]                  */
	0x14000003u,	/*       b    0x8e650  (native tail)    */
	0xd503201fu,	/* nop                                  */
	0xd503201fu,	/* nop                                  */
};
#define MOVZ_W19(v) (0x52800000u | (((uint32_t)(v) & 0xffffu) << 5) | 19u)
#define W_INTENT	0u
#define W_THIS		16u
#define WORD_STEP  0x00200000u
#define WORD_TRIES 64

static uint32_t *g_word;
static uintptr_t g_patch_site;
static int g_state;
static int g_mode = -1;		/* vivo mode, -1 = not known yet   */
static int g_cct = -1;		/* temperature, -1 = unset        */
static int g_computed;
static int g_pinned;
static int g_pcc_ok = 1;
static int g_last_zeiss = -1;
static int g_last_r = -1;
static int g_last_b = -1;
static long g_last_push_ms;
static int g_last_level = -1;
static int g_cct_valid;
static float g_cct_k;
static int g_game_on;
static int g_game_h, g_game_s, g_game_v, g_game_c;
static int g_last_game_on = -1;
static int g_last_gh, g_last_gs, g_last_gv, g_last_gc;
static pthread_mutex_t g_pcc_lock = PTHREAD_MUTEX_INITIALIZER;

static int intent_clamp(int v)
{
	return (v > 0 && v < 512) ? v : 0;
}
static void intent_store(int v)
{
	if (g_word != NULL)
		__atomic_store_n(&g_word[W_INTENT / 4], (uint32_t)v, __ATOMIC_RELEASE);
}
static int intent_load(void)
{
	if (g_word == NULL)
		return 0;
	return (int)__atomic_load_n(&g_word[W_INTENT / 4], __ATOMIC_ACQUIRE);
}
static uintptr_t this_load(void)
{
	if (g_word == NULL)
		return 0;
	return (uintptr_t)__atomic_load_n(
		(uintptr_t *)((char *)g_word + W_THIS), __ATOMIC_ACQUIRE);
}
static uint32_t *map_word_page(uintptr_t site_page, size_t ps)
{
	unsigned i;
	int dir;
	for (i = 1; i <= WORD_TRIES; i++) {
		for (dir = 0; dir < 2; dir++) {
			uintptr_t cand = dir ? site_page - (uintptr_t)i * WORD_STEP : site_page + (uintptr_t)i * WORD_STEP;
			void *p;
			if (cand < ps)
				continue;
			p = mmap((void *)cand, ps, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS |
					 MAP_FIXED_NOREPLACE,
				 -1, 0);
			if (p != MAP_FAILED)
				return (uint32_t *)p;
		}
	}
	return NULL;
}

static uint32_t encode_adrp(uintptr_t insn_addr, uintptr_t target)
{
	int64_t delta_pages = (int64_t)(target >> 12) - (int64_t)(insn_addr >> 12);
	uint32_t imm21;
	if (delta_pages < -(1 << 20) || delta_pages > (1 << 20) - 1)
		return 0;
	imm21 = (uint32_t)delta_pages & 0x1fffffu;
	return 0x90000009u | ((imm21 & 3u) << 29) | ((imm21 >> 2) << 5);
}
static unsigned char g_abx[256 * 1024];
static pthread_mutex_t g_abx_lock = PTHREAD_MUTEX_INITIALIZER;

static int abx_int(const char *path, const char *key, int *out)
{
	struct stat st;
	size_t klen = strlen(key), n = 0, i;
	int fd, found = -1;
	pthread_mutex_lock(&g_abx_lock);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		goto out;
	if (fstat(fd, &st) != 0 || st.st_size <= 0 ||
	    (size_t)st.st_size > sizeof(g_abx))
		goto out_close;
	while (n < (size_t)st.st_size) {
		ssize_t r = read(fd, g_abx + n, (size_t)st.st_size - n);

		if (r <= 0)
			break;
		n += (size_t)r;
	}
	for (i = 0; i + klen + 12 < n; i++) {
		size_t vlen;
		char v[32];
		if (g_abx[i] != 0x00 || g_abx[i + 1] != 0x04)
			continue;
		if (memcmp(g_abx + i + 4, key, klen))
			continue;
		if (g_abx[i + 4 + klen] != 0x2f)
			continue;
		if (g_abx[i + 5 + klen] != 0x00 || g_abx[i + 6 + klen] != 0x05)
			continue;
		vlen = g_abx[i + 8 + klen];
		if (vlen > n - (i + 9 + klen))
			vlen = n - (i + 9 + klen);
		if (vlen >= sizeof(v))
			vlen = sizeof(v) - 1;
		memcpy(v, g_abx + i + 9 + klen, vlen);
		v[vlen] = 0;
		if (v[0] >= '0' && v[0] <= '9')
			found = atoi(v);
		break;
	}
out_close:
	close(fd);
out:
	pthread_mutex_unlock(&g_abx_lock);
	if (found < 0)
		return -1;
	*out = found;
	return 0;
}
static int prop_get(const char *name, int *out)
{
	char buf[PROP_VALUE_MAX];
	int n;
	buf[0] = '\0';
	n = __system_property_get(name, buf);
	if (n <= 0)
		return -1;
	if (!(buf[0] == '-' || (buf[0] >= '0' && buf[0] <= '9')))
		return -1;
	*out = atoi(buf);
	return 0;
}

static int prop_int(const char *name)
{
	int v;
	return prop_get(name, &v) == 0 ? v : 0;
}
static int auto_on(void)
{
	int v;
	if (prop_get(PROP_AUTO, &v) != 0)
		return 1;
	return v != 0;
}

static int pcc_on(void)
{
	int v;
	if (prop_get(PROP_PCC, &v) != 0)
		return 1;
	return v != 0;
}
static int truetone_on(void)
{
	int v;

	if (prop_get(PROP_TRUETONE, &v) != 0)
		return 1;
	return v != 0;
}
static int effective_intent(void)
{
	int prop = intent_clamp(prop_int(PROP_INTENT));
	if (g_pinned || !auto_on())
		return prop;
	if (g_computed)
		return g_computed;
	return prop;
}

static int intent_for_mode(int mode)
{
	switch (mode) {
	case MODE_PROFESSIONAL:
		return INTENT_PROFESSIONAL;
	case MODE_BRIGHT:
		return INTENT_BRIGHT;
	case MODE_STANDARD:
		return INTENT_STANDARD;
	case MODE_ZEISS:
		return INTENT_ZEISS;
	default:
		return 0;
	}
}
static int cct_level(int v)
{
	if (v < 0)
		return 2;	/* unset -> neutral */
	if (v < 85)
		return 1;	/* warm */
	if (v < 171)
		return 2;	/* neutral */
	return 3;		/* cool */
}

/*
 * True tone, ambient colour (CCT) -> additive PCC delta on ZEISS base white point.
 * The record is fixed-width 8-byte ASCII written at offset 0 by sensors.light.so
 */
static int read_ambient(const char *path, double lo, double hi, float *out)
{
	char b[16];
	char *end;
	double v;
	ssize_t n;
	int fd, i;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n < 3)
		return -1;
	b[n] = 0;
	for (i = 0; i < n; i++) {
		if (!((b[i] >= '0' && b[i] <= '9') || b[i] == '.' ||
		      b[i] == ' ' || b[i] == '\n'))
			return -1;
	}
	end = NULL;
	v = strtod(b, &end);
	if (end == b || !(v >= lo) || v > hi)
		return -1;
	*out = (float)v;
	return 0;
}
static int cct_read(float *out)
{
	return read_ambient(CCT_PATH, 800.0, 25000.0, out);
}
static int slider_moved(int cct)
{
	if (cct < 0 || cct > 255)
		return 0;
	return cct_level(cct) != 2;
}
static float tt_warmth_from_cct(float k)
{
	float m, mn, mw, mc;

	if (!(k > 0.0f))
		return 0.0f;
	m = 1e6f / k;
	mn = 1e6f / TT_K_NEUTRAL;
	mw = 1e6f / TT_K_WARM;
	mc = 1e6f / TT_K_COOL;
	if (m > mn) {
		float t = (m - mn) / (mw - mn);
		return t > 1.0f ? 1.0f : t;
	}
	{
		float t = (mn - m) / (mn - mc);
		return t > 1.0f ? -1.0f : -t;
	}
}
static int tt_gain(int base, float delta)
{
	float v = (float)base + delta;
	if (v < (float)TT_GAIN_MIN)
		v = (float)TT_GAIN_MIN;
	if (v > (float)TT_GAIN_MAX)
		v = (float)TT_GAIN_MAX;
	return (int)(v + 0.5f);
}
static int truetone_active(int mode, int cct)
{
	return mode == MODE_ZEISS && truetone_on() && !slider_moved(cct) &&
	       g_cct_valid;
}
static void ambient_gains(int mode, int cct, int *r, int *b, float *warmth)
{
	int zeiss = (mode == MODE_ZEISS);
	float w = 0.0f;
	if (zeiss && truetone_active(mode, cct))
		w = tt_warmth_from_cct(g_cct_k);
	if (w > 0.0f) {
		*r = tt_gain(255, w * (float)TT_WARM_DR);
		*b = tt_gain(ZEISS_B, w * (float)TT_WARM_DB);
	} else if (w < 0.0f) {
		*r = tt_gain(255, -w * (float)TT_COOL_DR);
		*b = tt_gain(ZEISS_B, -w * (float)TT_COOL_DB);
	} else {
		*r = 255;
		*b = zeiss ? ZEISS_B : 255;
	}
	if (warmth != NULL)
		*warmth = w;
}
static int target_level(int mode, int cct)
{
	if (truetone_active(mode, cct))
		return 2;
	return cct_level(cct);
}
typedef uint32_t (*setcmwi_fn)(void *self, uint32_t mode, uint32_t intent);
static void self_apply(int intent)
{
	uintptr_t obj = this_load();
	setcmwi_fn fn = (setcmwi_fn)g_patch_site;
	if (obj == 0 || fn == NULL || intent == 0) {
		LOGI("self_apply skipped (obj=%p intent=%d)", (void *)obj,
		     intent);
		return;
	}
	{
		uint32_t rc = fn((void *)obj, CM_SRGB, (uint32_t)intent);
		LOGI("self_apply(mode 7, intent %d) -> %u", intent, rc);
	}
}
static int run_tool(const char *path, char *const argv[])
{
	pid_t pid;
	int status, rc;
	if (!g_pcc_ok)
		return -1;
	pid = fork();
	if (pid < 0) {
		LOGW("fork failed: %s", strerror(errno));
		g_pcc_ok = 0;
		return -1;
	}
	if (pid == 0) {
		execv(path, argv);
		_exit(127);
	}
	{
		int i, done = 0;
		for (i = 0; i < 200; i++) {
			if (waitpid(pid, &status, WNOHANG) == pid) {
				done = 1;
				break;
			}
			usleep(10 * 1000);
		}
		if (!done) {
			LOGW("%s did not finish in 2 s, killed", path);
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			return -1;
		}
	}
	if (!WIFEXITED(status)) {
		LOGW("waitpid(%s) failed", path);
		return -1;
	}
	rc = WEXITSTATUS(status);
	if (rc == 127) {
		LOGW("%s could not be executed (rc=127), PCC disabled", path);
		g_pcc_ok = 0;
		return -1;
	}
	if (rc != 0) {
		LOGW("%s rc=%d", path, rc);
		return -1;
	}
	return 0;
}
static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

static int game_clamp(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}
static int game_read(void)
{
	char buf[64];
	int fd, n, on = 0, h = 0, s = 0, v = 0, c = 0;
	fd = open(GAME_PCC_PATH, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		g_game_on = 0;
		return 0;
	}
	n = (int)read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) {
		g_game_on = 0;
		return 0;
	}
	buf[n] = 0;
	if (sscanf(buf, "%d %d %d %d %d", &on, &h, &s, &v, &c) != 5 || on != 1) {
		g_game_on = 0;
		return 0;
	}
	g_game_h = game_clamp(h, -180, 180);
	g_game_s = game_clamp(s, -50, 50);
	g_game_v = game_clamp(v, -255, 255);
	g_game_c = game_clamp(c, -100, 100);
	g_game_on = 1;
	return 1;
}

static void apply_pcc(int mode, int cct, int force)
{
	int zeiss = (mode == MODE_ZEISS);
	int level, prev, r, b, amb, need;
	float warmth = 0.0f;
	long now;
	if (!pcc_on())
		return;
	if (game_read()) {
		int game_need;
		pthread_mutex_lock(&g_pcc_lock);
		game_need = force || g_last_game_on != 1 ||
			    g_game_h != g_last_gh || g_game_s != g_last_gs ||
			    g_game_v != g_last_gv || g_game_c != g_last_gc;
		if (game_need) {
			char gh[12], gs[12], gv[12], gc[12];
			char *argv[] = { (char *)"dfset", (char *)"-e",
					 (char *)"255", (char *)"255",
					 (char *)"255", gh, gs, gv, gc,
					 (char *)"220", NULL };
			snprintf(gh, sizeof(gh), "%d", g_game_h);
			snprintf(gs, sizeof(gs), "%d", g_game_s);
			snprintf(gv, sizeof(gv), "%d", g_game_v);
			snprintf(gc, sizeof(gc), "%d", g_game_c);
			if (run_tool(DFSET_BIN, argv) == 0) {
				g_last_game_on = 1;
				g_last_gh = g_game_h;
				g_last_gs = g_game_s;
				g_last_gv = g_game_v;
				g_last_gc = g_game_c;
				g_last_push_ms = now_ms();
				LOGI("EXPERT_STATE game H %d S %d V %d C %d (mode %d, cct %d)",
				     g_game_h, g_game_s, g_game_v, g_game_c,
				     mode, cct);
			}
		}
		pthread_mutex_unlock(&g_pcc_lock);
		return;
	}
	if (g_last_game_on == 1) {
		g_last_game_on = 0;
		force = 1;
		g_last_zeiss = -1;
		g_last_level = -1;
		g_last_r = -1;
		g_last_b = -1;
	}
	pthread_mutex_lock(&g_pcc_lock);
	level = target_level(mode, cct);
	amb = truetone_active(mode, cct);
	ambient_gains(mode, cct, &r, &b, &warmth);
	if (force && !zeiss && level == 2 && g_last_zeiss == 0 &&
	    g_last_level == 2) {
		pthread_mutex_unlock(&g_pcc_lock);
		return;
	}
	need = force || zeiss != g_last_zeiss ||
	       abs(r - g_last_r) >= TT_STEP || abs(b - g_last_b) >= TT_STEP;
	if (need) {
		int ambient_only = !force && zeiss == g_last_zeiss;
		now = now_ms();
		if (ambient_only && g_last_push_ms != 0 &&
		    now - g_last_push_ms < TT_MIN_MS) {
			usleep((useconds_t)(TT_MIN_MS -
					    (now - g_last_push_ms)) * 1000);
		}
		{
			char g_r[12], g_b[12], sat[12];
			char *argv[] = { (char *)"dfset", (char *)"-e", g_r,
					 (char *)"255", g_b, (char *)"0", sat,
					 (char *)"0", (char *)"0", (char *)"220",
					 NULL };
			snprintf(g_r, sizeof(g_r), "%d", r);
			snprintf(g_b, sizeof(g_b), "%d", b);
			snprintf(sat, sizeof(sat), "%d",
				 zeiss ? ZEISS_SAT : 0);
			if (run_tool(DFSET_BIN, argv) == 0) {
				g_last_zeiss = zeiss;
				g_last_r = r;
				g_last_b = b;
				g_last_push_ms = now_ms();
				if (amb) {
					if (g_cct_valid)
						LOGI("EXPERT_STATE R %d G 255 B %d S %d (ambient %.0f K, warmth %+.2f, mode %d, cct %d)",
						     r, b, ZEISS_SAT,
						     (double)g_cct_k,
						     (double)warmth, mode, cct);
					else
						LOGI("EXPERT_STATE R %d G 255 B %d S %d (ambient unknown, mode %d, cct %d)",
						     r, b, ZEISS_SAT, mode, cct);
				} else if (zeiss) {
					LOGI("EXPERT_STATE R %d G 255 B %d S %d (base, mode %d, cct %d)",
					     r, b, ZEISS_SAT, mode, cct);
				}
			}
		}
	}
	if (force || level != g_last_level) {
		char l[8];
		prev = g_last_level;
		snprintf(l, sizeof(l), "%d", level);
		{
			char *argv[] = { (char *)"dfset", (char *)"-t", l, NULL };
			if (run_tool(DFSET_BIN, argv) == 0) {
				g_last_level = level;
				if (level != prev) {
					char src[48];
					if (!truetone_active(mode, cct))
						snprintf(src, sizeof(src),
							 "slider");
					else if (g_cct_valid)
						snprintf(src, sizeof(src),
							 "PCC, ambient %.0f K",
							 (double)g_cct_k);
					else
						snprintf(src, sizeof(src),
							 "PCC, ambient unknown");
					LOGI("COLOR_TEMP_STATE %d -> %d (%s, mode %d, cct %d)",
					     prev, level, src, mode, cct);
				}
			}
		}
	}
	pthread_mutex_unlock(&g_pcc_lock);
}
static void rederive(const char *why, int force_pcc)
{
	int mode = g_mode, cct = g_cct, v, eff, old, changed;
	float cct_k;
	g_cct_valid = (cct_read(&cct_k) == 0);
	if (g_cct_valid)
		g_cct_k = cct_k;
	if (abx_int(SYSTEM_XML, KEY_MODE, &v) == 0 && v >= 0 && v <= 3)
		mode = v;
	if (abx_int(SECURE_XML, KEY_CCT, &v) == 0)
		cct = v;
	changed = (mode != g_mode || cct != g_cct);
	g_mode = mode;
	g_cct = cct;
	g_computed = intent_for_mode(mode);
	eff = effective_intent();
	old = intent_load();
	if (eff != old) {
		intent_store(eff);
		changed = 1;
	}
	if (changed || force_pcc)
		LOGI("%s: vivo mode %d cct %d ambient %s%.0fK -> intent %d (was %d)%s",
		     why, mode, cct, g_cct_valid ? "" : "?",
		     (double)g_cct_k,
		     eff, old,
		     g_pinned ? " [pinned]" : (auto_on() ? "" : " [auto off]"));
	if (eff != 0 && (eff != old || force_pcc))
		self_apply(eff);
	apply_pcc(mode, cct, force_pcc);
}
static void *prop_watcher(void *arg)
{
	uint32_t serial = 0;
	int last_intent = -1, last_auto = -1, last_tt = -1;
	(void)arg;
	pthread_setname_np(pthread_self(), "stc-uiq-prop");
	last_intent = intent_clamp(prop_int(PROP_INTENT));
	last_auto = auto_on();
	last_tt = truetone_on();
	for (;;) {
		uint32_t next = serial;
		int v = intent_clamp(prop_int(PROP_INTENT));
		int auto_now = auto_on();
		int tt_now = truetone_on();
		int eff, old;
		if (v != last_intent) {
			g_pinned = (v != 0);
			LOGW(PROP_INTENT "=%d -> %s", v,
			     g_pinned ? "manual override" : "mode mapping");
			last_intent = v;
		}
		if (auto_now != last_auto) {
			if (auto_now)
				g_pinned = 0;
			LOGW(PROP_AUTO "=%d", auto_now);
			last_auto = auto_now;
		}
		if (tt_now != last_tt) {
			LOGW(PROP_TRUETONE "=%d -> %s", tt_now,
			     tt_now ? "ambient CCT for ZEISS"
				    : "colour temperature from the slider only");
			last_tt = tt_now;
			apply_pcc(g_mode, g_cct, 1);
		}
		eff = effective_intent();
		old = intent_load();
		if (eff != old) {
			intent_store(eff);
			LOGW("property: intent %d -> %d", old, eff);
		}
		__system_property_wait(NULL, serial, &next, NULL);
		serial = next;
	}
	return NULL;
}
#define DEBOUNCE_US 40000
static void drain(int fd, int *hit)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));
		ssize_t o;
		if (n <= 0)
			return;
		for (o = 0; o < n;) {
			struct inotify_event *e = (void *)(buf + o);
			if (e->len == 0) {
				*hit = 1;
			} else if (!strcmp(e->name, "settings_system.xml") ||
				   !strcmp(e->name, "settings_secure.xml")) {
				*hit = 1;
			}
			o += sizeof(*e) + e->len;
		}
	}
}
static void *mode_watcher(void *arg)
{
	int ifd;
	int fps;
	int cctw = -1;
	int gamew = -1;
	int prev_fps = -1;
	(void)arg;
	pthread_setname_np(pthread_self(), "stc-uiq-mode");
	ifd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	if (ifd >= 0) {
		if (inotify_add_watch(ifd, SETTINGS_DIR,
				      IN_MOVED_TO | IN_CREATE |
					      IN_CLOSE_WRITE) < 0)
			LOGW("inotify(" SETTINGS_DIR "): %s", strerror(errno));
		if (inotify_add_watch(ifd, NODE_GAMUT,
				      IN_MODIFY | IN_CLOSE_WRITE |
					      IN_ATTRIB) < 0)
			LOGW("inotify(" NODE_GAMUT "): %s", strerror(errno));
		cctw = inotify_add_watch(ifd, CCT_PATH,
					 IN_MODIFY | IN_CLOSE_WRITE);
		if (cctw < 0)
			LOGW("inotify(" CCT_PATH "): %s, will retry",
			     strerror(errno));
		else
			LOGI("watching " CCT_PATH " for true tone");
		gamew = inotify_add_watch(ifd, GAME_PCC_PATH,
					  IN_MODIFY | IN_CLOSE_WRITE);
		if (gamew < 0)
			LOGW("inotify(" GAME_PCC_PATH "): %s, will retry",
			     strerror(errno));
		else
			LOGI("watching " GAME_PCC_PATH " for game filters");
	} else {
		LOGW("inotify_init1: %s", strerror(errno));
	}
	fps = open(NODE_FPS, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

	/* the composer re-applies SF's colour mode right after this thread
	 * starts; give the display a moment, then push the intent and PCC/CCT
	 * once, so a boot straight into ZEISS looks right */
	sleep(3);
	rederive("startup", 1);
	for (;;) {
		struct pollfd p[2];
		int n = 0, hit = 0, resume = 0;
		int timeout = (ifd < 0 || (cctw >= 0 && gamew >= 0)) ? -1 : 2000;
		p[n].fd = ifd;
		p[n].events = POLLIN;
		p[n].revents = 0;
		n++;
		if (fps >= 0) {
			p[n].fd = fps;
			p[n].events = POLLPRI;
			p[n].revents = 0;
			n++;
		}
		if (poll(p, (nfds_t)n, timeout) < 0) {
			if (errno == EINTR)
				continue;
			LOGW("poll: %s", strerror(errno));
			return NULL;
		}
		if (cctw < 0 && ifd >= 0) {
			cctw = inotify_add_watch(ifd, CCT_PATH,
						 IN_MODIFY | IN_CLOSE_WRITE);
			if (cctw >= 0) {
				LOGI("watching " CCT_PATH " for true tone");
				hit = 1;
			}
		}
		if (gamew < 0 && ifd >= 0) {
			gamew = inotify_add_watch(ifd, GAME_PCC_PATH,
						  IN_MODIFY | IN_CLOSE_WRITE);
			if (gamew >= 0) {
				LOGI("watching " GAME_PCC_PATH
				     " for game filters");
				hit = 1;
			}
		}
		if (ifd >= 0 && (p[0].revents & POLLIN))
			drain(ifd, &hit);
		if (fps >= 0 && n > 1 && (p[n - 1].revents & POLLPRI)) {
			char b[32];
			ssize_t r = pread(fps, b, sizeof(b) - 1, 0);
			if (r > 0) {
				int v;
				b[r] = 0;
				v = atoi(b);
				if (prev_fps > 0 && v > prev_fps)
					resume = 1;
				if (v > 0)
					prev_fps = v;
			}
		}
		if (!hit && !resume)
			continue;
		usleep(DEBOUNCE_US);
		if (ifd >= 0)
			drain(ifd, &hit);
		rederive(resume ? "resume" : "settings", 0);
		if (ifd >= 0 && cctw >= 0 && !g_cct_valid) {
			cctw = inotify_add_watch(ifd, CCT_PATH,
						 IN_MODIFY | IN_CLOSE_WRITE);
		}
		/*
		 * On resume the composer restores SurfaceFlinger's colour mode
		 * "while the display is still settling", and the PCC LUT goes
		 * with it; The patched path keeps the intent for free, 
		 * but PCC is only re-pushed from here, so do three passes.
		 */
		if (resume) {
			int k;
			for (k = 0; k < 2; k++) {
				usleep((300 + 300 * k) * 1000);
				if (ifd >= 0)
					drain(ifd, &hit);
				rederive("resume", 1);
			}
		}
	}
	return NULL;
}
static int pin_self(void)
{
	static const char *const names[] = {
		"libstc-uiq.so", "/vendor/lib64/libstc-uiq.so"
	};
	unsigned i;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (dlopen(names[i], RTLD_NOW | RTLD_NODELETE) != NULL)
			return 0;
	}
	return -1;
}
static uintptr_t composer_base(void)
{
	char line[512];
	uintptr_t base = 0;
	FILE *f = fopen("/proc/self/maps", "r");
	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		unsigned long start = 0, end = 0, off = 1;
		char perms[8];
		if (strstr(line, COMPOSER_NAME) == NULL)
			continue;
		if (sscanf(line, "%lx-%lx %7s %lx %*s %*s", &start, &end,
			   perms, &off) < 4)
			continue;
		if (off != 0)
			continue;
		base = (uintptr_t)start;
		break;
	}
	fclose(f);
	return base;
}
static int patch_text(uintptr_t addr, const uint32_t *words, unsigned n)
{
	long ps = sysconf(_SC_PAGESIZE);
	uintptr_t page, last;
	volatile uint32_t *p = (volatile uint32_t *)addr;
	unsigned i;
	if (ps <= 0)
		return -1;
	page = addr & ~(uintptr_t)(ps - 1);
	last = (addr + n * 4u - 1u) & ~(uintptr_t)(ps - 1);
	if (mprotect((void *)page, (size_t)(last - page + (uintptr_t)ps),
		     PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
		LOGE("mprotect(%p) failed: %s", (void *)page, strerror(errno));
		return -1;
	}
	for (i = 0; i < n; i++)
		p[i] = words[i];
	__builtin___clear_cache((char *)addr, (char *)(addr + n * 4u));
	if (mprotect((void *)page, (size_t)(last - page + (uintptr_t)ps),
		     PROT_READ | PROT_EXEC) != 0)
		LOGW("could not restore r-x on %p: %s", (void *)page,
		     strerror(errno));
	return 0;
}
static int start_thread(void *(*fn)(void *))
{
	pthread_attr_t attr;
	pthread_t tid;
	int rc;
	if (pthread_attr_init(&attr) != 0)
		return -1;
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 128 * 1024);
	rc = pthread_create(&tid, &attr, fn, NULL);
	pthread_attr_destroy(&attr);
	return rc == 0 ? 0 : -1;
}
static void install_once(void)
{
	uint32_t patch[PATCH_INSNS];
	const volatile uint32_t *native;
	uintptr_t base, site, site_page;
	long ps;
	int intent, dynamic, i, v;
	if (g_state != 0)
		return;
	g_state = -1;
	if (prop_int(PROP_OFF) != 0) {
		LOGW(PROP_OFF " is set, staying out of the way");
		return;
	}
	base = composer_base();
	if (base == 0) {
		LOGW("composer not found in /proc/self/maps, disabled");
		return;
	}
	site = base + OFF_SETCMWI;
	native = (const volatile uint32_t *)site;
	for (i = 0; i < (int)VERIFY_INSNS; i++) {
		if (native[i] != native_insns[i]) {
			LOGW("composer text does not match (insn %d: %08x != "
			     "%08x), disabled", i, native[i], native_insns[i]);
			return;
		}
	}
	if (abx_int(SYSTEM_XML, KEY_MODE, &v) == 0 && v >= 0 && v <= 3)
		g_mode = v;
	if (abx_int(SECURE_XML, KEY_CCT, &v) == 0)
		g_cct = v;
	g_computed = intent_for_mode(g_mode);
	intent = effective_intent();
	ps = sysconf(_SC_PAGESIZE);
	site_page = site & ~(uintptr_t)(ps - 1);
	memcpy(patch, patch_words, sizeof(patch));
	g_word = NULL;
	if (ps > 0)
		g_word = map_word_page(site_page, (size_t)ps);
	if (g_word != NULL) {
		uint32_t adrp = encode_adrp(site + 10u * 4u, (uintptr_t)g_word);
		if (adrp == 0) {
			munmap(g_word, (size_t)ps);
			g_word = NULL;
		} else {
			patch[10] = adrp;
			dynamic = 1;
		}
	}
	if (g_word == NULL) {
		LOGW("no free page within adrp range, falling back to a "
		     "baked-in intent (composer restart needed to change it)");
		patch[10] = 0xd503201fu;
		patch[11] = 0xd503201fu;
		patch[12] = 0xd503201fu;
		patch[13] = MOVZ_W19(intent);
		dynamic = 0;
	}
	if (patch_text(site, patch, PATCH_INSNS) != 0) {
		LOGW("text patch failed, disabled");
		return;
	}
	g_patch_site = site;
	g_state = 1;
	if (!dynamic) {
		LOGI("patched %s+%#x: mode %u + RenderIntent %d forced for "
		     "ColorMode %u/%u (base=%p)", COMPOSER_NAME, OFF_SETCMWI,
		     CM_SRGB, intent, CM_SRGB, 0u, (void *)base);
		return;
	}
	intent_store(intent);
	LOGI("patched %s+%#x: mode %u + dynamic RenderIntent %d "
	     "(word %p) forced for ColorMode %u/%u (base=%p, vivo mode %d, "
	     "cct %d)", COMPOSER_NAME, OFF_SETCMWI, CM_SRGB, intent,
	     (void *)g_word, CM_SRGB, 0u, (void *)base, g_mode, g_cct);
	if (pin_self() != 0) {
		LOGW("could not pin the module in memory, intent stays at %d "
		     "until the next composer restart", intent);
		return;
	}
	if (start_thread(prop_watcher) != 0)
		LOGW("property watcher failed, manual override will not work");
	if (start_thread(mode_watcher) != 0)
		LOGW("mode watcher failed, only the property is used");
}

/*
 * STC plugin contract, recovered from libsnapdragoncolor-manager.so
 * (StcManager::GetLibInterface):
 *  void *GetScPostBlendInterface(uint32_t major, uint32_t minor);
 */
void *GetScPostBlendInterface(uint32_t major, uint32_t minor);
void *GetScPostBlendInterface(uint32_t major, uint32_t minor)
{
	LOGI("loaded by composer (GetScPostBlendInterface(%u,%u))", major,
	     minor);
	install_once();
	return NULL;
}
__attribute__((constructor)) static void stcuiq_ctor(void)
{
	install_once();
}
