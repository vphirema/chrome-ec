/* Copyright 2021 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "cbi.h"
#include "charge_ramp.h"
#include "charger.h"
#include "common.h"
#include "compile_time_macros.h"
#include "console.h"
#include "driver/bc12/pi3usb9201_public.h"
#include "driver/ppc/nx20p348x.h"
#include "driver/ppc/syv682x_public.h"
#include "driver/retimer/bb_retimer_public.h"
#include "driver/tcpm/nct38xx.h"
#include "driver/tcpm/ps8xxx_public.h"
#include "driver/tcpm/tcpci.h"
#include "ec_commands.h"
#include "fw_config.h"
#include "gpio.h"
#include "gpio_signal.h"
#include "hooks.h"
#include "ioexpander.h"
#include "system.h"
#include "task.h"
#include "task_id.h"
#include "timer.h"
#include "usb_charge.h"
#include "usb_mux.h"
#include "usb_pd.h"
#include "usb_pd_tcpm.h"
#include "usbc_config.h"
#include "usbc_ppc.h"

#include <stdbool.h>
#include <stdint.h>

#define CPRINTF(format, args...) cprintf(CC_USBPD, format, ##args)
#define CPRINTS(format, args...) cprints(CC_USBPD, format, ##args)

#ifdef CONFIG_ZEPHYR
enum ioex_port { IOEX_C0_NCT38XX = 0, IOEX_C2_NCT38XX, IOEX_PORT_COUNT };
#endif /* CONFIG_ZEPHYR */

#ifndef CONFIG_ZEPHYR
/* USBC TCPC configuration */
const struct tcpc_config_t tcpc_config[] = {
	[USBC_PORT_C0] = {
		.bus_type = EC_BUS_TYPE_I2C,
		.i2c_info = {
			.port = I2C_PORT_USB_C0_C2_TCPC,
			.addr_flags = NCT38XX_I2C_ADDR1_1_FLAGS,
		},
		.drv = &nct38xx_tcpm_drv,
		.flags = TCPC_FLAGS_TCPCI_REV2_0 |
			TCPC_FLAGS_NO_DEBUG_ACC_CONTROL,
	},
	[USBC_PORT_C1] = {
		.bus_type = EC_BUS_TYPE_I2C,
		.i2c_info = {
			.port = I2C_PORT_USB_C1_TCPC,
			.addr_flags = PS8XXX_I2C_ADDR1_FLAGS,
		},
		.drv = &ps8xxx_tcpm_drv,
		.flags = TCPC_FLAGS_TCPCI_REV2_0 |
			 TCPC_FLAGS_TCPCI_REV2_0_NO_VSAFE0V |
			 TCPC_FLAGS_CONTROL_VCONN |
			 TCPC_FLAGS_CONTROL_FRS,
	},
	[USBC_PORT_C2] = {
		.bus_type = EC_BUS_TYPE_I2C,
		.i2c_info = {
			.port = I2C_PORT_USB_C0_C2_TCPC,
			.addr_flags = NCT38XX_I2C_ADDR2_1_FLAGS,
		},
		.drv = &nct38xx_tcpm_drv,
		.flags = TCPC_FLAGS_TCPCI_REV2_0,
	},
};
BUILD_ASSERT(ARRAY_SIZE(tcpc_config) == USBC_PORT_COUNT);
BUILD_ASSERT(CONFIG_USB_PD_PORT_MAX_COUNT == USBC_PORT_COUNT);
#endif /* !CONFIG_ZEPHYR */

/******************************************************************************/
/* USB-A charging control */

#ifndef CONFIG_ZEPHYR
const int usb_port_enable[USB_PORT_COUNT] = {
	GPIO_EN_PP5000_USBA_R,
};
#endif
BUILD_ASSERT(ARRAY_SIZE(usb_port_enable) == USB_PORT_COUNT);

/******************************************************************************/

#ifndef CONFIG_ZEPHYR
/* USBC PPC configuration */
struct ppc_config_t ppc_chips[] = {
	[USBC_PORT_C0] = {
		.i2c_port = I2C_PORT_USB_C0_C2_PPC,
		.i2c_addr_flags = SYV682X_ADDR0_FLAGS,
		.frs_en = IOEX_USB_C0_FRS_EN,
		.drv = &syv682x_drv,
	},
	[USBC_PORT_C1] = {
		/* Compatible with Silicon Mitus SM5360A */
		.i2c_port = I2C_PORT_USB_C1_PPC,
		.i2c_addr_flags = NX20P3483_ADDR2_FLAGS,
		.drv = &nx20p348x_drv,
	},
	[USBC_PORT_C2] = {
		.i2c_port = I2C_PORT_USB_C0_C2_PPC,
		.i2c_addr_flags = SYV682X_ADDR2_FLAGS,
		.frs_en = IOEX_USB_C2_FRS_EN,
		.drv = &syv682x_drv,
	},
};
BUILD_ASSERT(ARRAY_SIZE(ppc_chips) == USBC_PORT_COUNT);

unsigned int ppc_cnt = ARRAY_SIZE(ppc_chips);

/* USBC mux configuration - Alder Lake includes internal mux */
static const struct usb_mux_chain usbc0_tcss_usb_mux = {
	.mux =
		&(const struct usb_mux){
			.usb_port = USBC_PORT_C0,
			.driver = &virtual_usb_mux_driver,
			.hpd_update = &virtual_hpd_update,
		},
};
static const struct usb_mux_chain usbc2_tcss_usb_mux = {
	.mux =
		&(const struct usb_mux){
			.usb_port = USBC_PORT_C2,
			.driver = &virtual_usb_mux_driver,
			.hpd_update = &virtual_hpd_update,
		},
};

/*
 * USB3 DB mux configuration - the top level mux still needs to be set
 * to the virtual_usb_mux_driver so the AP gets notified of mux changes
 * and updates the TCSS configuration on state changes.
 */
static const struct usb_mux_chain usbc1_usb3_db_retimer = {
	.mux =
		&(const struct usb_mux){
			.usb_port = USBC_PORT_C1,
			.driver = &tcpci_tcpm_usb_mux_driver,
			.hpd_update = &ps8xxx_tcpc_update_hpd_status,
		},
};

const struct usb_mux_chain usb_muxes[] = {
	[USBC_PORT_C0] = {
		.mux = &(const struct usb_mux) {
			.usb_port = USBC_PORT_C0,
			.flags = USB_MUX_FLAG_CAN_IDLE,
			.driver = &bb_usb_retimer,
			.hpd_update = bb_retimer_hpd_update,
			.i2c_port = I2C_PORT_USB_C0_C2_MUX,
			.i2c_addr_flags = USBC_PORT_C0_BB_RETIMER_I2C_ADDR,
		},
		.next = &usbc0_tcss_usb_mux,
	},
	[USBC_PORT_C1] = {
		.mux = &(const struct usb_mux) {
			/* PS8815 DB */
			.usb_port = USBC_PORT_C1,
			.driver = &virtual_usb_mux_driver,
			.hpd_update = &virtual_hpd_update,
		},
		.next = &usbc1_usb3_db_retimer,
	},
	[USBC_PORT_C2] = {
		.mux = &(const struct usb_mux) {
			.usb_port = USBC_PORT_C2,
			.flags = USB_MUX_FLAG_CAN_IDLE,
			.driver = &bb_usb_retimer,
			.hpd_update = bb_retimer_hpd_update,
			.i2c_port = I2C_PORT_USB_C0_C2_MUX,
			.i2c_addr_flags = USBC_PORT_C2_BB_RETIMER_I2C_ADDR,
		},
		.next = &usbc2_tcss_usb_mux,
	},
};
BUILD_ASSERT(ARRAY_SIZE(usb_muxes) == USBC_PORT_COUNT);

/* BC1.2 charger detect configuration */
const struct pi3usb9201_config_t pi3usb9201_bc12_chips[] = {
	[USBC_PORT_C0] = {
		.i2c_port = I2C_PORT_USB_C0_C2_BC12,
		.i2c_addr_flags = PI3USB9201_I2C_ADDR_3_FLAGS,
	},
	[USBC_PORT_C1] = {
		.i2c_port = I2C_PORT_USB_C1_BC12,
		.i2c_addr_flags = PI3USB9201_I2C_ADDR_3_FLAGS,
	},
	[USBC_PORT_C2] = {
		.i2c_port = I2C_PORT_USB_C0_C2_BC12,
		.i2c_addr_flags = PI3USB9201_I2C_ADDR_1_FLAGS,
	},
};
BUILD_ASSERT(ARRAY_SIZE(pi3usb9201_bc12_chips) == USBC_PORT_COUNT);

/*
 * USB C0 and C2 uses burnside bridge chips and have their reset
 * controlled by their respective TCPC chips acting as GPIO expanders.
 *
 * ioex_init() is normally called before we take the TCPCs out of
 * reset, so we need to start in disabled mode, then explicitly
 * call ioex_init().
 */

struct ioexpander_config_t ioex_config[] = {
	[IOEX_C0_NCT38XX] = {
		.i2c_host_port = I2C_PORT_USB_C0_C2_TCPC,
		.i2c_addr_flags = NCT38XX_I2C_ADDR1_1_FLAGS,
		.drv = &nct38xx_ioexpander_drv,
		.flags = IOEX_FLAGS_DEFAULT_INIT_DISABLED,
	},
	[IOEX_C2_NCT38XX] = {
		.i2c_host_port = I2C_PORT_USB_C0_C2_TCPC,
		.i2c_addr_flags = NCT38XX_I2C_ADDR2_1_FLAGS,
		.drv = &nct38xx_ioexpander_drv,
		.flags = IOEX_FLAGS_DEFAULT_INIT_DISABLED,
	},
};
BUILD_ASSERT(ARRAY_SIZE(ioex_config) == CONFIG_IO_EXPANDER_PORT_COUNT);
#endif /* !CONFIG_ZEPHYR */

#ifdef CONFIG_CHARGE_RAMP_SW

/*
 * TODO(b/181508008): tune this threshold
 */

#define BC12_MIN_VOLTAGE 4400

/**
 * Return true if VBUS is too low
 */
int board_is_vbus_too_low(int port, enum chg_ramp_vbus_state ramp_state)
{
	int voltage;

	if (charger_get_vbus_voltage(port, &voltage))
		voltage = 0;

	if (voltage == 0) {
		CPRINTS("%s: must be disconnected", __func__);
		return 1;
	}

	if (voltage < BC12_MIN_VOLTAGE) {
		CPRINTS("%s: port %d: vbus %d lower than %d", __func__, port,
			voltage, BC12_MIN_VOLTAGE);
		return 1;
	}

	return 0;
}

#endif /* CONFIG_CHARGE_RAMP_SW */

void config_usb_db_type(void)
{
	enum ec_cfg_usb_db_type db_type = ec_cfg_usb_db_type();

	/*
	 * TODO(b/180434685): implement multiple DB types
	 */

	CPRINTS("Configured USB DB type number is %d", db_type);
}

__override int bb_retimer_power_enable(const struct usb_mux *me, bool enable)
{
	enum ioex_signal rst_signal;

	if (me->usb_port == USBC_PORT_C0) {
/* TODO: explore how to handle board id in zephyr*/
#ifndef CONFIG_ZEPHYR
		rst_signal = IOEX_USB_C0_RT_RST_ODL;
#else
		/* On Zephyr use bb_controls generated from DTS */
		rst_signal = bb_controls[me->usb_port].retimer_rst_gpio;
#endif /* !CONFIG_ZEPHYR */
	} else if (me->usb_port == USBC_PORT_C2) {
/* TODO: explore how to handle board id in zephyr*/
#ifndef CONFIG_ZEPHYR
		rst_signal = IOEX_USB_C2_RT_RST_ODL;
#else
		/* On Zephyr use bb_controls generated from DTS */
		rst_signal = bb_controls[me->usb_port].retimer_rst_gpio;
#endif /* !CONFIG_ZEPHYR */
	} else {
		return EC_ERROR_INVAL;
	}

	/*
	 * We do not have a load switch for the burnside bridge chips,
	 * so we only need to sequence reset.
	 */

	if (enable) {
		/*
		 * Tpw, minimum time from VCC to RESET_N de-assertion is 100us.
		 * For boards that don't provide a load switch control, the
		 * retimer_init() function ensures power is up before calling
		 * this function.
		 */
		ioex_set_level(rst_signal, 1);
		/*
		 * Allow 1ms time for the retimer to power up lc_domain
		 * which powers I2C controller within retimer
		 */
		crec_msleep(1);
	} else {
		ioex_set_level(rst_signal, 0);
		crec_msleep(1);
	}
	return EC_SUCCESS;
}

void board_reset_pd_mcu(void)
{
	enum gpio_signal tcpc_rst;

#ifndef CONFIG_ZEPHYR
	tcpc_rst = GPIO_USB_C0_C2_TCPC_RST_ODL;
#else
	tcpc_rst = GPIO_UNIMPLEMENTED;
#endif /* !CONFIG_ZEPHYR */

	/*
	 * TODO(b/179648104): figure out correct timing
	 */

	gpio_set_level(tcpc_rst, 0);
	if (ec_cfg_usb_db_type() != DB_USB_ABSENT) {
		gpio_set_level(GPIO_USB_C1_RST_ODL, 0);
		gpio_set_level(GPIO_USB_C1_RT_RST_R_ODL, 0);
	}

	/*
	 * delay for power-on to reset-off and min. assertion time
	 */

	crec_msleep(20);

	gpio_set_level(tcpc_rst, 1);
	if (ec_cfg_usb_db_type() != DB_USB_ABSENT) {
		gpio_set_level(GPIO_USB_C1_RST_ODL, 1);
		gpio_set_level(GPIO_USB_C1_RT_RST_R_ODL, 1);
	}

	/* wait for chips to come up */

	crec_msleep(50);
}

static void board_tcpc_init(void)
{
	/* Don't reset TCPCs after initial reset */
	if (!system_jumped_late())
		board_reset_pd_mcu();

		/*
		 * These IO expander pins are implemented using the
		 * C0/C2 TCPC, so they must be set up after the TCPC has
		 * been taken out of reset.
		 */
#ifndef CONFIG_ZEPHYR
	ioex_init(IOEX_C0_NCT38XX);
	ioex_init(IOEX_C2_NCT38XX);
#else
	gpio_reset_port(DEVICE_DT_GET(DT_NODELABEL(ioex_port1)));
	gpio_reset_port(DEVICE_DT_GET(DT_NODELABEL(ioex_port2)));
#endif

	/* Enable PPC interrupts. */
	gpio_enable_interrupt(GPIO_USB_C0_PPC_INT_ODL);
	gpio_enable_interrupt(GPIO_USB_C2_PPC_INT_ODL);

#ifndef CONFIG_ZEPHYR
	/* Enable TCPC interrupts. */
	gpio_enable_interrupt(GPIO_USB_C0_C2_TCPC_INT_ODL);

	/* Enable BC1.2 interrupts. */
	gpio_enable_interrupt(GPIO_USB_C0_BC12_INT_ODL);
	gpio_enable_interrupt(GPIO_USB_C2_BC12_INT_ODL);
#endif /* !CONFIG_ZEPHYR */

	if (ec_cfg_usb_db_type() != DB_USB_ABSENT) {
		gpio_enable_interrupt(GPIO_USB_C1_PPC_INT_ODL);
#ifndef CONFIG_ZEPHYR
		gpio_enable_interrupt(GPIO_USB_C1_TCPC_INT_ODL);
		gpio_enable_interrupt(GPIO_USB_C1_BC12_INT_ODL);
#else
	} else {
		tcpc_config[1].irq_gpio.port = NULL;
#endif /* !CONFIG_ZEPHYR */
	}
}
DECLARE_HOOK(HOOK_INIT, board_tcpc_init, HOOK_PRIO_INIT_CHIPSET);

#ifndef CONFIG_ZEPHYR
uint16_t tcpc_get_alert_status(void)
{
	uint16_t status = 0;

	if (gpio_get_level(GPIO_USB_C0_C2_TCPC_INT_ODL) == 0)
		status |= PD_STATUS_TCPC_ALERT_0 | PD_STATUS_TCPC_ALERT_2;

	if ((ec_cfg_usb_db_type() != DB_USB_ABSENT) &&
	    gpio_get_level(GPIO_USB_C1_TCPC_INT_ODL) == 0)
		status |= PD_STATUS_TCPC_ALERT_1;

	return status;
}
#endif

int ppc_get_alert_status(int port)
{
	if (port == USBC_PORT_C0)
		return gpio_get_level(GPIO_USB_C0_PPC_INT_ODL) == 0;
	else if ((port == USBC_PORT_C1) &&
		 (ec_cfg_usb_db_type() != DB_USB_ABSENT))
		return gpio_get_level(GPIO_USB_C1_PPC_INT_ODL) == 0;
	else if (port == USBC_PORT_C2)
		return gpio_get_level(GPIO_USB_C2_PPC_INT_ODL) == 0;
	return 0;
}

#ifndef CONFIG_ZEPHYR
void tcpc_alert_event(enum gpio_signal signal)
{
	switch (signal) {
	case GPIO_USB_C0_C2_TCPC_INT_ODL:
		schedule_deferred_pd_interrupt(USBC_PORT_C0);
		break;
	case GPIO_USB_C1_TCPC_INT_ODL:
		if (ec_cfg_usb_db_type() == DB_USB_ABSENT)
			break;
		schedule_deferred_pd_interrupt(USBC_PORT_C1);
		break;
	default:
		break;
	}
}
#endif

void bc12_interrupt(enum gpio_signal signal)
{
	switch (signal) {
	case GPIO_USB_C0_BC12_INT_ODL:
		usb_charger_task_set_event(0, USB_CHG_EVENT_BC12);
		break;
	case GPIO_USB_C1_BC12_INT_ODL:
		if (ec_cfg_usb_db_type() == DB_USB_ABSENT)
			break;
		usb_charger_task_set_event(1, USB_CHG_EVENT_BC12);
		break;
	case GPIO_USB_C2_BC12_INT_ODL:
		usb_charger_task_set_event(2, USB_CHG_EVENT_BC12);
		break;
	default:
		break;
	}
}

void ppc_interrupt(enum gpio_signal signal)
{
	switch (signal) {
	case GPIO_USB_C0_PPC_INT_ODL:
		syv682x_interrupt(USBC_PORT_C0);
		break;
	case GPIO_USB_C1_PPC_INT_ODL:
		switch (ec_cfg_usb_db_type()) {
		case DB_USB_ABSENT:
		case DB_USB_ABSENT2:
			break;
		case DB_USB3_PS8815:
			nx20p348x_interrupt(USBC_PORT_C1);
			break;
		}
		break;
	case GPIO_USB_C2_PPC_INT_ODL:
		syv682x_interrupt(USBC_PORT_C2);
		break;
	default:
		break;
	}
}

void retimer_interrupt(enum gpio_signal signal)
{
}

__override bool board_is_dts_port(int port)
{
	return port == USBC_PORT_C0;
}

__override bool board_is_tbt_usb4_port(int port)
{
	if (port == USBC_PORT_C0 || port == USBC_PORT_C2)
		return true;

	return false;
}

__override enum tbt_compat_cable_speed board_get_max_tbt_speed(int port)
{
	if (!board_is_tbt_usb4_port(port))
		return TBT_SS_RES_0;

	return TBT_SS_TBT_GEN3;
}
