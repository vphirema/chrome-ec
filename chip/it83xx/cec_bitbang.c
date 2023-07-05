/* Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "cec.h"
#include "cec_bitbang_chip.h"
#include "console.h"
#include "driver/cec/bitbang.h"
#include "gpio.h"
#include "hwtimer_chip.h"
#include "task.h"
#include "timer.h"
#include "util.h"

#ifdef CONFIG_CEC_DEBUG
#define CPRINTF(format, args...) cprintf(CC_CEC, format, ##args)
#define CPRINTS(format, args...) cprints(CC_CEC, format, ##args)
#else
#define CPRINTF(...)
#define CPRINTS(...)
#endif

/* Only one instance of the bitbang driver is supported on ITE for now. */
static int cec_port;

/* Timestamp when the most recent interrupt occurred */
static timestamp_t interrupt_time;

/* Timestamp when the second most recent interrupt occurred */
static timestamp_t prev_interrupt_time;

/* Flag set when a transfer is initiated from the AP */
static bool transfer_initiated;

/*
 * ITE doesn't have a capture timer, so we use a countdown timer for timeout
 * events combined with a GPIO interrupt for capture events.
 */
void cec_tmr_cap_start(int port, enum cec_cap_edge edge, int timeout)
{
	const struct bitbang_cec_config *drv_config =
		cec_config[port].drv_config;

	switch (edge) {
	case CEC_CAP_EDGE_NONE:
		gpio_disable_interrupt(drv_config->gpio_in);
		break;
	case CEC_CAP_EDGE_FALLING:
		gpio_set_flags(drv_config->gpio_in, GPIO_INT_FALLING);
		gpio_enable_interrupt(drv_config->gpio_in);
		break;
	case CEC_CAP_EDGE_RISING:
		gpio_set_flags(drv_config->gpio_in, GPIO_INT_RISING);
		gpio_enable_interrupt(drv_config->gpio_in);
		break;
	}

	if (timeout > 0) {
		/*
		 * Take into account the delay from when the interrupt occurs to
		 * when we actually get here.
		 */
		int delay =
			CEC_US_TO_TICKS(get_time().val - interrupt_time.val);
		int timer_count = timeout - delay;

		/*
		 * Handle the case where the delay is greater than the timeout.
		 * This should never actually happen for typical delay and
		 * timeout values.
		 */
		if (timer_count < 0) {
			timer_count = 0;
			CPRINTS("CEC WARNING: timer_count < 0");
		}

		/* Start the timer and enable the timer interrupt */
		ext_timer_ms(drv_config->timer, CEC_CLOCK_SOURCE, 1, 1,
			     timer_count, 0, 1);
	} else {
		ext_timer_stop(drv_config->timer, 1);
	}
}

void cec_tmr_cap_stop(int port)
{
	const struct bitbang_cec_config *drv_config =
		cec_config[port].drv_config;

	gpio_disable_interrupt(drv_config->gpio_in);
	ext_timer_stop(drv_config->timer, 1);
}

int cec_tmr_cap_get(int port)
{
	return CEC_US_TO_TICKS(interrupt_time.val - prev_interrupt_time.val);
}

__override void cec_update_interrupt_time(int port)
{
	prev_interrupt_time = interrupt_time;
	interrupt_time = get_time();
}

void cec_ext_timer_interrupt(void)
{
	int port = cec_port;

	if (transfer_initiated) {
		transfer_initiated = false;
		cec_event_tx(port);
	} else {
		cec_update_interrupt_time(port);
		cec_event_timeout(port);
	}
}

void cec_gpio_interrupt(enum gpio_signal signal)
{
	int port = cec_port;

	cec_update_interrupt_time(port);
	cec_event_cap(port);
}

void cec_trigger_send(int port)
{
	const struct bitbang_cec_config *drv_config =
		cec_config[port].drv_config;

	/* Elevate to interrupt context */
	transfer_initiated = true;
	task_trigger_irq(et_ctrl_regs[drv_config->timer].irq);
}

void cec_enable_timer(int port)
{
	/*
	 * Nothing to do. Interrupts will be enabled as needed by
	 * cec_tmr_cap_start().
	 */
}

void cec_disable_timer(int port)
{
	cec_tmr_cap_stop(port);

	interrupt_time.val = 0;
	prev_interrupt_time.val = 0;
}

void cec_init_timer(int port)
{
	const struct bitbang_cec_config *drv_config =
		cec_config[port].drv_config;

	cec_port = port;

	ext_timer_ms(drv_config->timer, CEC_CLOCK_SOURCE, 0, 0, 0, 1, 0);
}
