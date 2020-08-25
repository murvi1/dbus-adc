#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

#include <velib/platform/plt.h>
#include <velib/types/ve_dbus_item.h>
#include <velib/types/ve_item.h>
#include <velib/utils/ve_item_utils.h>
#include <velib/utils/ve_logger.h>
#include <velib/vecan/products.h>

#include "sensors.h"

#define INSTANCE_BASE						20

#define MAX_SENSORS							32

// defines for the tank level sensor analog front end parameters
#define TANK_SENS_VREF						5.0
#define TANK_SENS_R1						680.0 // ohms
#define TANK_MAX_RESISTANCE					264 // ohms
#define TANK_VOLT_R1						30  // kohms
#define TANK_VOLT_R2						120 // okohms
#define TANK_CURRENT_R						47  // ohms

#define EUR_MIN_TANK_LEVEL_RESISTANCE		0 // ohms
#define EUR_MAX_TANK_LEVEL_RESISTANCE		180 // ohms
#define USA_MIN_TANK_LEVEL_RESISTANCE		240 // ohms
#define USA_MAX_TANK_LEVEL_RESISTANCE		30 // ohms

// defines for the temperature sensor analog front end parameters
#define TEMP_SENS_R1						10000.0 // ohms
#define TEMP_SENS_R2						4700.0  // ohms
#define TEMP_SENS_V_RATIO					((TEMP_SENS_R1 + TEMP_SENS_R2) / TEMP_SENS_R2)
#define TEMP_SENS_MAX_ADCIN					1.3 // ~400K
#define TEMP_SENS_MIN_ADCIN					0.8 // ~(-22) degrees C
#define TEMP_SENS_S_C_ADCIN					0.02
#define TEMP_SENS_INV_PLRTY_ADCIN			0.208 // 0.7 volts at divider input
#define TEMP_SENS_INV_PLRTY_ADCIN_BAND		0.15
#define TEMP_SENS_INV_PLRTY_ADCIN_LB		(TEMP_SENS_INV_PLRTY_ADCIN - TEMP_SENS_INV_PLRTY_ADCIN_BAND)
#define TEMP_SENS_INV_PLRTY_ADCIN_HB		(TEMP_SENS_INV_PLRTY_ADCIN + TEMP_SENS_INV_PLRTY_ADCIN_BAND)

static AnalogSensor *sensors[MAX_SENSORS];
static int sensorCount;

static VeVariantUnitFmt veUnitVolume = {3, "m3"};
static VeVariantUnitFmt veUnitCelsius0Dec = {0, "C"};
static VeVariantUnitFmt unitRes0Dec = {0, "ohm"};

/* Tank sensor */
static struct VeSettingProperties tankCapacityProps = {
	.type = VE_FLOAT,
	.def.value.Float = 0.2f /* m3 */,
	.max.value.Float = 1000.0f,
};

static struct VeSettingProperties tankFluidType = {
	.type = VE_SN32,
	.max.value.SN32 = INT32_MAX - 3,
};

static struct VeSettingProperties tankStandardProps = {
	.type = VE_SN32,
	.max.value.SN32 = TANK_STANDARD_COUNT - 1,
};

/* Temperature sensor */
static struct VeSettingProperties scaleProps = {
	.type = VE_FLOAT,
	.def.value.Float = 1.0f,
	.min.value.Float = 0.1f,
	.max.value.Float = 10.0f,
};

static struct VeSettingProperties offsetProps = {
	.type = VE_FLOAT,
	.min.value.Float = -100.0f,
	.max.value.Float = 100.0f,
};

static struct VeSettingProperties temperatureType = {
	.type = VE_SN32,
	.max.value.SN32 = INT32_MAX - 3,
};

static struct VeSettingProperties emptyStrType = {
	.type = VE_HEAP_STR,
	.def.value.Ptr = "",
};

static struct VeSettingProperties tankRangeProps = {
	.type = VE_FLOAT,
	.max.value.Float = TANK_MAX_RESISTANCE,
};

static struct VeSettingProperties tankSenseProps = {
	.type = VE_SN32,
	.def.value.SN32 = TANK_SENSE_VOLTAGE,
	.min.value.SN32 = TANK_SENSE_VOLTAGE,
	.max.value.SN32 = TANK_SENSE_COUNT - 1,
};

VeVariantEnumFmt const statusDef =
		VE_ENUM_DEF("Ok", "Disconnected",  "Short circuited",
					"Reverse polarity", "Unknown");
VeVariantEnumFmt const fluidTypeDef =
		VE_ENUM_DEF("Fuel", "Fresh water", "Waste water", "Live well",
					"Oil", "Black water (sewage)");
VeVariantEnumFmt const standardDef =
		VE_ENUM_DEF("European", "American", "Custom");
VeVariantEnumFmt const functionDef = VE_ENUM_DEF("None", "Default");

static struct VeItem *createEnumItem(AnalogSensor *sensor, const char *id,
		VeVariant *initial, const VeVariantEnumFmt *fmt, VeItemSetterFun *cb)
{
	struct VeItem *item = veItemCreateBasic(sensor->root, id, initial);
	veItemSetSetter(item, cb, sensor);
	if (fmt)
		veItemSetFmt(item, veVariantEnumFmt, fmt);

	return item;
}

/*
 * The settings of a sensor service are stored in localsettings, so when
 * the sensor value changes, send it to localsettings and if the setting
 * in localsettings changed, also update the sensor value.
 */
static struct VeItem *createSettingsProxy(struct VeItem *root,
		const char *prefix, char *settingsId, VeItemValueFmt *fmt,
		const void *fmtCtx, struct VeSettingProperties *props, char *serviceId)
{
	struct VeItem *localSettings = getLocalSettings();
	struct VeItem *sensorItem;

	if (serviceId == NULL)
		serviceId = settingsId;

	sensorItem = veItemCreateSettingsProxyId(localSettings, prefix, root,
			settingsId, fmt, fmtCtx, props, serviceId);
	if (!sensorItem) {
		logE("task", "veItemCreateSettingsProxy failed");
		pltExit(1);
	}
	return sensorItem;
}

static void createControlItems(AnalogSensor *sensor, const char *devid,
							   const char *prefix, SensorInfo *s)
{
	struct VeSettingProperties functionProps = {
		.type = VE_SN32,
		.def.value.SN32 = s->func_def,
		.max.value.SN32 = SENSOR_FUNCTION_COUNT - 1,
	};
	struct VeItem *root = getDbusRoot();
	char name[VE_MAX_UID_SIZE];
	VeVariant v;

	snprintf(name, sizeof(name), "Devices/%s/Function", devid);
	sensor->function = createSettingsProxy(root, prefix, "Function",
			veVariantEnumFmt, &functionDef, &functionProps, name);

	snprintf(name, sizeof(name), "Devices/%s/Label", devid);
	veItemCreateBasic(root, name, veVariantStr(&v, sensor->ifaceName));
}

static void setTankLevels(struct TankSensor *tank, sn32 empty, sn32 full)
{
	struct VeItem *settingsItem;
	VeVariant v;

	settingsItem = veItemCtxSet(tank->emptyRItem);
	if (veVariantIsValid(veItemLocalValue(settingsItem, &v)) &&
		v.value.Float != empty)
		veItemSet(settingsItem, veVariantSn32(&v, empty));

	settingsItem = veItemCtxSet(tank->fullRItem);
	if (veVariantIsValid(veItemLocalValue(settingsItem, &v)) &&
		v.value.Float != full)
		veItemSet(settingsItem, veVariantSn32(&v, full));
}

/*
 * Keep the settings in sync. The gui shouldn't allow changing the
 * resistance settings when not in custom mode, but external might
 * try. So always makes sure they match.
 */
static void onTankResConfigChanged(struct VeItem *item)
{
	struct TankSensor *tank = (struct TankSensor *) veItemCtx(item)->ptr;
	VeVariant standard;
	sn32 tankEmptyR, tankFullR;

	if (!veVariantIsValid(veItemLocalValue(tank->standardItem, &standard)))
		return;

	switch (standard.value.SN32) {
	case TANK_STANDARD_EU:
		tankEmptyR = EUR_MIN_TANK_LEVEL_RESISTANCE;
		tankFullR = EUR_MAX_TANK_LEVEL_RESISTANCE;
		break;
	case TANK_STANDARD_US:
		tankEmptyR = USA_MIN_TANK_LEVEL_RESISTANCE;
		tankFullR = USA_MAX_TANK_LEVEL_RESISTANCE;
		break;
	default:
		return;
	}

	setTankLevels(tank, tankEmptyR, tankFullR);
}

static void onTankShapeChanged(struct VeItem *item)
{
	struct TankSensor *tank = (struct TankSensor *) veItemCtx(item)->ptr;
	VeVariant shape;
	const char *map;
	int i;

	if (!veVariantIsValid(veItemLocalValue(tank->shapeItem, &shape))) {
		logE("tank", "invalid shape value");
		goto reset;
	}

	map = shape.value.Ptr;

	if (!map[0])
		goto reset;

	tank->shapeMap[0][0] = 0;
	tank->shapeMap[0][1] = 0;
	i = 1;

	while (i < TANK_SHAPE_MAX_POINTS) {
		unsigned int s, l;

		if (sscanf(map, "%u:%u", &s, &l) < 2) {
			logE("tank", "malformed shape spec");
			goto reset;
		}

		if (s < 1 || s > 99 || l < 1 || l > 99) {
			logE("tank", "shape level out of range 1-99");
			goto reset;
		}

		if (s <= tank->shapeMap[i - 1][0] ||
			l <= tank->shapeMap[i - 1][1]) {
			logE("tank", "shape level non-increasing");
			goto reset;
		}

		tank->shapeMap[i][0] = s / 100.0;
		tank->shapeMap[i][1] = l / 100.0;
		i++;

		map = strchr(map, ',');
		if (!map)
			break;

		map++;
	}

	tank->shapeMap[i][0] = 1;
	tank->shapeMap[i][1] = 1;
	tank->shapeMapLen = i + 1;

	return;

reset:
	tank->shapeMapLen = 0;
}

static int setGpio(int gpio, int val)
{
	char file[64];
	int fd;

	snprintf(file, sizeof(file), "/sys/class/gpio/gpio%d/value", gpio);

	fd = open(file, O_WRONLY);
	if (fd < 0) {
		perror(file);
		return -1;
	}

	write(fd, val ? "1" : "0", 1);
	close(fd);

	return 0;
}

static void onTankSenseChanged(struct VeItem *item)
{
	struct TankSensor *tank = veItemCtx(item)->ptr;
	VeVariant sense, v;
	const char *unit;
	int gpioVal;
	int minVal;
	int maxVal;

	if (!veVariantIsValid(veItemLocalValue(tank->senseTypeItem, &sense)))
		return;

	switch (sense.value.SN32) {
	case TANK_SENSE_VOLTAGE:
		gpioVal = 0;
		unit = "V";
		minVal = 0;
		maxVal = 10;
		break;
	case TANK_SENSE_CURRENT:
		gpioVal = 1;
		unit = "mA";
		minVal = 4;
		maxVal = 20;
		break;
	default:
		return;
	}

	setGpio(tank->sensor.interface.gpio, gpioVal);
	tank->senseType = sense.value.SN32;
	veItemSet(tank->sensor.rawUnitItem, veVariantStr(&v, unit));
	veItemSet(tank->standardItem, veVariantSn32(&v, TANK_STANDARD_CUSTOM));
	setTankLevels(tank, minVal, maxVal);
}

static void createItems(AnalogSensor *sensor, const char *devid, SensorInfo *s)
{
	VeVariant v;
	struct VeItem *root = sensor->root;
	char prefix[VE_MAX_UID_SIZE];

	snprintf(prefix, sizeof(prefix), "Settings/Devices/%s", devid);

	createControlItems(sensor, devid, prefix, s);

	/* App info */
	veItemCreateBasic(root, "Mgmt/ProcessName",
					  veVariantStr(&v, pltProgramName()));
	veItemCreateBasic(root, "Mgmt/ProcessVersion",
					  veVariantStr(&v, pltProgramVersion()));
	veItemCreateBasic(root, "Mgmt/Connection",
					  veVariantStr(&v, sensor->ifaceName));

	veItemCreateProductId(root, s->product_id);
	veItemCreateBasic(root, "ProductName",
			veVariantStr(&v, veProductGetName(s->product_id)));
	if (sensor->serial[0])
		veItemCreateBasic(root, "Serial", veVariantStr(&v, sensor->serial));
	veItemCreateBasic(root, "Connected", veVariantUn32(&v, veTrue));
	veItemCreateBasic(root, "DeviceInstance",
					  veVariantUn32(&v, sensor->instance));
	sensor->statusItem = createEnumItem(sensor, "Status",
			veVariantUn32(&v, SENSOR_STATUS_NOT_CONNECTED), &statusDef, NULL);

	createSettingsProxy(root, prefix, "CustomName", veVariantFmt, &veUnitNone,
						&emptyStrType, NULL);

	sensor->rawValueItem = veItemCreateBasic(root, "RawValue",
			veVariantInvalidType(&v, VE_FLOAT));
	sensor->rawUnitItem = veItemCreateBasic(root, "RawUnit",
			veVariantInvalidType(&v, VE_HEAP_STR));

	if (sensor->sensorType == SENSOR_TYPE_TANK) {
		struct TankSensor *tank = (struct TankSensor *) sensor;

		tank->levelItem = veItemCreateQuantity(root, "Level",
				veVariantInvalidType(&v, VE_UN32), &veUnitPercentage);
		tank->remaingItem = veItemCreateQuantity(root, "Remaining",
				veVariantInvalidType(&v, VE_FLOAT), &veUnitVolume);

		tank->capacityItem = createSettingsProxy(root, prefix, "Capacity",
				veVariantFmt, &veUnitVolume, &tankCapacityProps, NULL);
		tank->fluidTypeItem = createSettingsProxy(root, prefix, "FluidType2",
				veVariantEnumFmt, &fluidTypeDef, &tankFluidType, "FluidType");

		/* The callback will make sure these are kept in sync */
		tank->emptyRItem = createSettingsProxy(root, prefix, "RawValueEmpty",
				veVariantFmt, &unitRes0Dec, &tankRangeProps,  NULL);
		veItemCtx(tank->emptyRItem)->ptr = tank;
		veItemSetChanged(tank->emptyRItem, onTankResConfigChanged);

		tank->fullRItem = createSettingsProxy(root, prefix, "RawValueFull",
				veVariantFmt, &unitRes0Dec, &tankRangeProps, NULL);
		veItemCtx(tank->fullRItem)->ptr = tank;
		veItemSetChanged(tank->fullRItem, onTankResConfigChanged);

		tank->standardItem = createSettingsProxy(root, prefix, "Standard2",
				veVariantEnumFmt, &standardDef, &tankStandardProps, "Standard");
		veItemCtx(tank->standardItem)->ptr = tank;
		veItemSetChanged(tank->standardItem, onTankResConfigChanged);

		tank->shapeItem = createSettingsProxy(root, prefix, "Shape",
				veVariantFmt, &veUnitNone, &emptyStrType, NULL);
		veItemCtx(tank->shapeItem)->ptr = tank;
		veItemSetChanged(tank->shapeItem, onTankShapeChanged);

		if (sensor->interface.gpio > 0) {
			tank->senseTypeItem = createSettingsProxy(root, prefix, "SenseType",
					veVariantFmt, &veUnitNone, &tankSenseProps, NULL);
			veItemCtx(tank->senseTypeItem)->ptr = tank;
			veItemSetChanged(tank->senseTypeItem, onTankSenseChanged);
		} else {
			veItemSet(sensor->rawUnitItem, veVariantStr(&v, "Ω"));
		}
	} else if (sensor->sensorType == SENSOR_TYPE_TEMP) {
		struct TemperatureSensor *temp = (struct TemperatureSensor *) sensor;

		temp->temperatureItem = veItemCreateQuantity(root, "Temperature",
				veVariantInvalidType(&v, VE_SN32), &veUnitCelsius0Dec);

		temp->scaleItem = createSettingsProxy(root, prefix, "Scale",
				veVariantFmt, &veUnitNone, &scaleProps, NULL);
		temp->offsetItem = createSettingsProxy(root, prefix, "Offset",
				veVariantFmt, &veUnitNone, &offsetProps, NULL);
		createSettingsProxy(root, prefix, "TemperatureType2",
				veVariantFmt, &veUnitNone, &temperatureType, "TemperatureType");

		veItemSet(sensor->rawUnitItem, veVariantStr(&v, "V"));
	}
}

static void tankInit(AnalogSensor *sensor, const char *devid)
{
	SensorDbusInterface *dbus = &sensor->interface.dbus;

	snprintf(dbus->service, sizeof(dbus->service),
			 "com.victronenergy.tank.%s", devid);
}

static void temperatureInit(AnalogSensor *sensor, const char *devid)
{
	SensorDbusInterface *dbus = &sensor->interface.dbus;

	snprintf(dbus->service, sizeof(dbus->service),
			 "com.victronenergy.temperature.%s", devid);
}

/**
 * @brief hook the sensor items to their dbus services
 * @param s - struct with sensor parameters
 * @return Pointer to sensor struct
 */
AnalogSensor *sensorCreate(SensorInfo *s)
{
	AnalogSensor *sensor;
	char devid[64];
	char *p;

	if (sensorCount == MAX_SENSORS)
		return NULL;

	if (s->type == SENSOR_TYPE_TANK)
		sensor = calloc(1, sizeof(struct TankSensor));
	else if (s->type == SENSOR_TYPE_TEMP)
		sensor = calloc(1, sizeof(struct TemperatureSensor));
	else
		return NULL;

	if (!sensor)
		return NULL;

	snprintf(devid, sizeof(devid), "%s_%d", s->dev, s->pin);
	for (p = devid; *p; p++)
		if (!isalnum(*p))
			*p = '_';

	sensors[sensorCount++] = sensor;

	sensor->interface.devfd = s->devfd;
	sensor->interface.adcPin = s->pin;
	sensor->interface.adcScale = s->scale;
	sensor->interface.gpio = s->gpio;
	sensor->sensorType = s->type;
	sensor->instance =
		veDbusGetVrmDeviceInstance(devid, "analog", INSTANCE_BASE);
	sensor->root = veItemAlloc(NULL, "");
	snprintf(sensor->serial, sizeof(sensor->serial), "%s", s->serial);

	if (s->label[0])
		snprintf(sensor->ifaceName, sizeof(sensor->ifaceName), "%s", s->label);
	else
		snprintf(sensor->ifaceName, sizeof(sensor->ifaceName), "Analog input %s:%d", s->dev, s->pin);

	adcFilterReset(&sensor->interface.sigCond.filter);

	if (sensor->sensorType == SENSOR_TYPE_TANK)
		tankInit(sensor, devid);
	else if (sensor->sensorType == SENSOR_TYPE_TEMP)
		temperatureInit(sensor, devid);

	createItems(sensor, devid, s);

	return sensor;
}

static float calcTankInput(struct TankSensor *tank, float adcVal)
{
	if (tank->senseType == TANK_SENSE_RESISTANCE)
		return adcVal / (TANK_SENS_VREF - adcVal) * TANK_SENS_R1;

	if (tank->senseType == TANK_SENSE_VOLTAGE)
		return (TANK_VOLT_R1 + TANK_VOLT_R2) * adcVal / TANK_VOLT_R1;

	if (tank->senseType == TANK_SENSE_CURRENT)
		return 1000 * adcVal / TANK_CURRENT_R;

	return NAN;
}

/**
 * @brief process the tank level sensor adc data
 * @param sensor - pointer to the sensor struct
 * @return Boolean status veTrue - success, veFalse - fail
 */
static void updateTank(AnalogSensor *sensor)
{
	float level, capacity;
	SensorStatus status = SENSOR_STATUS_UNKNOWN;
	VeVariant v;
	struct TankSensor *tank = (struct TankSensor *) sensor;
	float tankEmptyR, tankFullR, tankR, tankRRaw, tankMinR;
	float vMeas = sensor->interface.adcSample;
	float vMeasRaw = sensor->interface.adcSampleRaw;
	int i;

	tankR = calcTankInput(tank, vMeas);
	tankRRaw = calcTankInput(tank, vMeasRaw);

	veItemOwnerSet(sensor->rawValueItem, veVariantFloat(&v, tankRRaw));

	if (!veVariantIsValid(veItemLocalValue(tank->emptyRItem, &v)))
		goto errorState;
	tankEmptyR = v.value.Float;

	if (!veVariantIsValid(veItemLocalValue(tank->fullRItem, &v)))
		goto errorState;
	tankFullR = v.value.Float;

	if (!veVariantIsValid(veItemLocalValue(tank->capacityItem, &v)))
		goto errorState;
	capacity = v.value.Float;

	/* prevent division by zero, configuration issue */
	if (tankFullR == tankEmptyR)
		goto errorState;

	/* If the resistance is higher then the max supported; assume not connected */
	if (tankR > fmax(tankEmptyR, tankFullR) * 1.05) {
		status = SENSOR_STATUS_NOT_CONNECTED;
		goto errorState;
	}

	/* Detect short, but only if not allow by the spec and a bit significant */
	tankMinR = fmin(tankEmptyR, tankFullR);
	if (tankMinR > 20 && tankR < 0.9 * tankMinR) {
		status = SENSOR_STATUS_SHORT;
		goto errorState;
	}

	status = SENSOR_STATUS_OK;
	level = (tankR - tankEmptyR) / (tankFullR - tankEmptyR);
	if (level < 0)
		level = 0;
	if (level > 1)
		level = 1;

	for (i = 1; i < tank->shapeMapLen; i++) {
		if (tank->shapeMap[i][0] >= level) {
			float s0 = tank->shapeMap[i - 1][0];
			float s1 = tank->shapeMap[i    ][0];
			float l0 = tank->shapeMap[i - 1][1];
			float l1 = tank->shapeMap[i    ][1];
			level = l0 + (level - s0) / (s1 - s0) * (l1 - l0);
			break;
		}
	}

	VeVariant oldRemaining;
	float newRemaing = level * capacity;
	float minRemainingChange = capacity / 5000.0f;

	veItemLocalValue(tank->remaingItem, &oldRemaining);
	if (veVariantIsValid(&oldRemaining) &&
		fabsf(oldRemaining.value.Float - newRemaing) < minRemainingChange)
		return;

	veItemOwnerSet(sensor->statusItem, veVariantUn32(&v, status));
	veItemOwnerSet(tank->levelItem, veVariantUn32(&v, 100 * level));
	veItemOwnerSet(tank->remaingItem, veVariantFloat(&v, level * capacity));

	return;

errorState:
	veItemOwnerSet(sensor->statusItem, veVariantUn32(&v, status));
	veItemInvalidate(tank->levelItem);
	veItemInvalidate(tank->remaingItem);
}

/**
 * @brief process the temperature sensor adc data
 * @param sensor - pointer to the sensor struct
 * @return Boolean status veTrue-success, veFalse-fail
 */
static void updateTemperature(AnalogSensor *sensor)
{
	float tempC, offset, scale;
	SensorStatus status = SENSOR_STATUS_UNKNOWN;
	float adcSample = sensor->interface.adcSample;
	float adcSampleRaw = sensor->interface.adcSampleRaw;
	struct TemperatureSensor *temperature = (struct TemperatureSensor *) sensor;
	VeVariant v;

	// calculate the output of the LM335 temperature sensor from the adc pin sample
	float vSense = adcSample * TEMP_SENS_V_RATIO;
	float vSenseRaw = adcSampleRaw * TEMP_SENS_V_RATIO;

	if (!veVariantIsValid(veItemLocalValue(temperature->offsetItem, &v)))
		goto updateState;
	offset = v.value.Float;

	if (!veVariantIsValid(veItemLocalValue(temperature->scaleItem, &v)))
		goto updateState;
	scale = v.value.Float;

	if (adcSample > TEMP_SENS_MIN_ADCIN && adcSample < TEMP_SENS_MAX_ADCIN) {
		// convert from Kelvin to Celsius
		tempC = 100 * vSense - 273;
		// Signal scale correction
		tempC *= scale;
		// Signal offset correction
		tempC += offset;

		status = SENSOR_STATUS_OK;
	} else if (adcSample > TEMP_SENS_MAX_ADCIN) {
		// open circuit error
		status = SENSOR_STATUS_NOT_CONNECTED;
	} else if (adcSample < TEMP_SENS_S_C_ADCIN ) {
		// short circuit error
		status = SENSOR_STATUS_SHORT;
	} else if (adcSample > TEMP_SENS_INV_PLRTY_ADCIN_LB &&
			   adcSample < TEMP_SENS_INV_PLRTY_ADCIN_HB) {
		// lm335 probably connected in reverse polarity
		status = SENSOR_STATUS_REVERSE_POLARITY;
	} else {
		// low temperature or unknown error
		status = SENSOR_STATUS_UNKNOWN;
	}

updateState:
	veItemOwnerSet(sensor->statusItem, veVariantUn32(&v, status));
	if (status == SENSOR_STATUS_OK)
		veItemOwnerSet(temperature->temperatureItem, veVariantSn32(&v, tempC));
	else
		veItemInvalidate(temperature->temperatureItem);
	veItemOwnerSet(sensor->rawValueItem, veVariantFloat(&v, vSenseRaw));
}

static void sensorDbusConnect(AnalogSensor *sensor)
{
	sensor->dbus = veDbusConnectString(veDbusGetDefaultConnectString());
	if (!sensor->dbus) {
		logE(sensor->interface.dbus.service, "dbus connect failed");
		pltExit(1);
	}

	veDbusItemInit(sensor->dbus, sensor->root);
	veDbusChangeName(sensor->dbus, sensor->interface.dbus.service);

	logI(sensor->interface.dbus.service, "connected to dbus");
}

void sensorTick(void)
{
	int i;
	VeVariant v;

	/* Read the ADC values */
	for (i = 0; i < sensorCount; i++) {
		AnalogSensor *sensor = sensors[i];
		un32 val;

		sensor->valid = adcRead(&val, sensor);
		if (sensor->valid)
			sensor->interface.adcSampleRaw = val * sensor->interface.adcScale;
	}

	/* Handle ADC values */
	for (i = 0; i < sensorCount; i++) {
		AnalogSensor *sensor = sensors[i];
		Filter *filter = &sensor->interface.sigCond.filter;

		if (!sensor->valid)
			continue;

		/* filter the input ADC sample */
		sensor->interface.adcSample =
			adcFilter(sensor->interface.adcSampleRaw, filter);

		if (!veVariantIsValid(veItemLocalValue(sensor->function, &v)))
			continue;

		switch (v.value.SN32) {
		case SENSOR_FUNCTION_DEFAULT:
			if (!sensor->interface.dbus.connected) {
				sensorDbusConnect(sensor);
				sensor->interface.dbus.connected = veTrue;
			}

			switch (sensor->sensorType) {
			case SENSOR_TYPE_TANK:
				updateTank(sensor);
				break;

			case SENSOR_TYPE_TEMP:
				updateTemperature(sensor);
				break;
			}
			break;

		case SENSOR_FUNCTION_NONE:
		default:
			if (sensor->interface.dbus.connected) {
				veDbusDisconnect(sensor->dbus);
				sensor->interface.dbus.connected = veFalse;
			}
			break;
		}
	}
}
