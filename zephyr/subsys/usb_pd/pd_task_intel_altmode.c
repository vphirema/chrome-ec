/* Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * Source file for PD task to configure USB-C Alternate modes on Intel SoC.
 */

#include "i2c.h"
#include "i2c/i2c.h"
#include "usb_mux.h"
#include "usbc/utils.h"

#include <stdlib.h>

#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <ap_power/ap_power.h>
#include <drivers/intel_altmode.h>
#include <usbc/pd_task_intel_altmode.h>

LOG_MODULE_DECLARE(usbpd_altmode, CONFIG_USB_PD_ALTMODE_LOG_LEVEL);

#define INTEL_ALTMODE_COMPAT_PD intel_pd_altmode

#define PD_CHIP_ENTRY(usbc_id, pd_id, config_fn) \
	[USBC_PORT_NEW(usbc_id)] = config_fn(pd_id),

#define CHECK_COMPAT(compat, usbc_id, pd_id, config_fn) \
	COND_CODE_1(DT_NODE_HAS_COMPAT(pd_id, compat),  \
		    (PD_CHIP_ENTRY(usbc_id, pd_id, config_fn)), ())

#define PD_CHIP_FIND(usbc_id, pd_id) \
	CHECK_COMPAT(INTEL_ALTMODE_COMPAT_PD, usbc_id, pd_id, DEVICE_DT_GET)

#define PD_CHIP(usbc_id)                                                      \
	COND_CODE_1(DT_NODE_HAS_PROP(usbc_id, pd_altmode),                    \
		    (PD_CHIP_FIND(usbc_id, DT_PHANDLE(usbc_id, pd_altmode))), \
		    ())

#define INTEL_ALTMODE_EVENT_MASK GENMASK(INTEL_ALTMODE_EVENT_COUNT - 1, 0)

enum intel_altmode_event {
	INTEL_ALTMODE_EVENT_FORCE,
	INTEL_ALTMODE_EVENT_INTERRUPT,
	INTEL_ALTMODE_EVENT_COUNT
};

struct intel_altmode_data {
	/* Driver event object to receive events posted. */
	struct k_event evt;
	/* Callback for the AP power events */
	struct ap_power_ev_callback cb;
	/* Cache the dta status register */
	union data_status_reg data_status[CONFIG_USB_PD_PORT_MAX_COUNT];
};

/* Generate device tree for available PDs */
static const struct device *pd_config_array[] = { DT_FOREACH_STATUS_OKAY(
	named_usbc_port, PD_CHIP) };

BUILD_ASSERT(ARRAY_SIZE(pd_config_array) == CONFIG_USB_PD_PORT_MAX_COUNT);

/* Store current data of the DATA STATUS register */

static struct intel_altmode_data intel_altmode_task_data;

static const struct device *intel_altmode_get_instance(void);

static void intel_altmode_post_event(enum intel_altmode_event event)
{
	const struct device *dev = intel_altmode_get_instance();
	struct intel_altmode_data *const data = dev->data;

	k_event_post(&data->evt, BIT(event));
}

static void intel_altmode_suspend_handler(struct ap_power_ev_callback *cb,
					  struct ap_power_ev_data data)
{
	LOG_DBG("suspend event: 0x%x", data.event);

	if (data.event == AP_POWER_RESUME) {
		/*
		 * Set event to forcefully get new PD data.
		 * This ensures EC doesn't miss the interrupt if the interrupt
		 * pull-ups are on A-rail.
		 */
		intel_altmode_post_event(INTEL_ALTMODE_EVENT_FORCE);
	} else {
		LOG_ERR("Invalid suspend event");
	}
}

static void intel_altmode_event_cb(void)
{
	intel_altmode_post_event(INTEL_ALTMODE_EVENT_INTERRUPT);
}

static uint32_t intel_altmode_wait_event(const struct device *dev)
{
	struct intel_altmode_data *const data = dev->data;
	uint32_t events;

	events = k_event_wait(&data->evt, INTEL_ALTMODE_EVENT_MASK, false,
			      Z_FOREVER);
	/* Clear all events posted */
	k_event_clear(&data->evt, events);

	return events & INTEL_ALTMODE_EVENT_MASK;
}

static void process_altmode_pd_data(int port)
{
	int rv;
	union data_status_reg status;
	mux_state_t mux = USB_PD_MUX_NONE;
	union data_status_reg *prev_status =
		&intel_altmode_task_data.data_status[port];
	union data_control_reg control = { .i2c_int_ack = 1 };

	LOG_INF("Process p%d data", port);

	/* Clear the interrupt */
	rv = pd_altmode_write(pd_config_array[port], &control);
	if (rv) {
		LOG_ERR("P%d write Err=%d", port, rv);
		return;
	}

	/* Read the status register */
	rv = pd_altmode_read(pd_config_array[port], &status);
	if (rv) {
		LOG_ERR("P%d read Err=%d", port, rv);
		return;
	}

	/* Nothing to do if the data in the status register has not changed */
	if (!memcmp(&status.raw_value[0], prev_status,
		    sizeof(union data_status_reg)))
		return;

	/* Update the new data */
	memcpy(prev_status, &status, sizeof(union data_status_reg));

	/* Process MUX events */

	/* Orientation */
	if (status.conn_ori)
		mux |= USB_PD_MUX_POLARITY_INVERTED;

	/* USB status */
	if (status.usb2 || status.usb3_2)
		mux |= USB_PD_MUX_USB_ENABLED;

	LOG_INF("Set p%d mux=0x%x", port, mux);

	usb_mux_set(port, mux,
		    mux == USB_PD_MUX_NONE ? USB_SWITCH_DISCONNECT :
					     USB_SWITCH_CONNECT,
		    polarity_rm_dts(status.conn_ori));
}

static void intel_altmode_thread(void *arg, void *unused1, void *unused2)
{
	int i;
	uint32_t events;
	struct device *const dev = (struct device *)arg;

	/* Add callbacks for suspend hooks */
	ap_power_ev_init_callback(&intel_altmode_task_data.cb,
				  intel_altmode_suspend_handler,
				  AP_POWER_RESUME);
	ap_power_ev_add_callback(&intel_altmode_task_data.cb);

	/* Register PD interrupt callback */
	for (i = 0; i < CONFIG_USB_PD_PORT_MAX_COUNT; i++)
		pd_altmode_set_result_cb(pd_config_array[i],
					 intel_altmode_event_cb);

	LOG_INF("Intel Altmode thread start");

	while (1) {
		events = intel_altmode_wait_event(dev);

		LOG_DBG("Altmode events=0x%x", events);

		if (events & BIT(INTEL_ALTMODE_EVENT_INTERRUPT)) {
			/* Process data of interrupted port */
			for (i = 0; i < CONFIG_USB_PD_PORT_MAX_COUNT; i++) {
				if (pd_altmode_is_interrupted(
					    pd_config_array[i]))
					process_altmode_pd_data(i);
			}
		} else if (events & BIT(INTEL_ALTMODE_EVENT_FORCE)) {
			/* Process data for any wake events on all ports */
			for (i = 0; i < CONFIG_USB_PD_PORT_MAX_COUNT; i++)
				process_altmode_pd_data(i);
		}
	}
}

static int intel_altmode_driver_init(const struct device *dev)
{
	struct intel_altmode_data *const data = dev->data;

	k_event_init(&data->evt);

	return 0;
}

DEVICE_DEFINE(intel_altmode_dev, "intel_altmode_drv", intel_altmode_driver_init,
	      NULL, &intel_altmode_task_data, NULL, APPLICATION,
	      CONFIG_APPLICATION_INIT_PRIORITY, NULL);

K_THREAD_DEFINE(intel_altmode_tid, CONFIG_TASK_PD_ALTMODE_INTEL_STACK_SIZE,
		intel_altmode_thread, DEVICE_GET(intel_altmode_dev), NULL, NULL,
		CONFIG_USBPD_ALTMODE_INTEL_THREAD_PRIORITY, 0, K_TICKS_FOREVER);

static const struct device *intel_altmode_get_instance(void)
{
	return DEVICE_GET(intel_altmode_dev);
}

void intel_altmode_task_start(void)
{
	k_thread_start(intel_altmode_tid);
}

#ifdef CONFIG_CONSOLE_CMD_USBPD_INTEL_ALTMODE
static int console_command_intel_altmode(const struct shell *shell, size_t argc,
					 char **argv)
{
	int port, rv = EC_ERROR_UNKNOWN, i;
	char rw, *e;
	uint16_t val1;
	uint32_t val2 = 0;
	union data_status_reg status;
	union data_control_reg control;

	if (argc < 3 || argc > 5) {
		rv = EC_ERROR_PARAM_COUNT;
		goto error;
	}

	/* Get PD port number */
	port = strtol(argv[1], &e, 0);
	if (*e || port > CONFIG_USB_PD_PORT_MAX_COUNT) {
		rv = EC_ERROR_PARAM1;
		goto error;
	}

	/* Validate r/w selection */
	rw = argv[2][0];
	if (rw != 'w' && rw != 'r') {
		rv = EC_ERROR_PARAM2;
		goto error;
	}

	if (rw == 'r') {
		if (argc > 3) {
			rv = EC_ERROR_PARAM_COUNT;
			goto error;
		}

		rv = pd_altmode_read(pd_config_array[port], &status);
		if (rv)
			goto error;

		shell_fprintf(shell, SHELL_INFO, "RD_VAL: ");
		for (i = 0; i < INTEL_ALTMODE_DATA_STATUS_REG_LEN; i++)
			shell_fprintf(shell, SHELL_INFO, "[%d]0x%x, ", i,
				      status.raw_value[i]);
		shell_fprintf(shell, SHELL_INFO, "\n");
	} else {
		if (argc < 4) {
			rv = EC_ERROR_PARAM_COUNT;
			goto error;
		}

		/* Control register data */
		val1 = strtoull(argv[3], &e, 0);
		if (*e) {
			rv = EC_ERROR_PARAM3;
			goto error;
		}

		/* Control register retimer data */
		if (argc > 4) {
			val2 = strtoull(argv[4], &e, 0);
			if (*e) {
				rv = EC_ERROR_PARAM4;
				goto error;
			}
		}

		memcpy(&control.raw_value[0], &val1, 2);
		memcpy(&control.raw_value[2], &val2, 4);

		rv = pd_altmode_write(pd_config_array[port], &control);
		if (rv)
			goto error;

		shell_fprintf(shell, SHELL_INFO, "WR_VAL: ");
		for (i = 0; i < INTEL_ALTMODE_DATA_CONTROL_REG_LEN; i++)
			shell_fprintf(shell, SHELL_INFO, "[%d]0x%x, ", i,
				      control.raw_value[i]);
		shell_fprintf(shell, SHELL_INFO, "\n");
	}

error:
	if (rv)
		shell_fprintf(shell, SHELL_INFO, "altmode rv=%d\n", rv);

	return rv;
}

SHELL_CMD_REGISTER(altmode, NULL, "Read or write to Altmode PD reg",
		   console_command_intel_altmode);
#endif /* CONFIG_CONSOLE_CMD_USBPD_INTEL_ALTMODE */

/*
 * TODO: For all the below functions; need to enable PD to EC power path
 * interface and gather the information.
 */
enum tcpc_cc_polarity pd_get_polarity(int port)
{
	return intel_altmode_task_data.data_status[port].conn_ori;
}

enum pd_data_role pd_get_data_role(int port)
{
	return !intel_altmode_task_data.data_status[port].data_role;
}

int pd_is_connected(int port)
{
	return intel_altmode_task_data.data_status[port].data_conn;
}

/*
 * To suppress the compilation error, below functions are added with tested
 * data.
 */
void pd_request_data_swap(int port)
{
}

enum pd_power_role pd_get_power_role(int port)
{
	return !intel_altmode_task_data.data_status[port].dp_src_snk;
}

uint8_t pd_get_task_state(int port)
{
	return 0;
}

int pd_comm_is_enabled(int port)
{
	return 1;
}

bool pd_get_vconn_state(int port)
{
	return true;
}

bool pd_get_partner_dual_role_power(int port)
{
	return false;
}

bool pd_get_partner_data_swap_capable(int port)
{
	return false;
}

bool pd_get_partner_usb_comm_capable(int port)
{
	return false;
}

bool pd_get_partner_unconstr_power(int port)
{
	return false;
}

const char *pd_get_task_state_name(int port)
{
	return "";
}

enum pd_cc_states pd_get_task_cc_state(int port)
{
	return PD_CC_UFP_ATTACHED;
}

bool pd_capable(int port)
{
	return true;
}
