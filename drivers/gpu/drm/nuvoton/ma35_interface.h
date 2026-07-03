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

struct ma35_drm;

struct ma35_interface {
	struct drm_encoder drm_encoder;
	struct drm_connector drm_connector;
	struct drm_bridge *drm_bridge;
	struct drm_bridge *drm_bridge_panel;
};

void ma35_interface_attach_crtc(struct ma35_drm *priv);
int ma35_interface_init(struct ma35_drm *priv);

#endif
