// SPDX-License-Identifier: GPL-2.0+
/*
 * Nuvoton DRM driver
 *
 * Copyright (C) 2025 Nuvoton Technology Corp.
 *
 * Author: Joey Lu <yclu4@nuvoton.com>
 */

#include <linux/types.h>
#include <linux/clk.h>

#include <drm/drm_bridge.h>
#include <drm/drm_bridge_connector.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <drm/drm_simple_kms_helper.h>

#include "ma35_drm.h"

#define ma35_encoder(e) \
	container_of(e, struct ma35_interface, drm_encoder)

static void ma35_encoder_mode_set(struct drm_encoder *encoder,
	struct drm_crtc_state *crtc_state,
	struct drm_connector_state *conn_state)
{
	struct drm_device *drm_dev = encoder->dev;
	struct ma35_drm *priv = ma35_drm(drm_dev);
	struct drm_display_mode *adjusted_mode = &crtc_state->adjusted_mode;
	int result;

	// pixel clock it checked in crtc
	clk_set_rate(priv->dcupclk, adjusted_mode->clock * 1000);
	result = DIV_ROUND_UP(clk_get_rate(priv->dcupclk), 1000);
	drm_dbg(drm_dev, "Pixel clock: %d kHz; request : %d kHz\n", result, adjusted_mode->clock);
}

static const struct drm_encoder_helper_funcs ma35_encoder_helper_funcs = {
	.atomic_mode_set	= ma35_encoder_mode_set,
};

static void ma35_encoder_attach_crtc(struct ma35_drm *priv)
{
	uint32_t possible_crtcs = drm_crtc_mask(&priv->crtc->drm_crtc);

	priv->interface->drm_encoder.possible_crtcs = possible_crtcs;
}

static int ma35_bridge_init(struct ma35_drm *priv, struct ma35_interface *interface)
{
	struct drm_device *drm_dev = &priv->drm_dev;
	struct device *dev = drm_dev->dev;
	struct device_node *of_node = dev->of_node;
	struct drm_bridge *next_bridge;
	struct drm_connector *connector;
	int ret;

	next_bridge = devm_drm_of_get_bridge(dev, of_node, 0, 0); // port, endpoint
	if (IS_ERR(next_bridge))
		return PTR_ERR(next_bridge);

	ret = drm_bridge_attach(&interface->drm_encoder, next_bridge, NULL,
				DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret) {
		drm_err(drm_dev, "Failed to attach bridge chain to encoder\n");
		return ret;
	}

	connector = drm_bridge_connector_init(drm_dev, &interface->drm_encoder);
	if (IS_ERR(connector)) {
		drm_err(drm_dev, "Failed to create bridge connector\n");
		return PTR_ERR(connector);
	}

	ret = drm_connector_attach_encoder(connector, &interface->drm_encoder);
	if (ret) {
		drm_err(drm_dev, "Failed to attach bridge connector to encoder\n");
		return ret;
	}

	interface->drm_bridge = next_bridge;
	interface->drm_connector = connector;

	return 0;
}

/*
 * encoder -> device-tree-described bridge chain
 */
int ma35_interface_init(struct ma35_drm *priv)
{
	struct ma35_interface *interface;
	struct drm_device *drm_dev = &priv->drm_dev;
	struct drm_encoder *drm_encoder;
	int ret;

	// encoder, connector
	interface = drmm_simple_encoder_alloc(drm_dev,
					struct ma35_interface, drm_encoder, DRM_MODE_ENCODER_DPI);
	if (IS_ERR(interface)) {
		ret = PTR_ERR(interface);
		drm_err(drm_dev, "Failed to initialize encoder\n");
		goto error_early;
	}
	priv->interface = interface;
	drm_encoder = &interface->drm_encoder;
	drm_encoder_helper_add(drm_encoder,
			       &ma35_encoder_helper_funcs);

	// attach encoder to crtc
	ma35_encoder_attach_crtc(priv);

	// attach bridge chain to encoder
	ret = ma35_bridge_init(priv, interface);
	if (ret) {
		if (ret == -ENODEV)
			drm_err(drm_dev,
				"No panel/bridge remote-endpoint found for this output\n");
		else if (ret != -EPROBE_DEFER)
			drm_err(drm_dev, "Failed to attach bridge chain\n");
		goto error_encoder;
	}

	return 0;

error_encoder:
	drm_encoder_cleanup(drm_encoder);

error_early:
	return ret;
}
