// SPDX-License-Identifier: MIT
/*
 * vtsgesture - screen-off gesture recogniser that reuses vivo's own scoring
 * algorithm (GetScore from bbktouchscreen.default.so).
 *
 * vts_bridge.ko with auto_resume=1 keeps the FTS IC scanning normally while the panel is off/AOD,
 * so /dev/input/eventX carries raw coordinates.
 *
 * Usage:
 *   vtsgesture -e <name>    enrol: wait for one stroke, save <dir>/<name>.sig
 *   vtsgesture -l           list templates
 *   vtsgesture              run: recognise strokes while the screen is off
 *
 * Base templates ship in /vendor_dlkm/etc/vts_gestures.
 * Self-learned samples live in /data/vts_gestures (persistent).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/system_properties.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>

#define DEVICE      "/dev/input/event7"
#define BASE_DIR    "/vendor_dlkm/etc/vts_gestures"
#define GEST_DIR    "/data/vts_gestures"
#define HAL_SO      "/vendor/lib64/hw/bbktouchscreen.default.so"
#define NPOINTS     30
#define SIGLEN      (NPOINTS * 2)
#define MAX_TPL     128
#define MAX_PTS     2048
#define DTW_MAX_X100   280
#define DTW_MARGIN_X100 30
#define SCORE_MARGIN 40
#define SCORE_COUNT 29

struct pt { float x, y; };
struct tpl {
	char name[32];
	unsigned char sig[SIGLEN];
};
struct stroke {
	struct pt p[MAX_PTS];
	int n;
	int active;
	int slot;
	long long t0;
};

static int (*score_fn)(const void *, const void *, int);
static float sqrt_lut[649];
static float dist(const struct pt *a, const struct pt *b)
{
	float dx = a->x - b->x, dy = a->y - b->y;
	return sqrtf(dx * dx + dy * dy);
}
static int make_sig(const struct stroke *s, unsigned char out[SIGLEN])
{
	float cum[MAX_PTS], total;
	struct pt r[NPOINTS];
	int i, j;
	if (getenv("VTS_DUMP")) {
		int k;
		fprintf(stderr, "DUMP n=%d\n", s->n);
		for (k = 0; k < s->n; k++)
			fprintf(stderr, "P %d %d\n", (int)s->p[k].x,
				(int)s->p[k].y);
	}
	if (s->n < 8)
		return -1;
	cum[0] = 0;
	for (i = 1; i < s->n; i++)
		cum[i] = cum[i - 1] + dist(&s->p[i - 1], &s->p[i]);
	total = cum[s->n - 1];
	if (total < 300.0f)
		return -1;
	j = 0;
	for (i = 0; i < NPOINTS; i++) {
		float t = total * i / (NPOINTS - 1);
		while (j < s->n - 2 && cum[j + 1] < t)
			j++;
		if (cum[j + 1] > cum[j]) {
			float f = (t - cum[j]) / (cum[j + 1] - cum[j]);
			r[i].x = s->p[j].x + (s->p[j + 1].x - s->p[j].x) * f;
			r[i].y = s->p[j].y + (s->p[j + 1].y - s->p[j].y) * f;
		} else {
			r[i] = s->p[j];
		}
	}
	{
		float x0 = r[0].x, x1 = r[0].x, y0 = r[0].y, y1 = r[0].y;
		float cx, cy, scale;
		for (i = 1; i < NPOINTS; i++) {
			if (r[i].x < x0) x0 = r[i].x;
			if (r[i].x > x1) x1 = r[i].x;
			if (r[i].y < y0) y0 = r[i].y;
			if (r[i].y > y1) y1 = r[i].y;
		}
		if (x1 - x0 < 1.0f) x1 = x0 + 1.0f;
		if (y1 - y0 < 1.0f) y1 = y0 + 1.0f;
		(void)cx; (void)cy; (void)scale;
		for (i = 0; i < NPOINTS; i++) {
			int qx = (int)(18.0f * (r[i].x - x0) / (x1 - x0) + 0.5f);
			int qy = (int)(18.0f * (r[i].y - y0) / (y1 - y0) + 0.5f);
			if (qx < 0) qx = 0;
			if (qx > 18) qx = 18;
			if (qy < 0) qy = 0;
			if (qy > 18) qy = 18;
			out[i * 2] = qx;
			out[i * 2 + 1] = qy;
		}
	}
	return 0;
}
static void sig_reverse(const unsigned char *in, unsigned char *out)
{
	int i;
	for (i = 0; i < NPOINTS; i++) {
		out[i * 2] = in[(NPOINTS - 1 - i) * 2];
		out[i * 2 + 1] = in[(NPOINTS - 1 - i) * 2 + 1];
	}
}
static void sig_shift(const unsigned char *in, unsigned char *out, int k)
{
	int i;
	for (i = 0; i < NPOINTS; i++) {
		int j = (i + k) % NPOINTS;
		out[i * 2] = in[j * 2];
		out[i * 2 + 1] = in[j * 2 + 1];
	}
}
static int sig_closed(const unsigned char *s)
{
	int dx = s[0] - s[(NPOINTS - 1) * 2];
	int dy = s[1] - s[(NPOINTS - 1) * 2 + 1];
	return dx * dx + dy * dy <= 25;
}
static int score_variants(const unsigned char *tpl, const unsigned char *sig, int closed)
{
	unsigned char rev[SIGLEN], tmp[SIGLEN];
	int best, k;
	best = score_fn(tpl, sig, SCORE_COUNT);
	sig_reverse(sig, rev);
	{
		int sc = score_fn(tpl, rev, SCORE_COUNT);

		if (sc < best)
			best = sc;
	}
	if (!closed)
		return best;
	for (k = 1; k < NPOINTS; k++) {
		int sc;
		sig_shift(sig, tmp, k);
		sc = score_fn(tpl, tmp, SCORE_COUNT);
		if (sc < best)
			best = sc;
		sig_shift(rev, tmp, k);
		sc = score_fn(tpl, tmp, SCORE_COUNT);
		if (sc < best)
			best = sc;
	}
	return best;
}
static float dtw_core(const unsigned char *a, const unsigned char *b)
{
	static float dp[NPOINTS + 1][NPOINTS + 1];
	int i, j;
	for (i = 0; i <= NPOINTS; i++) {
		dp[i][0] = 1e9f;
		dp[0][i] = 1e9f;
	}
	dp[0][0] = 0;
	for (i = 1; i <= NPOINTS; i++) {
		for (j = 1; j <= NPOINTS; j++) {
			int dx = (int)a[(i - 1) * 2] - (int)b[(j - 1) * 2];
			int dy = (int)a[(i - 1) * 2 + 1] - (int)b[(j - 1) * 2 + 1];
			float d = sqrt_lut[dx * dx + dy * dy];
			float m = dp[i - 1][j];
			if (dp[i][j - 1] < m)
				m = dp[i][j - 1];
			if (dp[i - 1][j - 1] < m)
				m = dp[i - 1][j - 1];
			dp[i][j] = d + m;
		}
	}
	return dp[NPOINTS][NPOINTS] / (2.0f * NPOINTS);
}
static float dtw_variants(const unsigned char *tpl, const unsigned char *sig, int closed)
{
	unsigned char rev[SIGLEN], tmp[SIGLEN];
	float best, sc;
	int k;
	best = dtw_core(tpl, sig);
	sig_reverse(sig, rev);
	sc = dtw_core(tpl, rev);
	if (sc < best)
		best = sc;
	if (!closed)
		return best;
	for (k = 1; k < NPOINTS; k++) {
		sig_shift(sig, tmp, k);
		sc = dtw_core(tpl, tmp);
		if (sc < best)
			best = sc;
		sig_shift(rev, tmp, k);
		sc = dtw_core(tpl, tmp);
		if (sc < best)
			best = sc;
	}
	return best;
}
static int ensure_dir(const char *dir)
{
	if (mkdir(dir, 0700) && errno != EEXIST) {
		fprintf(stderr, "mkdir %s: %s\n", dir, strerror(errno));
		return -1;
	}
	return 0;
}

static int load_dir(const char *dir, struct tpl *t, int max, int n)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	if (!d)
		return n;
	while ((de = readdir(d)) && n < max) {
		char path[256];
		FILE *f;
		size_t got;
		int i, dup = 0;
		if (strstr(de->d_name, ".sig") == NULL)
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		f = fopen(path, "rb");
		if (!f)
			continue;
		got = fread(t[n].sig, 1, SIGLEN, f);
		fclose(f);
		if (got != SIGLEN)
			continue;
		snprintf(t[n].name, sizeof(t[n].name), "%s", de->d_name);
		t[n].name[strcspn(t[n].name, ".")] = 0;
		{
			char *dot = strrchr(t[n].name, '.');
			if (dot && dot[1] && strspn(dot + 1, "0123456789") == strlen(dot + 1))
				*dot = 0;
		}
		for (i = 0; i < n; i++) {
			if (!strcmp(t[i].name, t[n].name) &&
			    !memcmp(t[i].sig, t[n].sig, SIGLEN)) {
				dup = 1;
				break;
			}
		}
		if (!dup)
			n++;
	}
	closedir(d);
	return n;
}
static int load_tpl(struct tpl *t, int max)
{
	int n = 0;
	n = load_dir(BASE_DIR, t, max, n);
	n = load_dir(GEST_DIR, t, max, n);
	return n;
}

static int sig_exists(const char *name, int idx)
{
	char path[256];
	snprintf(path, sizeof(path), BASE_DIR "/%s.%d.sig", name, idx);
	if (!access(path, F_OK))
		return 1;
	snprintf(path, sizeof(path), GEST_DIR "/%s.%d.sig", name, idx);
	return !access(path, F_OK);
}

static int save_sig(const char *name, const unsigned char *sig)
{
	char path[256];
	FILE *f;
	int idx = 1;
	if (ensure_dir(GEST_DIR))
		return -1;
	while (sig_exists(name, idx))
		idx++;
	snprintf(path, sizeof(path), GEST_DIR "/%s.%d.sig", name, idx);
	f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return -1;
	}
	fwrite(sig, 1, SIGLEN, f);
	fclose(f);
	printf("saved %s\n", path);
	return 0;
}
static int run_cmd(const char *cmd)
{
	char buf[256];
	int r;
	snprintf(buf, sizeof(buf), "%s", cmd);
	r = system(buf);
	return r;
}
static int setting_int(const char *ns, const char *key, int dflt)
{
	char cmd[256], buf[64];
	FILE *p;
	int v = dflt;
	snprintf(cmd, sizeof(cmd), "/system/bin/settings get %s %s 2>/dev/null", ns, key);
	p = popen(cmd, "r");
	if (!p)
		return dflt;
	if (fgets(buf, sizeof(buf), p)) {
		if (!strncmp(buf, "null", 4) || buf[0] == '\n')
			v = dflt;
		else
			v = atoi(buf);
	}
	pclose(p);
	return v;
}
struct gate {
	const char *label;
	const char *ns;
	const char *key;
	int master;
	int wake;
	int lockscreen;
};
static const struct gate gates[] = {
	{ "c", "system", "bbk_screen_disable_wake_dial_setting", 1, 1, 0 },
	{ "m", "system", "bbk_screen_disable_wake_music_setting", 1, 0, 0 },
	{ "e", "system", "bbk_screen_disable_wake_browser_setting", 1, 1, 0 },
	{ "o", "system", "bbk_screen_disable_wake_qq_setting", 1, 1, 0 },
	{ "w", "system", "bbk_screen_disable_wake_wechat_setting", 1, 1, 0 },
	{ "swipe_up", "system", "bbk_screen_disable_to_unlock_setting", 1, 1, 0 },
	{ "swipe_down", "system", "bbk_quick_open_camera_setting", 1, 1, 1 },
	{ "tap", "secure", "double_tap_to_wake", 0, 1, 0 },
};
#define NGATES (sizeof(gates) / sizeof(gates[0]))
static int g_allowed[NGATES];
static const struct gate *gate_for(const char *label)
{
	size_t i;
	for (i = 0; i < sizeof(gates) / sizeof(gates[0]); i++)
		if (!strcmp(gates[i].label, label))
			return &gates[i];
	return NULL;
}
static int action_allowed(const char *label)
{
	size_t i;
	for (i = 0; i < NGATES; i++)
		if (!strcmp(gates[i].label, label))
			return g_allowed[i];
	return 1;
}
static char *capture(const char *cmd, size_t max)
{
	FILE *p = popen(cmd, "r");
	char *buf;
	size_t n = 0;
	if (!p)
		return NULL;
	buf = malloc(max);
	if (!buf) {
		pclose(p);
		return NULL;
	}
	n = fread(buf, 1, max - 1, p);
	pclose(p);
	buf[n] = 0;
	return buf;
}
static int dump_get(const char *dump, const char *key)
{
	size_t kl = strlen(key);
	const char *p = dump;
	if (!dump)
		return -1;
	while ((p = strstr(p, key))) {
		if ((p == dump || p[-1] == '\n') && p[kl] == '=')
			return atoi(p + kl + 1);
		p += kl;
	}
	return -1;
}
static void kobj_write(const char *path, const char *val)
{
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(val, f);
		fclose(f);
	}
}
static volatile int g_active = -1, g_letters, g_dt2w;
static int screen_is_off(void);

static volatile sig_atomic_t g_wake;

static void sigusr1(int sig)
{
	(void)sig;
	g_wake = 1;
}

static void gating_refresh(void)
{
	char *dump = capture("/system/bin/settings list system", 65536);
	int master, letters = 0, dt2w, active;
	size_t i;
	if (!dump || !dump[0] || strstr(dump, "Can't find service") ||
	    strstr(dump, "Error")) {
		free(dump);
		return;
	}
	master = dump_get(dump, "bbk_smart_wakeup");
	dt2w = setting_int("secure", "double_tap_to_wake", -1);
	if (dt2w < 0) {
		free(dump);
		return;
	}
	for (i = 0; i < NGATES; i++) {
		if (!strcmp(gates[i].ns, "secure"))
			continue;
		g_allowed[i] = dump_get(dump, gates[i].key) == 1;
		if (gates[i].master && g_allowed[i])
			letters = 1;
	}
	free(dump);
	dt2w = dt2w == 1;
	for (i = 0; i < NGATES; i++)
		if (!strcmp(gates[i].ns, "secure"))
			g_allowed[i] = dt2w;
	active = letters || dt2w;
	g_letters = letters;
	g_dt2w = dt2w;
	if (active != g_active) {
		g_active = active;
		kobj_write("/sys/module/vts_bridge/parameters/auto_resume",
			   active ? "1" : "0");
		printf("gate: %s (letters=%d dt2w=%d master=%d)\n",
		       active ? "on" : "off", letters, dt2w, master);
		fflush(stdout);
	}
	if (active && screen_is_off())
		kobj_write("/sys/touchscreen/resume", "1");
}
struct action {
	const char *label;
	const char *cmd;
};
static const struct action actions[] = {
	{ "c", "am start -a android.intent.action.DIAL" },
	{ "m", "input keyevent KEYCODE_MEDIA_PLAY_PAUSE" },
	{ "e", "am start -a android.intent.action.VIEW" },
	{ "o", "monkey -p com.tencent.mobileqq -c android.intent.category.LAUNCHER 1" },
	{ "w", "monkey -p com.tencent.mm -c android.intent.category.LAUNCHER 1" },
	{ "swipe_up", "true" },
	{ "swipe_down", "am start -a android.media.action.VIVO_STILL_IMAGE_CAMERA_SECURE -n com.android.camera/com.android.camera.SecureCameraActivity -f 0x10018000 --es wave_open_application 0 --es SlideToCamera wakeup" },
};
static void *screen_watch(void *arg)
{
	const prop_info *pi;
	uint32_t serial, new_serial;
	int i;
	(void)arg;
	pi = __system_property_find("debug.tracing.screen_state");
	for (i = 0; !pi && i < 30; i++) {
		sleep(1);
		pi = __system_property_find("debug.tracing.screen_state");
	}
	if (!pi)
		return NULL;
	sleep(5);
	gating_refresh();
	serial = __system_property_serial(pi);
	for (;;) {
		char val[PROP_VALUE_MAX] = "";
		if (!__system_property_wait(pi, serial, &new_serial, NULL))
			continue;
		serial = new_serial;
		__system_property_get("debug.tracing.screen_state", val);
		printf("screen state -> %s\n", val);
		fflush(stdout);
		gating_refresh();
	{
		int gate_wait;
		for (gate_wait = 0; gate_wait < 10 && g_active < 0; gate_wait++) {
			sleep(1);
			gating_refresh();
		}
		if (g_active < 0) {
			kobj_write("/sys/module/vts_bridge/parameters/auto_resume", "1");
			kobj_write("/sys/touchscreen/resume", "1");
		}
	}
	}
	return NULL;
}
static void act(const char *name)
{
	const struct gate *g = gate_for(name);
	size_t i;
	if (!action_allowed(name)) {
		printf("action %s: suppressed (vivo setting off)\n", name);
		fflush(stdout);
		return;
	}
	if (g && g->wake) {
		printf("action %s: wake\n", name);
		fflush(stdout);
		run_cmd("input keyevent KEYCODE_WAKEUP");
		if (!g->lockscreen) {
			run_cmd("wm dismiss-keyguard");
			run_cmd("service call activity_task 65 i32 4 " ">/dev/null 2>&1");
		}
	}
	for (i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
		if (!strcmp(actions[i].label, name)) {
			printf("action %s: %s\n", name, actions[i].cmd);
			fflush(stdout);
			run_cmd(actions[i].cmd);
			return;
		}
	}
	printf("action %s: none\n", name);
	fflush(stdout);
}
static void stroke_reset(struct stroke *s)
{
	s->n = 0;
	s->active = 0;
	s->slot = 0;
	s->t0 = 0;
}
static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int screen_is_off(void)
{
	char val[PROP_VALUE_MAX] = "";
	int fd, n;

	fd = open("/sys/class/vts/vivo_ts/status/lcd_on", O_RDONLY);
	if (fd >= 0) {
		n = read(fd, val, sizeof(val) - 1);
		close(fd);
		if (n > 0) {
			val[n] = 0;
			return atoi(val) == 0;
		}
	}
	__system_property_get("debug.tracing.screen_state", val);
	return val[0] == '1';
}
static int tap_geometry(const struct stroke *s, float *cx, float *cy)
{
	float x0 = s->p[0].x, x1 = x0, y0 = s->p[0].y, y1 = y0;
	long long dur = now_ms() - s->t0;
	int i;
	for (i = 1; i < s->n; i++) {
		if (s->p[i].x < x0) x0 = s->p[i].x;
		if (s->p[i].x > x1) x1 = s->p[i].x;
		if (s->p[i].y < y0) y0 = s->p[i].y;
		if (s->p[i].y > y1) y1 = s->p[i].y;
	}
	*cx = (x0 + x1) * 0.5f;
	*cy = (y0 + y1) * 0.5f;
	if (s->n > 12 || dur > 350)
		return 0;
	return (x1 - x0) <= 500.0f && (y1 - y0) <= 500.0f;
}
static void handle_tap(float cx, float cy)
{
	static long long tap_ms;
	static float tap_x, tap_y;
	static int tap_valid;
	long long t = now_ms();
	if (tap_valid && t - tap_ms <= 600 &&
	    fabsf(cx - tap_x) <= 700.0f && fabsf(cy - tap_y) <= 700.0f) {
		tap_valid = 0;
		if (action_allowed("tap")) {
			printf("double tap -> wake\n");
			fflush(stdout);
			run_cmd("input keyevent KEYCODE_WAKEUP");
		} else {
			printf("double tap: suppressed (setting off)\n");
			fflush(stdout);
		}
		return;
	}
	tap_valid = 1;
	tap_ms = t;
	tap_x = cx;
	tap_y = cy;
}
struct swipe_rule {
	float vert, aspect, straight, net;
	int have;
};
static struct swipe_rule swipe_rule[2];
struct swipe_feat {
	float net, path, w, h, dx, dy, vert, aspect, straight;
};
static void swipe_defaults(void)
{
	swipe_rule[0] = (struct swipe_rule){ 0.70f, 2.50f, 0.75f, 2500.0f, 1 };
	swipe_rule[1] = swipe_rule[0];
}
static int swipe_measure(const struct stroke *s, struct swipe_feat *f)
{
	float x0, x1, y0, y1;
	int i;
	if (s->n < 4)
		return 0;
	x0 = x1 = s->p[0].x;
	y0 = y1 = s->p[0].y;
	f->path = 0;
	for (i = 1; i < s->n; i++) {
		float ax = s->p[i].x - s->p[i - 1].x;
		float ay = s->p[i].y - s->p[i - 1].y;
		f->path += sqrtf(ax * ax + ay * ay);
		if (s->p[i].x < x0) x0 = s->p[i].x;
		if (s->p[i].x > x1) x1 = s->p[i].x;
		if (s->p[i].y < y0) y0 = s->p[i].y;
		if (s->p[i].y > y1) y1 = s->p[i].y;
	}
	f->w = x1 - x0;
	f->h = y1 - y0;
	f->dx = s->p[s->n - 1].x - s->p[0].x;
	f->dy = s->p[s->n - 1].y - s->p[0].y;
	f->net = sqrtf(f->dx * f->dx + f->dy * f->dy);
	if (f->net < 1.0f || f->path < 1.0f)
		return 0;
	f->vert = fabsf(f->dy) / f->net;
	f->aspect = (f->h + 1.0f) / (f->w + 1.0f);
	f->straight = f->net / f->path;
	return 1;
}
static int swipe_direction(const struct stroke *s)
{
	struct swipe_feat f;
	int d;
	if (!swipe_measure(s, &f))
		return -1;
	d = f.dy < 0 ? 0 : 1;
	if (!swipe_rule[d].have)
		return -1;
	if (f.net < swipe_rule[d].net)
		return -1;
	if (f.vert < swipe_rule[d].vert)
		return -1;
	if (f.aspect < swipe_rule[d].aspect)
		return -1;
	if (f.straight < swipe_rule[d].straight)
		return -1;
	return d;
}
static int stroke_is_vertical_swipe(const struct stroke *s, int *up)
{
	int d = swipe_direction(s);
	if (d < 0)
		return 0;
	*up = (d == 0);
	return 1;
}
static void load_swipe_conf(void)
{
	const char *paths[] = { GEST_DIR "/swipes.conf", BASE_DIR "/swipes.conf" };
	FILE *f = NULL;
	char line[256];
	size_t i;
	swipe_defaults();
	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		f = fopen(paths[i], "r");
		if (f)
			break;
	}
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char dir[16];
		float v, a, st, n;
		int d;
		if (sscanf(line, "%15s vert=%f aspect=%f straight=%f net=%f",
			   dir, &v, &a, &st, &n) != 5)
			continue;
		if (!strcmp(dir, "up"))
			d = 0;
		else if (!strcmp(dir, "down"))
			d = 1;
		else
			continue;
		swipe_rule[d] = (struct swipe_rule){ v, a, st, n, 1 };
	}
	fclose(f);
}
#define SWIPE_CAL_MAX 32
static struct swipe_feat cal_up[SWIPE_CAL_MAX], cal_down[SWIPE_CAL_MAX];
static int cal_nup, cal_ndown;
static float cal_pct(const struct swipe_feat *a, int n, int which, float p)
{
	float v[SWIPE_CAL_MAX];
	int i, j, lo, hi;
	float pos, frac;
	for (i = 0; i < n; i++) {
		v[i] = which == 0 ? a[i].vert :
		       which == 1 ? a[i].aspect :
		       which == 2 ? a[i].straight : a[i].net;
	}
	for (i = 1; i < n; i++) {
		float t = v[i];

		for (j = i - 1; j >= 0 && v[j] > t; j--)
			v[j + 1] = v[j];
		v[j + 1] = t;
	}
	pos = p * (n - 1);
	lo = (int)pos;
	hi = lo + 1;
	frac = pos - lo;
	if (hi >= n)
		return v[n - 1];
	return v[lo] + (v[hi] - v[lo]) * frac;
}
static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}
static void swipe_cal_finish(void)
{
	struct swipe_rule up = { 0.70f, 2.50f, 0.75f, 2500.0f, 1 };
	struct swipe_rule dn = up;
	char path[256];
	FILE *f;
	if (cal_nup >= 3) {
		up.vert = clampf(cal_pct(cal_up, cal_nup, 0, 0.25f) * 0.90f, 0.60f, 0.80f);
		up.aspect = clampf(cal_pct(cal_up, cal_nup, 1, 0.25f) * 0.60f, 2.20f, 3.00f);
		up.straight = clampf(cal_pct(cal_up, cal_nup, 2, 0.25f) * 0.90f, 0.70f, 0.82f);
		up.net = clampf(cal_pct(cal_up, cal_nup, 3, 0.25f) * 0.60f, 1500.0f, 4000.0f);
	}
	if (cal_ndown >= 3) {
		dn.vert = clampf(cal_pct(cal_down, cal_ndown, 0, 0.25f) * 0.90f, 0.60f, 0.80f);
		dn.aspect = clampf(cal_pct(cal_down, cal_ndown, 1, 0.25f) * 0.60f, 2.20f, 3.00f);
		dn.straight = clampf(cal_pct(cal_down, cal_ndown, 2, 0.25f) * 0.90f, 0.70f, 0.82f);
		dn.net = clampf(cal_pct(cal_down, cal_ndown, 3, 0.25f) * 0.60f, 1500.0f, 4000.0f);
	}
	ensure_dir(GEST_DIR);
	snprintf(path, sizeof(path), GEST_DIR "/swipes.conf");
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "# learned by `vtsgesture -S`\n");
		fprintf(f, "up vert=%.2f aspect=%.2f straight=%.2f net=%.0f\n", up.vert, up.aspect, up.straight, up.net);
		fprintf(f, "down vert=%.2f aspect=%.2f straight=%.2f net=%.0f\n", dn.vert, dn.aspect, dn.straight, dn.net);
		fclose(f);
	}
	printf("swipe calibration: up   vert=%.2f aspect=%.2f straight=%.2f net=%.0f (n=%d)\n", up.vert, up.aspect, up.straight, up.net, cal_nup);
	printf("swipe calibration: down vert=%.2f aspect=%.2f straight=%.2f net=%.0f (n=%d)\n", dn.vert, dn.aspect, dn.straight, dn.net, cal_ndown);
	fflush(stdout);
}
static int swipe_cal_add(const struct stroke *s)
{
	struct swipe_feat f;
	int d;
	if (!swipe_measure(s, &f)) {
		printf("swipe cal: ignored (too short)\n");
		fflush(stdout);
		return 0;
	}
	if (f.net < 300.0f || f.vert < 0.40f) {
		printf("swipe cal: ignored (net=%.0f vert=%.2f)\n",
		       f.net, f.vert);
		fflush(stdout);
		return 0;
	}
	d = f.dy < 0 ? 0 : 1;
	if (d == 0 ? cal_nup >= SWIPE_CAL_MAX : cal_ndown >= SWIPE_CAL_MAX)
		return 0;
	if (d == 0)
		cal_up[cal_nup++] = f;
	else
		cal_down[cal_ndown++] = f;
	printf("swipe cal %s %d: net=%.0f vert=%.2f aspect=%.2f straight=%.2f\n", d == 0 ? "up" : "down", d == 0 ? cal_nup : cal_ndown, f.net, f.vert, f.aspect, f.straight);
	fflush(stdout);
	return cal_nup >= 8 && cal_ndown >= 8;
}
static int label_cap(const char *name)
{
	if (!strcmp(name, "c"))
		return 220;
	if (!strcmp(name, "e") || !strcmp(name, "o") || !strcmp(name, "w"))
		return 230;
	if (!strcmp(name, "m"))
		return 270;
	return DTW_MAX_X100;
}
static int stroke_big_enough(const struct stroke *s)
{
	float x0, x1, y0, y1, path = 0.0f;
	int i;
	if (s->n < 10)
		return 0;
	x0 = x1 = s->p[0].x;
	y0 = y1 = s->p[0].y;
	for (i = 1; i < s->n; i++) {
		float dx = s->p[i].x - s->p[i - 1].x;
		float dy = s->p[i].y - s->p[i - 1].y;
		path += sqrtf(dx * dx + dy * dy);
		if (s->p[i].x < x0) x0 = s->p[i].x;
		if (s->p[i].x > x1) x1 = s->p[i].x;
		if (s->p[i].y < y0) y0 = s->p[i].y;
		if (s->p[i].y > y1) y1 = s->p[i].y;
	}
	return path >= 2000.0f && (x1 - x0 >= 1200.0f || y1 - y0 >= 1200.0f);
}
static int proximity_covered(void)
{
	char buf[16];
	int fd, n;
	fd = open("/data/vts_gestures/proximity", O_RDONLY);
	if (fd < 0)
		fd = open("/sys/touchscreen/proximity", O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	return atoi(buf) == 0;
}
static void start_prox_helper(void)
{
	if (access("/vendor/etc/vtsprox", R_OK) == 0)
		system("cp -f /vendor/etc/vtsprox /data/local/tmp/vtsprox "
		       "2>/dev/null; chmod 755 /data/local/tmp/vtsprox "
		       "2>/dev/null");
	if (access("/data/local/tmp/vtsprox", X_OK) == 0)
		system("pkill -x vtsprox 2>/dev/null; "
		       "setsid /data/local/tmp/vtsprox </dev/null >/dev/null "
		       "2>&1 &");
}

static void handle_stroke(struct stroke *s, const char *enrol, struct tpl *tpl, int ntpl)
{
	unsigned char sig[SIGLEN];
	int i, best = -1, bestscore = 1 << 30, second = 1 << 30;
	long long match_ms = 0;
	const char *secondlab = NULL;
	if (!stroke_big_enough(s)) {
		stroke_reset(s);
		return;
	}
	if (make_sig(s, sig)) {
		stroke_reset(s);
		return;
	}
	if (enrol) {
		save_sig(enrol, sig);
		stroke_reset(s);
		return;
	}
	{
		int up;
		if (stroke_is_vertical_swipe(s, &up)) {
			printf("gesture=swipe_%s points=%d\n", up ? "up" : "down", s->n);
			fflush(stdout);
			act(up ? "swipe_up" : "swipe_down");
			stroke_reset(s);
			return;
		}
	}
	{
		int closed = sig_closed(sig);
		float dist[MAX_TPL];
		float d1 = 1e9f, d2 = 1e9f;
		int k;
		long long tmatch = now_ms();
		for (i = 0; i < ntpl; i++) {
			dist[i] = dtw_variants(tpl[i].sig, sig, closed);
			if (dist[i] < d1) {
				d1 = dist[i];
				best = i;
			}
		}
		if (best >= 0) {
			for (k = 0; k < ntpl; k++) {
				if (!strcmp(tpl[k].name, tpl[best].name))
					continue;
				if (dist[k] < d2) {
					d2 = dist[k];
					secondlab = tpl[k].name;
				}
			}
		}
		match_ms = now_ms() - tmatch;
		if (best >= 0)
			bestscore = (int)(d1 * 100);
		second = d2 > 1e8f ? (1 << 30) : (int)(d2 * 100);
	}
	if (best >= 0 && bestscore <= label_cap(tpl[best].name) &&
	    (second == (1 << 30) || second - bestscore >= DTW_MARGIN_X100)) {
		printf("gesture=%s dtw=%d.%02d second=%s %d.%02d " "points=%d match=%lldms\n", tpl[best].name, bestscore / 100, bestscore % 100, secondlab ? secondlab : "-", second / 100, second % 100, s->n, match_ms);
		fflush(stdout);
		if (!enrol && bestscore <= 150 &&
		    (second == (1 << 30) || second - bestscore >= 80)) {
			save_sig(tpl[best].name, sig);
			printf("learned %s\n", tpl[best].name);
			fflush(stdout);
		}
		act(tpl[best].name);
	} else {
		printf("no match (best=%s %d.%02d, second=%s %d.%02d, "
		       "points=%d match=%lldms)\n",
		       best >= 0 ? tpl[best].name : "-", bestscore / 100,
		       bestscore % 100, secondlab ? secondlab : "-",
		       second / 100, second % 100, s->n, match_ms);
		fflush(stdout);
	}
	stroke_reset(s);
}
#define BIT_TEST(nr, addr) (((addr)[(nr) / 8] >> ((nr) % 8)) & 1)

static int is_touch_device(int fd)
{
	unsigned char absbits[(ABS_MAX + 8) / 8];
	char name[128] = "";
	if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
		return 0;
	memset(absbits, 0, sizeof(absbits));
	if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0)
		return 0;
	if (!BIT_TEST(ABS_MT_POSITION_X, absbits) ||
	    !BIT_TEST(ABS_MT_POSITION_Y, absbits))
		return 0;
	printf("touchscreen: %s (%s)\n", name, "multitouch");
	fflush(stdout);
	return 1;
}
static int find_touch_device(char *out, size_t len)
{
	int i;
	for (i = 0; i < 32; i++) {
		int fd;
		snprintf(out, len, "/dev/input/event%d", i);
		fd = open(out, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (is_touch_device(fd)) {
			close(fd);
			return 0;
		}
		close(fd);
	}
	return -1;
}

int main(int argc, char **argv)
{
	struct stroke s;
	struct tpl tpl[MAX_TPL];
	int ntpl, fd, enrol_mode = 0, list_mode = 0, test_mode = 0;
	int swipe_cal_mode = 0;
	const char *enrol = NULL;
	void *hal;
	while ((argc > 1) && argv[1][0] == '-') {
		if (!strcmp(argv[1], "-e") && argc > 2) {
			enrol_mode = 1;
			enrol = argv[2];
			argv += 2; argc -= 2;
		} else if (!strcmp(argv[1], "-l")) {
			list_mode = 1;
			argv++; argc--;
		} else if (!strcmp(argv[1], "-t")) {
			test_mode = 1;
			argv++; argc--;
		} else if (!strcmp(argv[1], "-S")) {
			swipe_cal_mode = 1;
			argv++; argc--;
		} else {
			fprintf(stderr, "usage: %s [-e name|-l|-t|-S]\n",
				argv[0]);
			return 2;
		}
	}
	gating_refresh();
	if (enrol_mode || swipe_cal_mode) {
		kobj_write("/sys/module/vts_bridge/parameters/auto_resume", "1");
		kobj_write("/sys/touchscreen/resume", "1");
	} else {
		char pidbuf[16];

		start_prox_helper();
		signal(SIGUSR1, sigusr1);
		snprintf(pidbuf, sizeof(pidbuf), "%d", getpid());
		kobj_write("/sys/touchscreen/watcher_pid", pidbuf);
	}
	hal = dlopen(HAL_SO, RTLD_NOW);
	if (!hal) {
		fprintf(stderr, "dlopen %s: %s\n", HAL_SO, dlerror());
		return 1;
	}
	score_fn = (int (*)(const void *, const void *, int))
		dlsym(hal, "GetScore");
	if (!score_fn) {
		fprintf(stderr, "GetScore not found\n");
		return 1;
	}
	{
		int i;
		for (i = 0; i < 649; i++)
			sqrt_lut[i] = sqrtf((float)i);
	}
	load_swipe_conf();
	ntpl = load_tpl(tpl, MAX_TPL);
	if (test_mode) {
		int i, j;
		printf("self-test, %d templates\n", ntpl);
		fflush(stdout);
		for (i = 0; i < ntpl; i++) {
			for (j = 0; j < ntpl; j++) {
				int sc = score_fn(tpl[i].sig, tpl[j].sig, SCORE_COUNT);

				printf("%s vs %s = %d\n", tpl[i].name, tpl[j].name, sc);
				fflush(stdout);
			}
		}
		return 0;
	}
	if (list_mode) {
		int i;

		printf("%d templates\n", ntpl);
		for (i = 0; i < ntpl; i++)
			printf("  %s\n", tpl[i].name);
		return 0;
	}
	{
		char dev[64];
		int tries;
		for (tries = 0; tries < 30; tries++) {
			if (!find_touch_device(dev, sizeof(dev)))
				break;
			sleep(1);
		}
		if (tries == 30) {
			fprintf(stderr, "no multitouch input device found\n");
			return 1;
		}
		fd = open(dev, O_RDONLY | O_NONBLOCK);
		if (fd < 0) {
			fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
			return 1;
		}
		printf("vtsgesture: %s, %d templates, dev=%s\n",
		       enrol ? "enrol" : "run", ntpl, dev);
	}
	fflush(stdout);
	stroke_reset(&s);
	{
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		long long last_gate = now_ms();

		for (;;) {
			struct input_event ev;
			ssize_t n;
			int pr = poll(&pfd, 1, 5000);

			if (pr < 0) {
				if (errno == EINTR) {
					if (g_wake) {
						g_wake = 0;
						gating_refresh();
						last_gate = now_ms();
					}
					continue;
				}
				break;
			}
			if (pr == 0 || now_ms() - last_gate >= 5000) {
				gating_refresh();
				last_gate = now_ms();
			}
			if (pr == 0)
				continue;
			n = read(fd, &ev, sizeof(ev));
			if (n != sizeof(ev)) {
				if (n < 0 && errno == EAGAIN)
					continue;
				continue;
			}
			if (ev.type == EV_ABS) {
				if (ev.code == ABS_MT_SLOT)
					s.slot = ev.value;
				else if (ev.code == ABS_MT_TRACKING_ID) {
					if (ev.value >= 0) {
						if (s.n)
							stroke_reset(&s);
						s.active = 1;
						s.t0 = now_ms();
					}
				} else if (ev.code == ABS_MT_POSITION_X && s.active &&
					   s.slot == 0) {
					if (s.n < MAX_PTS)
						s.p[s.n].x = ev.value;
				} else if (ev.code == ABS_MT_POSITION_Y && s.active &&
					   s.slot == 0) {
					if (s.n < MAX_PTS)
						s.p[s.n++].y = ev.value;
				}
			} else if (ev.type == EV_KEY && ev.code == BTN_TOUCH &&
				   ev.value == 0) {
				int off = screen_is_off();
				float cx = 0, cy = 0;
				int tap = 0;
				if (swipe_cal_mode) {
					if (s.active && s.n > 3 &&
					    swipe_cal_add(&s)) {
						swipe_cal_finish();
						stroke_reset(&s);
						return 0;
					}
					stroke_reset(&s);
					continue;
				}
				if (s.active && s.n > 0 && off && !enrol_mode &&
				    proximity_covered()) {
					printf("blocked (proximity covered)\n");
					fflush(stdout);
					stroke_reset(&s);
					continue;
				}
				if (s.active && s.n > 0 && off && !enrol_mode)
					tap = tap_geometry(&s, &cx, &cy);
				if (tap) {
					if (g_dt2w)
						handle_tap(cx, cy);
				} else if (s.active && s.n > 3 &&
					   (enrol_mode || (off && g_letters))) {
					handle_stroke(&s, enrol, tpl, ntpl);
				}
				stroke_reset(&s);
			} else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			}
		}
	}
	return 0;
}
