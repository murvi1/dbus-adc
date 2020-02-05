#ifndef _VALUES_H_
#define _VALUES_H_

#include <velib/types/ve_item.h>
#include <velib/types/variant_print.h>

/**
 * To seperate gui logic / any other logic from the communication
 * the received values are stored in a tree in SI units. This struct
 * contains the field needed to create the tree.
 */
// tick interval definition for app ticking
#define DESIRED_VALUES_TASK_INTERVAL	100 // ms
#define VALUES_TASK_INTERVAL			DESIRED_VALUES_TASK_INTERVAL / 50 // 50ms base ticking

typedef struct {
	VeVariantUnitFmt unit;
	VeItemValueFmt *fun;
} FormatInfo;

// information for interfacing to dbus service
typedef struct {
	struct VeItem *item;
	VeVariant *local;
	char const *id;
	FormatInfo *fmt;
	un8 timeout;
	VeItemSetterFun *setValueCallback;
} ItemInfo;

// values variables structure for dbus settings parameters
typedef struct {
	VeVariant def;
	VeVariant min;
	VeVariant max;
} values_range_t;

// structure to hold the information required to dbus interfacing
typedef struct {
	float def;
	float min;
	float max;
	char path[64];
	struct VeDbus *connect;
	struct VeItem *value;
} dbus_info_t;

/***********************************************/
// Public function prototypes
void valuesTick(void);
struct VeItem *getConsumerRoot(void);

#endif
