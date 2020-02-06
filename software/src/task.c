#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <velib/platform/plt.h>
#include <velib/types/ve_dbus_item.h>
#include <velib/types/ve_values.h>
#include <velib/utils/ve_logger.h>

#include "sensors.h"
#include "task.h"

#define SENSOR_TICKS	2 /* 100ms */

#define CONFIG_FILE	"/etc/venus/dbus-adc.conf"

#define VREF_MIN	1.0
#define VREF_MAX	10.0

#define SCALE_MIN	1023
#define SCALE_MAX	65535

static struct VeItem *consumer;

static void error(const char *file, int line, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "%s:%d: ", file, line);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	exit(1);
}

static char *token(char *buf, char **next)
{
	char *end;

	while (isspace(*buf))
		buf++;

	if (!*buf)
		return NULL;

	end = buf + 1;

	while (*end && !isspace(*end))
		end++;

	if (*end)
		*end++ = 0;

	*next = end;

	return buf;
}

static float get_float(const char *p, float min, float max,
					   const char *file, int line)
{
	char *end;
	float v = strtof(p, &end);

	if (*end)
		error(file, line, "invalid number '%s'\n", p);

	if (!(v >= min && v <= max)) /* also catch NaN */
		error(file, line, "value out of range [%f, %f]\n", min, max);

	return v;
}

static unsigned get_uint(const char *p, unsigned min, unsigned max,
						 const char *file, int line)
{
	char *end;
	unsigned v = strtoul(p, &end, 0);

	if (*end)
		error(file, line, "invalid number '%s'\n", p);

	if (v < min || v > max)
		error(file, line, "value out of range [%u, %u]\n", min, max);

	return v;
}

static int open_dev(const char *dev, const char *file, int line)
{
	char buf[64];
	int fd;

	snprintf(buf, sizeof(buf), "/sys/bus/iio/devices/%s", dev);

	fd = open(buf, O_RDONLY);
	if (fd < 0)
		error(file, line, "bad device '%s'\n", dev);

	return fd;
}

static void load_config(const char *file)
{
	FILE *f;
	char buf[128];
	int devfd = -1;
	float vref = 0;
	unsigned scale = 0;
	int line = 0;
	int type;
	int pin;

	f = fopen(file, "r");
	if (!f)
		error(file, 0, "%s\n", strerror(errno));

	while (fgets(buf, sizeof(buf), f)) {
		char *cmd, *arg, *rest;
		char *p = buf;

		line++;

		if (!strchr(p, '\n'))
			error(file, line, "line too long\n");

		cmd = strchr(p, '#');
		if (cmd)
			*cmd = 0;

		cmd = token(p, &p);
		if (!cmd)
			continue;

		arg = token(p, &p);
		if (!arg)
			error(file, line, "missing value\n");

		rest = token(p, &p);
		if (rest)
			error(file, line, "trailing junk\n");

		if (!strcmp(cmd, "device")) {
			devfd = open_dev(arg, file, line);
			continue;
		}

		if (!strcmp(cmd, "vref")) {
			vref = get_float(arg, VREF_MIN, VREF_MAX, file, line);
			continue;
		}

		if (!strcmp(cmd, "scale")) {
			scale = get_uint(arg, SCALE_MIN, SCALE_MAX, file, line);
			continue;
		}

		if (!strcmp(cmd, "tank"))
			type = SENSOR_TYPE_TANK;
		else if (!strcmp(cmd, "temp"))
			type = SENSOR_TYPE_TEMP;
		else
			error(file, line, "unknown directive\n");

		if (devfd < 0)
			error(file, line, "%s requires device\n", cmd);

		if (!vref)
			error(file, line, "%s requires vref\n", cmd);

		if (!scale)
			error(file, line, "%s requires scale\n", cmd);

		pin = get_uint(arg, 0, -1u, file, line);

		if (add_sensor(devfd, pin, vref / scale, type))
			error(file, line, "error adding sensor\n");
	}
}

void values_dbus_service_connectSettings(void)
{
	const char *settingsService = "com.victronenergy.settings";
	struct VeItem *input_root = veValueTree();
	struct VeDbus *dbus;

	if (!(dbus = veDbusGetDefaultBus())) {
		printf("dbus connection failed\n");
		pltExit(5);
	}
	veDbusSetListeningDbus(dbus);

	/* Connect to settings service */
	consumer = veItemGetOrCreateUid(input_root, settingsService);
	if (!veDbusAddRemoteService(settingsService, consumer, veTrue)) {
		logE("task", "veDbusAddRemoteService failed");
		pltExit(1);
	}
}

struct VeItem *getConsumerRoot(void)
{
	return consumer;
}

/**
 * @brief taskInit
 * initiate the system and enable the interrupts to start ticking the app
 */
void taskInit(void)
{
	// Connect to settings service to dbus
	values_dbus_service_connectSettings();

	load_config(CONFIG_FILE);
}

void taskUpdate(void)
{
	// Not in use
}

/* 50 ms time update. */
void taskTick(void)
{
	static un16 values_task_timer = SENSOR_TICKS;

	if (--values_task_timer == 0) {
		values_task_timer = SENSOR_TICKS;
		sensors_handle();
	}
}

char const *pltProgramVersion(void)
{
	return "1.18";
}
