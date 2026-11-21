// SPDX-License-Identifier: MIT
// vtsprox - proximity watcher for vtsgesture
/*
 * It must run from /data (default linker namespace). vendor binaries cannot
 * link libandroid.so. vtsgesture copies it from /vendor/etc and starts it.
 */
#include <android/sensor.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATE_FILE "/data/vts_gestures/proximity"
#define RESUME_NODE "/sys/touchscreen/resume"
#define PROX_THRESHOLD 5.0f
#define SPURIOUS_LIMIT 3
#define BACKOFF_MIN_US 1000000
#define BACKOFF_MAX_US 60000000

static ASensorManager *sensor_mgr;
static const ASensor *prox_sensor;
static struct ASensorEventQueue *prox_queue;
static int last_covered = -1;

static void resume_touch(void)
{
	int fd = open(RESUME_NODE, O_WRONLY);
	if (fd >= 0) {
		write(fd, "1", 1);
		close(fd);
	}
}
static void write_file(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);

	if (fd >= 0) {
		write(fd, s, strlen(s));
		close(fd);
	}
}
static int open_channel(ALooper *looper)
{
	prox_queue = ASensorManager_createEventQueue(sensor_mgr, looper, 0, NULL, NULL);
	if (!prox_queue)
		return 0;
	if (ASensorEventQueue_enableSensor(prox_queue, prox_sensor) < 0) {
		ASensorManager_destroyEventQueue(sensor_mgr, prox_queue);
		prox_queue = NULL;
		return 0;
	}
	ASensorEventQueue_setEventRate(prox_queue, prox_sensor, 200000);
	return 1;
}
static void close_channel(void)
{
	if (prox_queue) {
		ASensorEventQueue_disableSensor(prox_queue, prox_sensor);
		ASensorManager_destroyEventQueue(sensor_mgr, prox_queue);
		prox_queue = NULL;
	}
}
int main(void)
{
	ALooper *looper;
	long backoff = BACKOFF_MIN_US;
	int spurious = 0;
	int i;
	mkdir("/data/vts_gestures", 0777);
	for (i = 0; i < 30 && !prox_sensor; i++) {
		sensor_mgr = ASensorManager_getInstanceForPackage("vtsprox");
		if (sensor_mgr)
			prox_sensor = ASensorManager_getDefaultSensor(sensor_mgr, ASENSOR_TYPE_PROXIMITY);
		if (!prox_sensor)
			sleep(1);
	}
	if (!prox_sensor)
		return 1;
	looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
	if (!looper)
		return 1;
	for (i = 0; i < 5 && !prox_queue; i++) {
		if (!open_channel(looper))
			sleep(1);
	}
	if (!prox_queue)
		return 1;
	for (;;) {
		struct ASensorEvent ev;
		int got = 0;
		int ident;
		ident = ALooper_pollOnce(-1, NULL, NULL, NULL);
		if (ident == ALOOPER_POLL_ERROR)
			goto reconnect;
		while (ASensorEventQueue_getEvents(prox_queue, &ev, 1) > 0) {
			int covered = ev.distance < PROX_THRESHOLD;
			got = 1;
			if (covered != last_covered) {
				write_file(STATE_FILE, covered ? "0\n" : "1\n");
				if (!covered)
					resume_touch();
				last_covered = covered;
			}
		}
		if (got) {
			spurious = 0;
			backoff = BACKOFF_MIN_US;
			continue;
		}
		if (++spurious < SPURIOUS_LIMIT)
			continue;
reconnect:
		close_channel();
		usleep(backoff);
		if (backoff < BACKOFF_MAX_US)
			backoff *= 2;
		spurious = 0;
		while (!open_channel(looper)) {
			usleep(backoff);
			if (backoff < BACKOFF_MAX_US)
				backoff *= 2;
		}
	}
	return 0;
}
//🐥