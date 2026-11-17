// SPDX-License-Identifier: MIT
// dfset - call vendor.xiaomi.hardware.displayfeature@1.0::IDisplayFeature over hwbinder.
//🐥
// The generated HIDL client stub exports the two symbols we need:
//   IDisplayFeature::getService(const std::string&, bool)   -> sp<IDisplayFeature>
//   BpHwDisplayFeature::setFeature(int32_t,int32_t,int32_t,int32_t) -> Return<void>
//
// so we dlopen the stub, resolve both, and call setFeature() on the returned
// proxy. This is the real HAL (the one whose displayeffectBase looper runs),
// unlike the local DisplayEffect in /vendor/bin/displayfeature that DisplayFeatureControl drives.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>

#define SYM_GETSERVICE \ "_ZN6vendor6xiaomi8hardware14displayfeature4V1_015IDisplayFeature10getServiceERKNSt3__112basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb"
#define SYM_SETFEATURE \ "_ZN6vendor6xiaomi8hardware14displayfeature4V1_018BpHwDisplayFeature10setFeatureEiiii"

static const char *kStub = "/vendor/lib64/vendor.xiaomi.hardware.displayfeature@1.0.so";

struct SpRet {
	void *p;
	SpRet(const SpRet &o) : p(o.p) {}
	~SpRet() {}
};
struct RetVoid {
	unsigned char buf[64];
	RetVoid() { memset(buf, 0, sizeof(buf)); }
	RetVoid(const RetVoid &o) { memcpy(buf, o.buf, sizeof(buf)); }
	~RetVoid() {}
};
typedef SpRet (*get_service_fn)(const std::string &name, bool get_stub);
typedef RetVoid (*set_feature_fn)(void *self, int display_id, int case_id, int mode_id, int cookie);
#define FI_EXPERT     26
#define FI_COLOR_TEMP 23

static void *g_proxy;
static set_feature_fn g_set;

static int call_feature(int display, int feature, int value, int cookie)
{
	RetVoid ret = g_set(g_proxy, display, feature, value, cookie);
	int status;

	memcpy(&status, ret.buf, sizeof(status));
	return status;
}
int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr,
			"usage: %s <displayId> <caseId> <modeId> <cookie>\n"
			"       %s -e <R> <G> <B> <H> <S> <V> <contrast> <gamma>\n"
			"       %s -t <cct level 1|2|3>\n",
			argv[0], argv[0], argv[0]);
		return 2;
	}
	void *h = dlopen(kStub, RTLD_NOW);
	if (!h) {
		fprintf(stderr, "dlopen(%s): %s\n", kStub, dlerror());
		return 3;
	}
	get_service_fn get_service;
	*(void **)(&get_service) = dlsym(h, SYM_GETSERVICE);
	*(void **)(&g_set) = dlsym(h, SYM_SETFEATURE);
	if (!get_service || !g_set) {
		fprintf(stderr, "dlsym failed\n");
		return 4;
	}
	std::string name = "default";
	SpRet sp = get_service(name, false);
	g_proxy = sp.p;
	if (!g_proxy) {
		fprintf(stderr, "IDisplayFeature::getService(\"default\") -> null\n");
		return 5;
	}
	if (!strcmp(argv[1], "-e")) {
		int v[8] = { 255, 255, 255, 0, 0, 0, 0, 220 };
		int slots[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
		int i, rc = 0;
		for (i = 0; i < 8 && 2 + i < argc; i++)
			v[i] = atoi(argv[2 + i]);
		for (i = 0; i < 8; i++) {
			int st = call_feature(0, FI_EXPERT, v[i], slots[i]);
			if (st && !rc)
				rc = st;
		}
		return rc;
	}
	if (!strcmp(argv[1], "-t")) {
		if (argc < 3)
			return 2;
		return call_feature(0, FI_COLOR_TEMP, atoi(argv[2]), 255);
	}
	if (argc != 5) {
		fprintf(stderr, "usage: %s <displayId> <caseId> <modeId> <cookie>\n", argv[0]);
		return 2;
	}
	return call_feature(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
}
