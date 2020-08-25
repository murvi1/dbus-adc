#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "sensors.h"

/**
 * @brief performs an adc sample read
 * @param value - a pointer to the variable which will store the result
 * @param sensor - pointer to sensor struct
 * @return - veTrue on success, veFalse on error
 */
veBool adcRead(un32 *value, AnalogSensor *sensor)
{
	char file[64];
	char val[16];
	int fd;
	int n;

	snprintf(file, sizeof(file), "in_voltage%d_raw",
			 sensor->interface.adcPin);

	fd = openat(sensor->interface.devfd, file, O_RDONLY);
	if (fd < 0) {
		perror(file);
		return veFalse;
	}

	n = read(fd, val, sizeof(val));
	close(fd);

	if (n <= 0)
		return veFalse;

	if (val[n - 1] != '\n')
		return veFalse;

	*value = strtoul(val, NULL, 0);

	return veTrue;
}

/**
 * @brief a single pole IIR low pass filter
 * @param x - the current sample
 * @param f - filter parameters
 * @return the next filtered value (filter output)
 */
float adcFilter(float x, Filter *f)
{
	int i;

	if (f->sum < 0) {
		for (i = 0; i < FILTER_LEN; i++)
			f->values[i] = x;

		f->sum = FILTER_LEN * x;
	}

	f->sum += x;
	f->sum -= f->values[f->pos];
	f->values[f->pos] = x;
	f->pos = (f->pos + 1) & (FILTER_LEN - 1);

	return f->sum / FILTER_LEN;
}

void adcFilterReset(Filter *f)
{
	f->sum = -1;
}
