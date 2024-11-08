/* Copyright 2024 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "battery.h"
#include "common.h"
#include "cros_board_info.h"
#include "cros_cbi.h"
#include "ec_commands.h"
#include "fan.h"
#include "gpio/gpio_int.h"
#include "hooks.h"
#include "keyboard_backlight.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(board_init, LOG_LEVEL_INF);
#define CPRINTS(format, args...) cprints(CC_SYSTEM, format, ##args)

enum battery_present battery_hw_present(void)
{
	const struct gpio_dt_spec *batt_pres;

	batt_pres = GPIO_DT_FROM_NODELABEL(gpio_ec_battery_pres_odl);

	/* The GPIO is low when the battery is physically present */
	return gpio_pin_get_dt(batt_pres) ? BP_NO : BP_YES;
}

test_export_static void fan_init(void)
{
	int ret;
	uint32_t val;
	/*
	 * Retrieve the fan config.
	 */
	ret = cros_cbi_get_fw_config(FW_FAN, &val);
	if (ret != 0) {
		LOG_ERR("Error retrieving CBI FW_CONFIG field %d", FW_FAN);
		return;
	}
	if (val != FW_FAN_PRESENT) {
		/* Disable the fan */
		fan_set_count(0);
	}
}
DECLARE_HOOK(HOOK_INIT, fan_init, HOOK_PRIO_POST_FIRST);

static bool key_bl = FW_KB_BL_NOT_PRESENT;

int8_t board_vivaldi_keybd_idx(void)
{
	CPRINTS("idx:This is %s kb backlight.", key_bl ? "with" : "without");
	if (key_bl == FW_KB_BL_NOT_PRESENT) {
		return DT_NODE_CHILD_IDX(DT_NODELABEL(kbd_config_0));
	} else {
		return DT_NODE_CHILD_IDX(DT_NODELABEL(kbd_config_1));
	}
}

/*
 * Keyboard function decided by FW config.
 */
test_export_static void kb_init(void)
{
	int ret;
	uint32_t val;

	ret = cros_cbi_get_fw_config(FW_KB_BL, &val);
	if (ret != 0) {
		LOG_ERR("Error retrieving CBI FW_CONFIG field %d", FW_KB_BL);
		return;
	}

	CPRINTS("This is %s kb backlight.", key_bl ? "with" : "without");
	if (val == FW_KB_BL_PRESENT) {
		key_bl = FW_KB_BL_PRESENT;
	} else {
		key_bl = FW_KB_BL_NOT_PRESENT;
		kblight_enable(0);
	}
}
DECLARE_HOOK(HOOK_INIT, kb_init, HOOK_PRIO_POST_FIRST);
