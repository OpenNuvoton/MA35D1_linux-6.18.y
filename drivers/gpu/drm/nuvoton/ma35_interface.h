// SPDX-License-Identifier: GPL-2.0+
/*
 * Nuvoton DRM driver
 *
 * Copyright (C) 2025 Nuvoton Technology Corp.
 *
 * Author: Joey Lu <yclu4@nuvoton.com>
 */

#ifndef _MA35_INTERFACE_H_
#define _MA35_INTERFACE_H_

#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <drm/drm_encoder.h>

/*
 * VPLL (advanced PLL, integer/fractional mode):
 *   85.7 MHz <= FCLKO <= 2400 MHz
 * DCU pixel clock divider:
 *   dcup_div = VPLL / (2 * (DCUPDIV + 1))
 */
#define MA35_VPLL_MIN_HZ	85700000UL
#define MA35_DCUP_DIV_MIN	2
#define MA35_DCUP_DIV_MAX	16
#define MA35_MIN_PIXEL_CLK \
	DIV_ROUND_UP(MA35_VPLL_MIN_HZ, MA35_DCUP_DIV_MAX * 1000UL)

struct ma35_drm;

struct ma35_interface {
	struct drm_encoder drm_encoder;
	struct drm_connector *drm_connector;
	struct drm_bridge *drm_bridge;
};

int ma35_interface_init(struct ma35_drm *priv);

#endif
