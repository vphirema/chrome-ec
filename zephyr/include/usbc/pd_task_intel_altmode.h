/* Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * @file
 * @brief PD task to configure USB-C Alternate modes on Intel SoC.
 */

#ifndef __CROS_EC_PD_TASK_INTEL_ALTMODE_H
#define __CROS_EC_PD_TASK_INTEL_ALTMODE_H

#if defined(CONFIG_USB_PD_ALTMODE_INTEL) && \
	defined(CONFIG_PLATFORM_EC_USB_PD_TCPMV2)
#error "If using PD in PD mode, do not use TCPMv2"
#endif

/**
 * @brief Starts the Intel Alternate Mode configuration thread.
 */
void intel_altmode_task_start(void);

#endif /* __CROS_EC_PD_TASK_INTEL_ALTMODE_H */
