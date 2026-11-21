// SPDX-License-Identifier: MIT
// vivodispcolor_shim - give the old vivo display-colour HAL the transactions
// OriginOS 6 framework expects, by mapping them onto the Xiaomi PCC.
#define LOG_TAG "vivodisp-shim"

#include <uchar.h>
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define TX_SET_GAME_DISPLAY_ENHANCE 30
#define TX_PERFORM_GAME_COLOR_ENHANCE 38
#define RESET_VALUE (-65536)

extern binder_status_t AServiceManager_addService(AIBinder *binder, const char *instance);

static AIBinder *g_orig;
static AIBinder_Class *g_class;

struct pcc_req {
	int on;
	int h, s, v, c;
};
#define GAME_PCC_PATH "/data/vendor/display/game_pcc"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static struct pcc_req g_req;
static int g_pending;
static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}
static void pcc_exec(const struct pcc_req *q)
{
	char buf[64];
	int fd, n;
	n = snprintf(buf, sizeof(buf), "%d %d %d %d %d\n", q->on, q->h, q->s, q->v, q->c);
	fd = open(GAME_PCC_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		LOGE("open(" GAME_PCC_PATH "): %s", strerror(errno));
		return;
	}
	if (write(fd, buf, (size_t)n) != n)
		LOGE("write(" GAME_PCC_PATH "): %s", strerror(errno));
	close(fd);
}
static void *pcc_worker(void *arg)
{
	(void)arg;
	for (;;) {
		struct pcc_req q;
		pthread_mutex_lock(&g_lock);
		while (!g_pending)
			pthread_cond_wait(&g_cond, &g_lock);
		q = g_req;
		g_pending = 0;
		pthread_mutex_unlock(&g_lock);
		pcc_exec(&q);
	}
	return NULL;
}
static void pcc_post(const struct pcc_req *q)
{
	pthread_mutex_lock(&g_lock);
	g_req = *q;
	g_pending = 1;
	pthread_cond_signal(&g_cond);
	pthread_mutex_unlock(&g_lock);
}
static int32_t find_args_offset(const AParcel *in)
{
	int32_t off;
	for (off = 0; off <= 16; off += 4) {
		int32_t len = 0, i;
		int printable = 1;
		int32_t pos;
		AParcel_setDataPosition(in, off);
		if (AParcel_readInt32(in, &len) != STATUS_OK)
			continue;
		if (len < 1 || len > 200)
			continue;
		for (i = 0; i < len && i < 12; i++) {
			char16_t c = 0;
			if (AParcel_readChar(in, &c) != STATUS_OK ||
			    c < 0x20 || c > 0x7e) {
				printable = 0;
				break;
			}
		}
		if (!printable)
			continue;
		pos = off + 4 + (len + 1) * 2;
		return (pos + 3) & ~3;
	}
	return -1;
}
static void skip_interface_token(const AParcel *in)
{
	int32_t pos = find_args_offset(in);
	AParcel_setDataPosition(in, pos < 0 ? 0 : pos);
}
static void game_display_enhance(const AParcel *in)
{
	int32_t hue = 0, gamma = -1, lut = -1, display = 0;
	float sat = 0, contrast = 0, value = 0;
	struct pcc_req q;
	skip_interface_token(in);
	if (AParcel_readInt32(in, &hue) != STATUS_OK ||
	    AParcel_readFloat(in, &sat) != STATUS_OK ||
	    AParcel_readFloat(in, &contrast) != STATUS_OK ||
	    AParcel_readFloat(in, &value) != STATUS_OK ||
	    AParcel_readInt32(in, &gamma) != STATUS_OK ||
	    AParcel_readInt32(in, &lut) != STATUS_OK ||
	    AParcel_readInt32(in, &display) != STATUS_OK) {
		LOGE("tx30: short parcel");
		return;
	}
	if (hue == RESET_VALUE && gamma < 0 && lut < 0) {
		q.on = 0;
		q.h = q.s = q.v = q.c = 0;
		LOGI("tx30 reset");
		pcc_post(&q);
		return;
	}

	/* derived presets: the controller sends reset params plus the LUT id */
	if (gamma == 1 || gamma == 2 || gamma == 3 || gamma == 4 || lut == 256) {
		q.on = 1;
		q.h = 0;
		q.s = 0;
		q.v = 0;
		q.c = 0;
		switch (gamma) {
		case 1: /* dark enhancement */
			q.v = 40; q.c = 10;
			break;
		case 2: /* anti-glare */
			q.v = -20; q.c = -15;
			break;
		case 3: /* old movies */
			q.c = 5;
			break;
		case 4: /* PQ enhance */
			q.s = 25; q.c = 20;
			break;
		default: /* night vision 3D LUT */
			if (lut == 256) {
				q.s = 15; q.v = 10;
			}
			break;
		}
		LOGI("tx30 preset gamma=%d lut=%d -> v=%d s=%d c=%d",
		     gamma, lut, q.v, q.s, q.c);
		pcc_post(&q);
		return;
	}
	q.on = 1;
	q.h = clamp(hue, -180, 180);
	q.s = clamp((int)(sat * 100.0f + (sat >= 0 ? 0.5f : -0.5f)), -50, 50);
	q.c = clamp((int)(contrast * 100.0f + (contrast >= 0 ? 0.5f : -0.5f)), -100, 100);
	q.v = clamp((int)(value + (value >= 0 ? 0.5f : -0.5f)), -255, 255);
	LOGI("tx30 h=%d s=%.3f c=%.3f v=%.1f -> H=%d S=%d C=%d V=%d", hue, sat, contrast, value, q.h, q.s, q.c, q.v);
	pcc_post(&q);
}
static void perform_game_color_enhance(const AParcel *in)
{
	int32_t state = 0, frames = 0, rate = 0, display = 0;
	struct pcc_req q;
	skip_interface_token(in);
	if (AParcel_readInt32(in, &state) != STATUS_OK ||
	    AParcel_readInt32(in, &frames) != STATUS_OK ||
	    AParcel_readInt32(in, &rate) != STATUS_OK ||
	    AParcel_readInt32(in, &display) != STATUS_OK) {
		LOGE("tx38: short parcel");
		return;
	}
	q.on = state > 0 ? 1 : 0;
	q.h = 0;
	q.c = 0;
	q.s = state > 0 ? 12 : 0;
	q.v = state > 0 ? 5 : 0;
	LOGI("tx38 state=%d -> on=%d S=%d V=%d", state, q.on, q.s, q.v);
	pcc_post(&q);
}
static void *on_create(void *args)
{
	(void)args;
	return NULL;
}
static void on_destroy(void *userData)
{
	(void)userData;
}
static binder_status_t on_transact(AIBinder *binder, transaction_code_t code, const AParcel *in, AParcel *out)
{
	(void)binder;
	switch (code) {
	case TX_SET_GAME_DISPLAY_ENHANCE:
		game_display_enhance(in);
		return STATUS_OK;
	case TX_PERFORM_GAME_COLOR_ENHANCE:
		perform_game_color_enhance(in);
		return STATUS_OK;
	case 31: /* setCABSEnableStrength(int,int) */
	case 33: /* setSre3DLutAlgo(float,int,int,int) */
	case 37: /* performApOD(int,int) */
		return STATUS_OK;
	case 32: /* cmdForFactory(int,int) -> int */
		AParcel_writeInt32(out, 0);
		return STATUS_OK;
	case 36: /* setDisplayedRgbContentSamplingEnabled(...) -> bool */
		AParcel_writeBool(out, false);
		return STATUS_OK;
	default:
		break;
	}
	if (code == 13 || code == 19 || code == 20 || code == 21)
		LOGI("forward tx %u", code);
	{
		AParcel *in2 = NULL;
		AParcel *out2 = NULL;
		binder_status_t st;
		int32_t off = find_args_offset(in);
		if (off < 0)
			return STATUS_UNKNOWN_ERROR;
		st = AIBinder_prepareTransaction(g_orig, &in2);
		if (st != STATUS_OK || in2 == NULL)
			return st != STATUS_OK ? st : STATUS_UNKNOWN_ERROR;
		AParcel_setDataPosition(in, off);
		for (;;) {
			int32_t w;
			if (AParcel_readInt32(in, &w) != STATUS_OK)
				break;
			if (AParcel_writeInt32(in2, w) != STATUS_OK) {
				AParcel_delete(in2);
				return STATUS_UNKNOWN_ERROR;
			}
		}
		st = AIBinder_transact(g_orig, code, &in2, &out2, 0);
		if (st == STATUS_OK && out2 != NULL) {
			AParcel_setDataPosition(out2, 0);
			for (;;) {
				int32_t w;
				if (AParcel_readInt32(out2, &w) != STATUS_OK)
					break;
				if (AParcel_writeInt32(out, w) != STATUS_OK)
					break;
			}
			AParcel_delete(out2);
		}
		return st;
	}
}
__attribute__((visibility("default")))
binder_status_t AServiceManager_addService(AIBinder *binder, const char *instance)
{
	static binder_status_t (*real)(AIBinder *, const char *);
	AIBinder *wrapper;
	if (real == NULL) {
		real = (binder_status_t (*)(AIBinder *, const char *))
			dlsym(RTLD_NEXT, "AServiceManager_addService");
		if (real == NULL) {
			LOGE("dlsym(AServiceManager_addService) failed");
			return STATUS_UNKNOWN_ERROR;
		}
	}
	if (instance == NULL || strstr(instance, "vivodispcolor") == NULL)
		return real(binder, instance);
	LOGI("wrapping %s", instance);
	AIBinder_incStrong(binder);
	g_orig = binder;
	if (g_class == NULL) {
		g_class = AIBinder_Class_define(
			"vendor.vivo.hardware.vivodispcolor.IVivoDispColorAdapter",
			on_create, on_destroy, on_transact);
		if (g_class == NULL) {
			LOGE("AIBinder_Class_define failed lazily");
			return real(binder, instance);
		}
	}
	wrapper = AIBinder_new(g_class, NULL);
	if (wrapper == NULL) {
		LOGE("AIBinder_new failed");
		return real(binder, instance);
	}
	return real(wrapper, instance);
}
__attribute__((constructor))
static void shim_init(void)
{
	pthread_t th;
	int err;
	err = pthread_create(&th, NULL, pcc_worker, NULL);
	if (err != 0)
		LOGE("pthread_create: %s", strerror(err));
	else
		pthread_detach(th);
	LOGI("loaded");
}
