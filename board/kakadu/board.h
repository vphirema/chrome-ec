/* Copyright 2020 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Configuration for kakadu */

#ifndef __CROS_EC_BOARD_H
#define __CROS_EC_BOARD_H

#define VARIANT_KUKUI_BATTERY_MAX17055
#define VARIANT_KUKUI_CHARGER_MT6370
#define VARIANT_KUKUI_POGO_KEYBOARD
#define VARIANT_KUKUI_TABLET_PWRBTN

#ifndef SECTION_IS_RW
#define VARIANT_KUKUI_NO_SENSORS
#endif /* SECTION_IS_RW */

#include "baseboard.h"

#define CONFIG_USB_MUX_IT5205
#define CONFIG_VOLUME_BUTTONS

/* Battery */
#define BATTERY_DESIRED_CHARGING_CURRENT    3500  /* mA */

#define CONFIG_CHARGER_MT6370_BACKLIGHT
#ifdef BOARD_KAKADU
#define CHARGER_LIMIT_TIMEOUT_HOURS 48
#define CHARGER_LIMIT_TIMEOUT_HOURS_TEMP 2
#endif

/* Motion Sensors */
#ifdef SECTION_IS_RW
#define CONFIG_ACCELGYRO_LSM6DSM
#define CONFIG_ACCEL_INTERRUPTS
#define CONFIG_ACCEL_LSM6DSM_INT_EVENT \
	TASK_EVENT_MOTION_SENSOR_INTERRUPT(LID_ACCEL)

/* Camera VSYNC */
#define CONFIG_SYNC
#define CONFIG_SYNC_COMMAND
#define CONFIG_SYNC_INT_EVENT \
	TASK_EVENT_MOTION_SENSOR_INTERRUPT(VSYNC)
#endif /* SECTION_IS_RW */

/* I2C ports */
#define I2C_PORT_CHARGER  0
#define I2C_PORT_TCPC0    0
#define I2C_PORT_USB_MUX  0
#define I2C_PORT_BATTERY  1
#define I2C_PORT_VIRTUAL_BATTERY I2C_PORT_BATTERY
#define I2C_PORT_ACCEL    1
#define I2C_PORT_BC12     1
#define I2C_PORT_ALS      1

/* Route sbs host requests to virtual battery driver */
#define VIRTUAL_BATTERY_ADDR_FLAGS 0x0B

/* Define the host events which are allowed to wakeup AP in S3. */
#define CONFIG_MKBP_HOST_EVENT_WAKEUP_MASK \
		(EC_HOST_EVENT_MASK(EC_HOST_EVENT_LID_OPEN) |\
		 EC_HOST_EVENT_MASK(EC_HOST_EVENT_POWER_BUTTON))

/* MKBP */
#define CONFIG_MKBP_EVENT
#define CONFIG_MKBP_EVENT_WAKEUP_MASK \
	(BIT(EC_MKBP_EVENT_SENSOR_FIFO) | BIT(EC_MKBP_EVENT_HOST_EVENT))

#define PD_OPERATING_POWER_MW 15000

#ifndef __ASSEMBLER__

enum adc_channel {
	/* Real ADC channels begin here */
	ADC_BOARD_ID = 0,
	ADC_EC_SKU_ID,
	ADC_BATT_ID,
	ADC_POGO_ADC_INT_L,
	ADC_CH_COUNT
};

/* power signal definitions */
enum power_signal {
	AP_IN_S3_L,
	PMIC_PWR_GOOD,

	/* Number of signals */
	POWER_SIGNAL_COUNT,
};

/* Motion sensors */
enum sensor_id {
	LID_ACCEL = 0,
	LID_GYRO,
	VSYNC,
	SENSOR_COUNT,
};

enum charge_port {
	CHARGE_PORT_USB_C,
};

#include "gpio_signal.h"
#include "registers.h"

#ifdef SECTION_IS_RO
/* Interrupt handler for emmc task */
void emmc_cmd_interrupt(enum gpio_signal signal);
#endif

void board_reset_pd_mcu(void);
int board_get_version(void);
int board_is_sourcing_vbus(int port);
void pogo_adc_interrupt(enum gpio_signal signal);
int board_discharge_on_ac(int enable);


#endif /* !__ASSEMBLER__ */

#endif /* __CROS_EC_BOARD_H */
