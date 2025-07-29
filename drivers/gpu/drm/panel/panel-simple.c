/*
 * Copyright (C) 2013, NVIDIA Corporation.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

#include <video/display_timing.h>
#include <video/mipi_display.h>
#include <video/of_display_timing.h>
#include <video/videomode.h>

#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <drm/drm_dsc.h>

#include <linux/nvmem-consumer.h>

#include "panel-simple.h"

enum panel_simple_cmd_type {
	CMD_TYPE_DEFAULT,
	CMD_TYPE_SPI
};

struct panel_cmd_header {
	u8 data_type;
	u8 delay;
	u8 payload_length;
} __packed;

struct panel_cmd_desc {
	struct panel_cmd_header header;
	u8 *payload;
};

struct panel_cmd_seq {
	struct panel_cmd_desc *cmds;
	unsigned int cmd_cnt;
};

/**
 * @modes: Pointer to array of fixed modes appropriate for this panel.  If
 *         only one mode then this can just be the address of this the mode.
 *         NOTE: cannot be used with "timings" and also if this is specified
 *         then you cannot override the mode in the device tree.
 * @num_modes: Number of elements in modes array.
 * @timings: Pointer to array of display timings.  NOTE: cannot be used with
 *           "modes" and also these will be used to validate a device tree
 *           override if one is present.
 * @num_timings: Number of elements in timings array.
 * @bpc: Bits per color.
 * @size: Structure containing the physical size of this panel.
 * @delay: Structure containing various delay values for this panel.
 * @bus_format: See MEDIA_BUS_FMT_... defines.
 * @bus_flags: See DRM_BUS_FLAG_... defines.
 */
struct panel_desc {
	const struct drm_display_mode *modes;
	unsigned int num_modes;
	const struct display_timing *timings;
	unsigned int num_timings;

	unsigned int bpc;

	/**
	 * @width: width (in millimeters) of the panel's active display area
	 * @height: height (in millimeters) of the panel's active display area
	 */
	struct {
		unsigned int width;
		unsigned int height;
	} size;

	/**
	 * @prepare: the time (in milliseconds) that it takes for the panel to
	 *           become ready and start receiving video data
	 * @hpd_absent_delay: Add this to the prepare delay if we know Hot
	 *                    Plug Detect isn't used.
	 * @enable: the time (in milliseconds) that it takes for the panel to
	 *          display the first valid frame after starting to receive
	 *          video data
	 * @disable: the time (in milliseconds) that it takes for the panel to
	 *           turn the display off (no content is visible)
	 * @unprepare: the time (in milliseconds) that it takes for the panel
	 *             to power itself down completely
	 * @reset: the time (in milliseconds) that it takes for the panel
	 *         to reset itself completely
	 * @init: the time (in milliseconds) that it takes for the panel to
	 *	  send init command sequence after reset deassert
	 */
	struct {
		unsigned int prepare;
		unsigned int hpd_absent_delay;
		unsigned int enable;
		unsigned int disable;
		unsigned int unprepare;
		unsigned int reset;
		unsigned int init;
	} delay;

	u32 bus_format;
	u32 bus_flags;
	int connector_type;

	struct panel_cmd_seq *init_seq;
	struct panel_cmd_seq *exit_seq;

	enum panel_simple_cmd_type cmd_type;

	int (*spi_read)(struct device *dev, const u8 cmd, u8 *val);
	int (*spi_write)(struct device *dev, const u8 *data, size_t len, u8 type);
};

struct panel_simple {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;
	bool prepared;
	bool enabled;
	bool power_invert;
	bool no_hpd;

	const struct panel_desc *desc;

	struct regulator *supply;
	struct i2c_adapter *ddc;

	struct gpio_desc *enable_gpio;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *hpd_gpio;
	struct device *dev;

	struct drm_display_mode override_mode;

	struct drm_dsc_picture_parameter_set *pps;
	enum drm_panel_orientation orientation;
};


struct entry {
    u32 offset;
    u32 length;
};

/**
 * @magic: LCD config firmware magic number. 
 * @vendor: LCD vendor name.
 * @model: LCD model name.
 * @version: LCD config firmware version.
 * @timing_entry: Entry of timing table.
 * @init_seq_entry: Entry of init sequence.
 * @eixt_seq_entry: Entry of exit sequence.
 * @touchscreen_entry: Entry of touchscreen properties.
 * @firmware_size: Firmware size.
 */
struct firmware_header {
	u32 magic;
    u8 vendor[16];
    u8 model[16];
    u8 version[8];
    struct entry timing_entry;
    struct entry init_seq_entry;
	struct entry eixt_seq_entry;
	struct entry touchscreen_entry;
	struct entry vesa_dsc_entry;
	struct entry other_info_entry;
	u32 firmware_size;
};

// Sysfs操作函数
static ssize_t reset_show(struct device *dev, struct device_attribute *attr, char *buf) {
    struct panel_simple *panel = dev_get_drvdata(dev);
    return sprintf(buf, "%d\n", gpiod_get_value(panel->reset_gpio));
}

static ssize_t reset_store(struct device *dev, struct device_attribute *attr,
                         const char *buf, size_t count) {
    struct panel_simple *panel = dev_get_drvdata(dev);
    unsigned long value;
    int ret;

    ret = kstrtoul(buf, 0, &value);
    if (ret < 0) return ret;

    gpiod_set_value(panel->reset_gpio, value ? 1 : 0);
    return count;
}
static DEVICE_ATTR(reset, 0644, reset_show, reset_store);

static inline void panel_simple_msleep(unsigned int msecs)
{
	usleep_range(msecs * 1000, msecs * 1000 + 100);
}

static inline struct panel_simple *to_panel_simple(struct drm_panel *panel)
{
	return container_of(panel, struct panel_simple, base);
}

static int panel_simple_parse_cmd_seq(struct device *dev,
				      const u8 *data, int length,
				      struct panel_cmd_seq *seq)
{
	struct panel_cmd_header *header;
	struct panel_cmd_desc *desc;
	char *buf, *d;
	unsigned int i, cnt, len;

	if (!seq)
		return -EINVAL;

	buf = devm_kmemdup(dev, data, length, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	d = buf;
	len = length;
	cnt = 0;
	while (len > sizeof(*header)) {
		header = (struct panel_cmd_header *)d;

		d += sizeof(*header);
		len -= sizeof(*header);

		if (header->payload_length > len)
			return -EINVAL;

		d += header->payload_length;
		len -= header->payload_length;
		cnt++;
	}

	if (len)
		return -EINVAL;

	seq->cmd_cnt = cnt;
	seq->cmds = devm_kcalloc(dev, cnt, sizeof(*desc), GFP_KERNEL);
	if (!seq->cmds)
		return -ENOMEM;

	d = buf;
	len = length;
	for (i = 0; i < cnt; i++) {
		header = (struct panel_cmd_header *)d;
		len -= sizeof(*header);
		d += sizeof(*header);

		desc = &seq->cmds[i];
		desc->header = *header;
		desc->payload = d;

		d += header->payload_length;
		len -= header->payload_length;
	}

	return 0;
}

static int panel_simple_xfer_dsi_cmd_seq(struct panel_simple *panel,
					 struct panel_cmd_seq *seq)
{
	struct device *dev = panel->base.dev;
	struct mipi_dsi_device *dsi = panel->dsi;
	unsigned int i;
	int err;

	if (!IS_ENABLED(CONFIG_DRM_MIPI_DSI))
		return -EINVAL;
	if (!seq)
		return -EINVAL;

	for (i = 0; i < seq->cmd_cnt; i++) {
		struct panel_cmd_desc *cmd = &seq->cmds[i];

		switch (cmd->header.data_type) {
		case MIPI_DSI_COMPRESSION_MODE:
			err = mipi_dsi_compression_mode(dsi, cmd->payload[0]);
			break;
		case MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM:
		case MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM:
		case MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM:
		case MIPI_DSI_GENERIC_LONG_WRITE:
			err = mipi_dsi_generic_write(dsi, cmd->payload,
						     cmd->header.payload_length);
			break;
		case MIPI_DSI_DCS_SHORT_WRITE:
		case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
		case MIPI_DSI_DCS_LONG_WRITE:
			err = mipi_dsi_dcs_write_buffer(dsi, cmd->payload,
							cmd->header.payload_length);
			break;
		case MIPI_DSI_PICTURE_PARAMETER_SET:
			if (!panel->pps) {
				panel->pps = devm_kzalloc(dev, sizeof(*panel->pps),
							  GFP_KERNEL);
				if (!panel->pps)
					return -ENOMEM;

				memcpy(panel->pps, cmd->payload, cmd->header.payload_length);
			}

			err = mipi_dsi_picture_parameter_set(dsi, panel->pps);
			break;
		default:
			return -EINVAL;
		}

		if (err < 0)
			dev_err(dev, "failed to write dcs cmd: %d\n", err);

		if (cmd->header.delay)
			panel_simple_msleep(cmd->header.delay);
	}

	return 0;
}

static int panel_simple_xfer_spi_cmd_seq(struct panel_simple *panel, struct panel_cmd_seq *cmds)
{
	int i;
	int ret;

	if (!cmds)
		return -EINVAL;

	for (i = 0; i < cmds->cmd_cnt; i++) {
		struct panel_cmd_desc *cmd = &cmds->cmds[i];

		ret = panel->desc->spi_write(panel->base.dev, cmd->payload,
					     cmd->header.payload_length, cmd->header.data_type);
		if (ret)
			return ret;

		if (cmd->header.delay)
			panel_simple_msleep(cmd->header.delay);
	}

	return 0;
}

static unsigned int panel_simple_get_timings_modes(struct panel_simple *panel,
						   struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	unsigned int i, num = 0;

	for (i = 0; i < panel->desc->num_timings; i++) {
		const struct display_timing *dt = &panel->desc->timings[i];
		struct videomode vm;

		videomode_from_timing(dt, &vm);
		mode = drm_mode_create(connector->dev);
		if (!mode) {
			dev_err(panel->base.dev, "failed to add mode %ux%u\n",
				dt->hactive.typ, dt->vactive.typ);
			continue;
		}

		drm_display_mode_from_videomode(&vm, mode);

		mode->type |= DRM_MODE_TYPE_DRIVER;

		if (panel->desc->num_timings == 1)
			mode->type |= DRM_MODE_TYPE_PREFERRED;

		drm_mode_probed_add(connector, mode);
		num++;
	}

	return num;
}

static unsigned int panel_simple_get_display_modes(struct panel_simple *panel,
						   struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	unsigned int i, num = 0;

	for (i = 0; i < panel->desc->num_modes; i++) {
		const struct drm_display_mode *m = &panel->desc->modes[i];

		mode = drm_mode_duplicate(connector->dev, m);
		if (!mode) {
			dev_err(panel->base.dev, "failed to add mode %ux%u@%u\n",
				m->hdisplay, m->vdisplay,
				drm_mode_vrefresh(m));
			continue;
		}

		mode->type |= DRM_MODE_TYPE_DRIVER;

		if (panel->desc->num_modes == 1)
			mode->type |= DRM_MODE_TYPE_PREFERRED;

		drm_mode_set_name(mode);

		drm_mode_probed_add(connector, mode);
		num++;
	}

	return num;
}

static int panel_simple_get_non_edid_modes(struct panel_simple *panel,
					   struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	bool has_override = panel->override_mode.type;
	unsigned int num = 0;

	if (!panel->desc)
		return 0;

	if (has_override) {
		mode = drm_mode_duplicate(connector->dev,
					  &panel->override_mode);
		if (mode) {
			drm_mode_probed_add(connector, mode);
			num = 1;
		} else {
			dev_err(panel->base.dev, "failed to add override mode\n");
		}
	}

	/* Only add timings if override was not there or failed to validate */
	if (num == 0 && panel->desc->num_timings)
		num = panel_simple_get_timings_modes(panel, connector);

	/*
	 * Only add fixed modes if timings/override added no mode.
	 *
	 * We should only ever have either the display timings specified
	 * or a fixed mode. Anything else is rather bogus.
	 */
	WARN_ON(panel->desc->num_timings && panel->desc->num_modes);
	if (num == 0)
		num = panel_simple_get_display_modes(panel, connector);

	if (panel->desc->bpc)
		connector->display_info.bpc = panel->desc->bpc;
	if (panel->desc->size.width)
		connector->display_info.width_mm = panel->desc->size.width;
	if (panel->desc->size.height)
		connector->display_info.height_mm = panel->desc->size.height;
	if (panel->desc->bus_format)
		drm_display_info_set_bus_formats(&connector->display_info,
						 &panel->desc->bus_format, 1);
	if (panel->desc->bus_flags)
		connector->display_info.bus_flags = panel->desc->bus_flags;

	return num;
}

static int panel_simple_regulator_enable(struct panel_simple *p)
{
	int err;

	if (p->power_invert) {
		if (regulator_is_enabled(p->supply) > 0)
			regulator_disable(p->supply);
	} else {
		err = regulator_enable(p->supply);
		if (err < 0)
			return err;
	}

	return 0;
}

static int panel_simple_regulator_disable(struct panel_simple *p)
{
	int err;

	if (p->power_invert) {
		if (!regulator_is_enabled(p->supply)) {
			err = regulator_enable(p->supply);
			if (err < 0)
				return err;
		}
	} else {
		regulator_disable(p->supply);
	}

	return 0;
}

int panel_simple_loader_protect(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);
	int err;

	err = panel_simple_regulator_enable(p);
	if (err < 0) {
		dev_err(panel->dev, "failed to enable supply: %d\n", err);
		return err;
	}

	p->prepared = true;
	p->enabled = true;

	return 0;
}
EXPORT_SYMBOL(panel_simple_loader_protect);

static int panel_simple_disable(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);

	if (!p->enabled)
		return 0;

	if (p->desc->delay.disable)
		panel_simple_msleep(p->desc->delay.disable);

	p->enabled = false;

	return 0;
}

static int panel_simple_unprepare(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);

	if (!p->prepared)
		return 0;

	if (p->desc->exit_seq) {
		if (p->desc->cmd_type == CMD_TYPE_SPI) {
			if (panel_simple_xfer_spi_cmd_seq(p, p->desc->exit_seq)) {
				dev_err(panel->dev, "failed to send exit spi cmds seq\n");
				return -EINVAL;
			}
		} else {
			if (p->dsi)
				panel_simple_xfer_dsi_cmd_seq(p, p->desc->exit_seq);
		}
	}

	gpiod_direction_output(p->reset_gpio, 1);
	gpiod_direction_output(p->enable_gpio, 0);

	panel_simple_regulator_disable(p);

	if (p->desc->delay.unprepare)
		panel_simple_msleep(p->desc->delay.unprepare);

	p->prepared = false;

	return 0;
}

static int panel_simple_get_hpd_gpio(struct device *dev,
				     struct panel_simple *p, bool from_probe)
{
	int err;

	p->hpd_gpio = devm_gpiod_get_optional(dev, "hpd", GPIOD_IN);
	if (IS_ERR(p->hpd_gpio)) {
		err = PTR_ERR(p->hpd_gpio);

		/*
		 * If we're called from probe we won't consider '-EPROBE_DEFER'
		 * to be an error--we'll leave the error code in "hpd_gpio".
		 * When we try to use it we'll try again.  This allows for
		 * circular dependencies where the component providing the
		 * hpd gpio needs the panel to init before probing.
		 */
		if (err != -EPROBE_DEFER || !from_probe) {
			dev_err(dev, "failed to get 'hpd' GPIO: %d\n", err);
			return err;
		}
	}

	return 0;
}

static int panel_simple_prepare(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);
	unsigned int delay;
	int err;
	int hpd_asserted;

	if (p->prepared)
		return 0;

	err = panel_simple_regulator_enable(p);
	if (err < 0) {
		dev_err(panel->dev, "failed to enable supply: %d\n", err);
		return err;
	}

	gpiod_direction_output(p->enable_gpio, 1);

	delay = p->desc->delay.prepare;
	if (p->no_hpd)
		delay += p->desc->delay.hpd_absent_delay;
	if (delay)
		panel_simple_msleep(delay);

	if (p->hpd_gpio) {
		if (IS_ERR(p->hpd_gpio)) {
			err = panel_simple_get_hpd_gpio(panel->dev, p, false);
			if (err)
				return err;
		}

		err = readx_poll_timeout(gpiod_get_value_cansleep, p->hpd_gpio,
					 hpd_asserted, hpd_asserted,
					 1000, 2000000);
		if (hpd_asserted < 0)
			err = hpd_asserted;

		if (err) {
			dev_err(panel->dev,
				"error waiting for hpd GPIO: %d\n", err);
			return err;
		}
	}

	gpiod_direction_output(p->reset_gpio, 1);

	if (p->desc->delay.reset)
		panel_simple_msleep(p->desc->delay.reset);

	gpiod_direction_output(p->reset_gpio, 0);

	if (p->desc->delay.init)
		panel_simple_msleep(p->desc->delay.init);

	if (p->desc->init_seq) {
		if (p->desc->cmd_type == CMD_TYPE_SPI) {
			if (panel_simple_xfer_spi_cmd_seq(p, p->desc->init_seq)) {
				dev_err(panel->dev, "failed to send init spi cmds seq\n");
				return -EINVAL;
			}
		} else {
			if (p->dsi)
				panel_simple_xfer_dsi_cmd_seq(p, p->desc->init_seq);
		}
	}

	p->prepared = true;

	return 0;
}

static int panel_simple_enable(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);

	if (p->enabled)
		return 0;

	if (p->desc->delay.enable)
		panel_simple_msleep(p->desc->delay.enable);

	p->enabled = true;

	return 0;
}

static int panel_simple_get_modes(struct drm_panel *panel,
				  struct drm_connector *connector)
{
	struct panel_simple *p = to_panel_simple(panel);
	int num = 0;

	/* probe EDID if a DDC bus is available */
	if (p->ddc) {
		struct edid *edid = drm_get_edid(connector, p->ddc);

		drm_connector_update_edid_property(connector, edid);
		if (edid) {
			num += drm_add_edid_modes(connector, edid);
			kfree(edid);
		}
	}

	/* add hard-coded panel modes */
	num += panel_simple_get_non_edid_modes(p, connector);

	/* set up connector's "panel orientation" property */
	drm_connector_set_panel_orientation(connector, p->orientation);

	return num;
}

static int panel_simple_get_timings(struct drm_panel *panel,
				    unsigned int num_timings,
				    struct display_timing *timings)
{
	struct panel_simple *p = to_panel_simple(panel);
	unsigned int i;

	if (p->desc->num_timings < num_timings)
		num_timings = p->desc->num_timings;

	if (timings)
		for (i = 0; i < num_timings; i++)
			timings[i] = p->desc->timings[i];

	return p->desc->num_timings;
}

static const struct drm_panel_funcs panel_simple_funcs = {
	.disable = panel_simple_disable,
	.unprepare = panel_simple_unprepare,
	.prepare = panel_simple_prepare,
	.enable = panel_simple_enable,
	.get_modes = panel_simple_get_modes,
	.get_timings = panel_simple_get_timings,
};

static struct panel_desc panel_dpi;

static int panel_dpi_probe(struct device *dev,
			   struct panel_simple *panel)
{
	struct display_timing *timing;
	const struct device_node *np;
	struct panel_desc *desc;
	unsigned int bus_flags;
	struct videomode vm;
	int ret;

	np = dev->of_node;
	desc = devm_kzalloc(dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	timing = devm_kzalloc(dev, sizeof(*timing), GFP_KERNEL);
	if (!timing)
		return -ENOMEM;

	ret = of_get_display_timing(np, "panel-timing", timing);
	if (ret < 0) {
		dev_err(dev, "%pOF: no panel-timing node found for \"panel-dpi\" binding\n",
			np);
		return ret;
	}

	desc->timings = timing;
	desc->num_timings = 1;

	of_property_read_u32(np, "width-mm", &desc->size.width);
	of_property_read_u32(np, "height-mm", &desc->size.height);

	/* Extract bus_flags from display_timing */
	bus_flags = 0;
	vm.flags = timing->flags;
	drm_bus_flags_from_videomode(&vm, &bus_flags);
	desc->bus_flags = bus_flags;

	/* We do not know the connector for the DT node, so guess it */
	desc->connector_type = DRM_MODE_CONNECTOR_DPI;

	panel->desc = desc;

	return 0;
}

#define PANEL_SIMPLE_BOUNDS_CHECK(to_check, bounds, field) \
	(to_check->field.typ >= bounds->field.min && \
	 to_check->field.typ <= bounds->field.max)
static void panel_simple_parse_panel_timing_node(struct device *dev,
						 struct panel_simple *panel,
						 const struct display_timing *ot)
{
	const struct panel_desc *desc = panel->desc;
	struct videomode vm;
	unsigned int i;

	if (WARN_ON(desc->num_modes)) {
		dev_err(dev, "Reject override mode: panel has a fixed mode\n");
		return;
	}
	if (WARN_ON(!desc->num_timings)) {
		dev_err(dev, "Reject override mode: no timings specified\n");
		return;
	}

	for (i = 0; i < panel->desc->num_timings; i++) {
		const struct display_timing *dt = &panel->desc->timings[i];

		if (!PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, hactive) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, hfront_porch) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, hback_porch) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, hsync_len) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, vactive) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, vfront_porch) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, vback_porch) ||
		    !PANEL_SIMPLE_BOUNDS_CHECK(ot, dt, vsync_len))
			continue;

		if (ot->flags != dt->flags)
			continue;

		videomode_from_timing(ot, &vm);
		drm_display_mode_from_videomode(&vm, &panel->override_mode);
		panel->override_mode.type |= DRM_MODE_TYPE_DRIVER |
					     DRM_MODE_TYPE_PREFERRED;
		break;
	}

	if (WARN_ON(!panel->override_mode.type))
		dev_err(dev, "Reject override mode: No display_timing found\n");
}

static int dcs_bl_update_status(struct backlight_device *bl)
{
	struct panel_simple *p = bl_get_data(bl);
	struct mipi_dsi_device *dsi = p->dsi;
	int ret;

	if (!p->prepared)
		return 0;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness(dsi, bl->props.brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

static int dcs_bl_get_brightness(struct backlight_device *bl)
{
	struct panel_simple *p = bl_get_data(bl);
	struct mipi_dsi_device *dsi = p->dsi;
	u16 brightness = bl->props.brightness;
	int ret;

	if (!p->prepared)
		return 0;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness(dsi, &brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return brightness & 0xff;
}

static const struct backlight_ops dcs_bl_ops = {
	.update_status = dcs_bl_update_status,
	.get_brightness = dcs_bl_get_brightness,
};

static int panel_simple_probe(struct device *dev, const struct panel_desc *desc)
{
	struct panel_simple *panel;
	struct display_timing dt;
	struct device_node *ddc;
	int connector_type;
	u32 bus_flags;
	int err;

	panel = devm_kzalloc(dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;

	panel->enabled = false;
	panel->prepared = false;
	panel->desc = desc;

	panel->no_hpd = of_property_read_bool(dev->of_node, "no-hpd");
	if (!panel->no_hpd) {
		err = panel_simple_get_hpd_gpio(dev, panel, true);
		if (err)
			return err;
	}

	panel->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(panel->supply)) {
		err = PTR_ERR(panel->supply);
		dev_err(dev, "failed to get power regulator: %d\n", err);
		return err;
	}

	panel->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_ASIS);
	if (IS_ERR(panel->enable_gpio)) {
		err = PTR_ERR(panel->enable_gpio);
		if (err != -EPROBE_DEFER)
			dev_err(dev, "failed to get enable GPIO: %d\n", err);
		return err;
	}

	panel->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(panel->reset_gpio)) {
		err = PTR_ERR(panel->reset_gpio);
		if (err != -EPROBE_DEFER)
			dev_err(dev, "failed to get reset GPIO: %d\n", err);
		return err;
	}

	err = of_drm_get_panel_orientation(dev->of_node, &panel->orientation);
	if (err) {
		dev_err(dev, "%pOF: failed to get orientation %d\n", dev->of_node, err);
		return err;
	}

	panel->power_invert = of_property_read_bool(dev->of_node, "power-invert");

	ddc = of_parse_phandle(dev->of_node, "ddc-i2c-bus", 0);
	if (ddc) {
		panel->ddc = of_find_i2c_adapter_by_node(ddc);
		of_node_put(ddc);

		if (!panel->ddc) {
			err = -EPROBE_DEFER;
			dev_err(dev, "failed to find ddc-i2c-bus: %d\n", err);
			return err;
		}
	}

	if (desc == &panel_dpi) {
		/* Handle the generic panel-dpi binding */
		err = panel_dpi_probe(dev, panel);
		if (err)
			goto free_ddc;
		desc = panel->desc;
	} else {
		if (!of_get_display_timing(dev->of_node, "panel-timing", &dt))
			panel_simple_parse_panel_timing_node(dev, panel, &dt);
	}

	connector_type = desc->connector_type;
	/* Catch common mistakes for panels. */
	switch (connector_type) {
	case 0:
		dev_dbg(dev, "Specify missing connector_type\n");
		connector_type = DRM_MODE_CONNECTOR_DPI;
		break;
	case DRM_MODE_CONNECTOR_LVDS:
		WARN_ON(desc->bus_flags &
			~(DRM_BUS_FLAG_DE_LOW |
			  DRM_BUS_FLAG_DE_HIGH |
			  DRM_BUS_FLAG_DATA_MSB_TO_LSB |
			  DRM_BUS_FLAG_DATA_LSB_TO_MSB));
		WARN_ON(desc->bus_format != MEDIA_BUS_FMT_RGB666_1X7X3_SPWG &&
			desc->bus_format != MEDIA_BUS_FMT_RGB888_1X7X4_SPWG &&
			desc->bus_format != MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA);
		WARN_ON(desc->bus_format == MEDIA_BUS_FMT_RGB666_1X7X3_SPWG &&
			desc->bpc != 6);
		WARN_ON((desc->bus_format == MEDIA_BUS_FMT_RGB888_1X7X4_SPWG ||
			 desc->bus_format == MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA) &&
			desc->bpc != 8);
		break;
	case DRM_MODE_CONNECTOR_eDP:
		if (desc->bus_format == 0)
			dev_warn(dev, "Specify missing bus_format\n");
		if (desc->bpc != 6 && desc->bpc != 8)
			dev_warn(dev, "Expected bpc in {6,8} but got: %u\n", desc->bpc);
		break;
	case DRM_MODE_CONNECTOR_DSI:
		if (desc->bpc != 6 && desc->bpc != 8)
			dev_warn(dev, "Expected bpc in {6,8} but got: %u\n", desc->bpc);
		break;
	case DRM_MODE_CONNECTOR_DPI:
		bus_flags = DRM_BUS_FLAG_DE_LOW |
			    DRM_BUS_FLAG_DE_HIGH |
			    DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE |
			    DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE |
			    DRM_BUS_FLAG_DATA_MSB_TO_LSB |
			    DRM_BUS_FLAG_DATA_LSB_TO_MSB |
			    DRM_BUS_FLAG_SYNC_SAMPLE_POSEDGE |
			    DRM_BUS_FLAG_SYNC_SAMPLE_NEGEDGE;
		if (desc->bus_flags & ~bus_flags)
			dev_warn(dev, "Unexpected bus_flags(%d)\n", desc->bus_flags & ~bus_flags);
		if (!(desc->bus_flags & bus_flags))
			dev_warn(dev, "Specify missing bus_flags\n");
		if (desc->bus_format == 0)
			dev_warn(dev, "Specify missing bus_format\n");
		if (desc->bpc != 6 && desc->bpc != 8)
			dev_warn(dev, "Expected bpc in {6,8} but got: %u\n", desc->bpc);
		break;
	default:
		dev_warn(dev, "Specify a valid connector_type: %d\n", desc->connector_type);
		connector_type = DRM_MODE_CONNECTOR_DPI;
		break;
	}

	drm_panel_init(&panel->base, dev, &panel_simple_funcs, connector_type);

	err = drm_panel_of_backlight(&panel->base);
	if (err) {
		dev_err(dev, "failed to find backlight: %d\n", err);
		goto free_ddc;
	}

	drm_panel_add(&panel->base);

	dev_set_drvdata(dev, panel);

	panel->dev = dev;
	if (device_create_file(dev, &dev_attr_reset)) {
		dev_warn(dev, "Failed to create reset sysfs file\n");
	}else{
		dev_info(dev, "create reset sysfs file success\n");
	}
	return 0;

free_ddc:
	if (panel->ddc)
		put_device(&panel->ddc->dev);

	return err;
}

static int panel_simple_remove(struct device *dev)
{
	struct panel_simple *panel = dev_get_drvdata(dev);

	drm_panel_remove(&panel->base);
	drm_panel_disable(&panel->base);
	drm_panel_unprepare(&panel->base);

	if (panel->ddc)
		put_device(&panel->ddc->dev);

	device_remove_file(dev, &dev_attr_reset);
	return 0;
}

static void panel_simple_shutdown(struct device *dev)
{
	struct panel_simple *panel = dev_get_drvdata(dev);

	drm_panel_disable(&panel->base);
	drm_panel_unprepare(&panel->base);
}

static const struct drm_display_mode ivo_m133nwf4_r0_mode = {
	.clock = 138778,
	.hdisplay = 1920,
	.hsync_start = 1920 + 24,
	.hsync_end = 1920 + 24 + 48,
	.htotal = 1920 + 24 + 48 + 88,
	.vdisplay = 1080,
	.vsync_start = 1080 + 3,
	.vsync_end = 1080 + 3 + 12,
	.vtotal = 1080 + 3 + 12 + 17,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
};

static const struct panel_desc ivo_m133nwf4_r0 = {
	.modes = &ivo_m133nwf4_r0_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 294,
		.height = 165,
	},
	.delay = {
		.hpd_absent_delay = 200,
		.unprepare = 500,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DATA_MSB_TO_LSB,
	.connector_type = DRM_MODE_CONNECTOR_eDP,
};

static const struct of_device_id platform_of_match[] = {
	{
		.compatible = "simple-panel",
		.data = NULL,
	}, {
		.compatible = "ivo,m133nwf4-r0",
		.data = &ivo_m133nwf4_r0,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, platform_of_match);

static bool of_child_node_is_present(const struct device_node *node,
				     const char *name)
{
	struct device_node *child;

	child = of_get_child_by_name(node, name);
	of_node_put(child);

	return !!child;
}

static int panel_simple_of_get_firmware_desc_data_binfile(const char *file_path,struct device *dev,
					   struct panel_desc *desc)
{
    struct device_node *np = dev->of_node; // 从dev获取设备树节点
    struct file *fp;
    struct firmware_header *header;
    struct drm_display_mode *mode;
    struct videomode *vm;
    const u8 *init_data , *exit_data ;
    u32 bus_flags;
    loff_t pos = 0;
    int ret;

	/* 1. 打开文件 */
	dev_info(dev, "==============================================");
	dev_info(dev, "Loading panel config from: %s\n", file_path);
    fp = filp_open(file_path, O_RDONLY, 0);
    if (IS_ERR(fp)) {
		dev_err(dev, "Can't open %s: err=%ld (path exists? mounted? perm?)\n",file_path, PTR_ERR(fp));
        return PTR_ERR(fp);
    }
    header = (struct firmware_header *)devm_kzalloc(dev, sizeof(*header), GFP_KERNEL);
	if (!header){
        ret= -ENOMEM;
        goto err_close;
    }

    /* 2. 读取文件头 */
    ret = kernel_read(fp, header, sizeof(*header), &pos);
    if (ret < 0) {
        dev_err(dev, "Failed to read firmware header: %d\n", ret);
        goto err_close;
    }

    /* 3. 校验固件有效性 */
    if (header->magic != 0xA5A55A5A || header->firmware_size <= 0) {
        dev_err(dev, "Invalid firmware: magic=0x%x, size=%u\n",
               header->magic, header->firmware_size);
        ret = -EINVAL;
        goto err_close;
    }
    if (header->firmware_size < sizeof(*header)) {
        dev_err(dev, "Invalid firmware size: %u < %zu\n",
            header->firmware_size, sizeof(*header));
        ret = -EINVAL;
        goto err_close;
    }
    /* 4. 打印关键信息（完全匹配原函数格式） */
    dev_info(dev, "panel firmware magic: %x\n", header->magic);
    dev_info(dev, "panel firmware version: %.*s, size: %d\n", 8 ,header->version, header->firmware_size);
    dev_info(dev, "panel vendor: %.*s, model: %.*s\n", 16 ,header->vendor, 16 ,header->model);

    vm = (struct videomode *)devm_kzalloc(dev, sizeof(*vm), GFP_KERNEL);
    if (!vm){
		ret= -ENOMEM;
        goto err_close;}

    /*  读取时序数据 */
    if (header->timing_entry.length != sizeof(*vm)) {
        dev_err(dev, "Invalid timing data size: expected %zu, got %u\n", sizeof(*vm), header->timing_entry.length);
        ret = -EINVAL;
        goto err_close;
    }
    pos = header->timing_entry.offset;
    ret = kernel_read(fp, vm, sizeof(*vm), &pos);
    if (ret != sizeof(*vm)) {
        dev_err(dev, "Failed to read timing data: %d\n", ret);
        goto err_close;
    }

    /* 读取初始化序列（devm自动释放） */
    init_data = (const u8 *)devm_kzalloc(dev, header->init_seq_entry.length, GFP_KERNEL);
    if (!init_data) {
        ret = -ENOMEM;
        goto err_close;
    }
    pos = header->init_seq_entry.offset;
    ret = kernel_read(fp, (void *)init_data, header->init_seq_entry.length, &pos);
    if (ret < 0) {
        dev_err(dev, "Failed to read init sequence: %d\n", ret);
        goto err_close;
    }

    /*读取关闭序列（devm自动释放） */
    exit_data = (const u8 *)devm_kzalloc(dev, header->eixt_seq_entry.length, GFP_KERNEL);
    if (!exit_data) {
        ret = -ENOMEM;
        goto err_close;
    }

    pos = header->eixt_seq_entry.offset;
    ret = kernel_read(fp, (void *)exit_data, header->eixt_seq_entry.length, &pos);
    if (ret < 0) {
        dev_err(dev, "Failed to read exit sequence: %d\n", ret);
        goto err_close;
    }

    /* 分配模式内存（使用devm自动管理） */
    mode = (struct drm_display_mode*)devm_kzalloc(dev, sizeof(*mode), GFP_KERNEL);
    if (!mode) {
        ret = -ENOMEM;
        goto err_close;
    }

    drm_display_mode_from_videomode(vm, mode);
    drm_bus_flags_from_videomode(vm, &bus_flags);

    /* 填充panel_desc */
    desc->modes = mode;
    desc->num_modes = 1;
    desc->bus_flags = bus_flags;
    /* 从设备树读取delay参数 */
    of_property_read_u32(np, "prepare-delay-ms", &desc->delay.prepare);
    of_property_read_u32(np, "enable-delay-ms", &desc->delay.enable);
    of_property_read_u32(np, "disable-delay-ms", &desc->delay.disable);
    of_property_read_u32(np, "unprepare-delay-ms", &desc->delay.unprepare);
    of_property_read_u32(np, "reset-delay-ms", &desc->delay.reset);
    of_property_read_u32(np, "init-delay-ms", &desc->delay.init);

    /* 解析命令序列 */
    desc->init_seq = devm_kzalloc(dev, sizeof(*desc->init_seq), GFP_KERNEL);
    if (!desc->init_seq) {
        ret = -ENOMEM;
        goto err_close;
    }
    ret = panel_simple_parse_cmd_seq(dev, init_data, header->init_seq_entry.length,desc->init_seq);
    if (ret) {
        dev_err(dev, "Failed to parse init sequence\n");
        goto err_close;
    }

    if (init_data) {
        devm_kfree(dev, (void *)init_data);
        init_data = NULL;
    }
    desc->exit_seq = devm_kzalloc(dev, sizeof(*desc->exit_seq), GFP_KERNEL);
    if (!desc->exit_seq) {
        ret = -ENOMEM;
        goto err_close;
    }

    ret = panel_simple_parse_cmd_seq(dev, exit_data, header->eixt_seq_entry.length,desc->exit_seq);
    if (ret) {
        dev_err(dev, "Failed to parse exit sequence\n");
        goto err_close;
    }
    if (exit_data) {
        devm_kfree(dev, (void *)exit_data);
        exit_data = NULL;
    }
    filp_close(fp, NULL);
    return 0;

err_close:
    filp_close(fp, NULL);
    return ret;
}
/*
static int panel_simple_of_get_firmware_desc_data(struct device *dev,
					   struct panel_desc *desc)
{
	struct device_node *np = dev->of_node;
	struct nvmem_device *nvmem;
	struct firmware_header *header;
	struct drm_display_mode *mode;
	struct videomode *vm;
	const u8 *init_data;
	const u8 *exit_data;
	u32 bus_flags;
	int ret;

	nvmem = devm_nvmem_device_get(dev, "eeprom");
	if (IS_ERR(nvmem))
		return PTR_ERR(nvmem);

	header = (struct firmware_header *)devm_kzalloc(dev, sizeof(*header), GFP_KERNEL);
	if (!header)
		return -ENOMEM;

	ret = nvmem_device_read(nvmem, 0, sizeof(*header), header);
	if (ret < 0) {
		dev_err(dev, "failed to read firmware header: %d\n", ret);
		return ret;
	}

	if (header->firmware_size <= 0 || header->magic != 0xDEAD5A5A) {
		dev_err(dev, "Invalid eeprom firmware");
		return -EINVAL;
	}

	dev_info(dev, "lcd firmware magic: %x\n", header->magic);
	dev_info(dev, "lcd firmware version: %s, size: %d\n", header->version, header->firmware_size);
	dev_info(dev, "lcd vendor: %s, model: %s\n", header->vendor, header->model);
	

	vm = (struct videomode *)devm_kzalloc(dev, sizeof(*vm), GFP_KERNEL);
	if (!vm)
		return -ENOMEM;

	ret = nvmem_device_read(nvmem, header->timing_entry.offset,
				  header->timing_entry.length, vm);
	if (ret < 0)
		return ret;

	init_data = (const u8 *)devm_kzalloc(dev, header->init_seq_entry.length, GFP_KERNEL);
	if (!init_data)
		return -ENOMEM;
		
	ret = nvmem_device_read(nvmem, header->init_seq_entry.offset,
				  header->init_seq_entry.length,
				  (void *)init_data);
	if (ret < 0)
		return ret;
				
	exit_data = (const u8 *)devm_kzalloc(dev, header->eixt_seq_entry.length,
				  GFP_KERNEL);
	if (!exit_data)
		return -ENOMEM;

	ret = nvmem_device_read(nvmem, header->eixt_seq_entry.offset,
				  header->eixt_seq_entry.length,
				  (void *)exit_data);
	if (ret < 0)
		return ret;

	devm_nvmem_device_put(dev, nvmem);

	mode = (struct drm_display_mode*)devm_kzalloc(dev, sizeof(*mode), GFP_KERNEL);
	if (!mode)
		return -ENOMEM;
	
	drm_display_mode_from_videomode(vm, mode);
	drm_bus_flags_from_videomode(vm, &bus_flags);
	
	desc->modes = mode;
	desc->num_modes = 1;
	desc->bus_flags = bus_flags;

	of_property_read_u32(np, "prepare-delay-ms", &desc->delay.prepare);
	of_property_read_u32(np, "enable-delay-ms", &desc->delay.enable);
	of_property_read_u32(np, "disable-delay-ms", &desc->delay.disable);
	of_property_read_u32(np, "unprepare-delay-ms", &desc->delay.unprepare);
	of_property_read_u32(np, "reset-delay-ms", &desc->delay.reset);
	of_property_read_u32(np, "init-delay-ms", &desc->delay.init);
		
	desc->init_seq = devm_kzalloc(dev, sizeof(*desc->init_seq),
				      GFP_KERNEL);
	if (!desc->init_seq)
		return -ENOMEM;

	ret = panel_simple_parse_cmd_seq(dev, init_data, header->init_seq_entry.length,
						 desc->init_seq);
	if (ret) {
		dev_err(dev, "failed to parse init sequence\n");
		return ret;
	}

	desc->exit_seq = devm_kzalloc(dev, sizeof(*desc->exit_seq),
				      GFP_KERNEL);
	if (!desc->exit_seq)
		return -ENOMEM;

	ret = panel_simple_parse_cmd_seq(dev, exit_data, header->eixt_seq_entry.length,
						 desc->exit_seq);
	if (ret) {
		dev_err(dev, "failed to parse exit sequence\n");
		return ret;
	}	

	return 0;
}
*/
static int panel_simple_of_get_desc_data(struct device *dev,
					 struct panel_desc *desc)
{
	struct device_node *np = dev->of_node;
	u32 bus_flags;
	const void *data;
	int len;
	int err;

	if (of_child_node_is_present(np, "display-timings")) {
		struct drm_display_mode *mode;

		mode = devm_kzalloc(dev, sizeof(*mode), GFP_KERNEL);
		if (!mode)
			return -ENOMEM;

		if (!of_get_drm_display_mode(np, mode, &bus_flags,
					     OF_USE_NATIVE_MODE)) {
			desc->modes = mode;
			desc->num_modes = 1;
			desc->bus_flags = bus_flags;
		}
	} else if (of_child_node_is_present(np, "panel-timing")) {
		struct display_timing *timing;
		struct videomode vm;

		timing = devm_kzalloc(dev, sizeof(*timing), GFP_KERNEL);
		if (!timing)
			return -ENOMEM;

		if (!of_get_display_timing(np, "panel-timing", timing)) {
			desc->timings = timing;
			desc->num_timings = 1;

			bus_flags = 0;
			vm.flags = timing->flags;
			drm_bus_flags_from_videomode(&vm, &bus_flags);
			desc->bus_flags = bus_flags;
		}
	}

	if (desc->num_modes || desc->num_timings) {
		of_property_read_u32(np, "bpc", &desc->bpc);
		of_property_read_u32(np, "bus-format", &desc->bus_format);
		of_property_read_u32(np, "width-mm", &desc->size.width);
		of_property_read_u32(np, "height-mm", &desc->size.height);
	}

	of_property_read_u32(np, "prepare-delay-ms", &desc->delay.prepare);
	of_property_read_u32(np, "enable-delay-ms", &desc->delay.enable);
	of_property_read_u32(np, "disable-delay-ms", &desc->delay.disable);
	of_property_read_u32(np, "unprepare-delay-ms", &desc->delay.unprepare);
	of_property_read_u32(np, "reset-delay-ms", &desc->delay.reset);
	of_property_read_u32(np, "init-delay-ms", &desc->delay.init);

	data = of_get_property(np, "panel-init-sequence", &len);
	if (data) {
		desc->init_seq = devm_kzalloc(dev, sizeof(*desc->init_seq),
					      GFP_KERNEL);
		if (!desc->init_seq)
			return -ENOMEM;

		err = panel_simple_parse_cmd_seq(dev, data, len,
						 desc->init_seq);
		if (err) {
			dev_err(dev, "failed to parse init sequence\n");
			return err;
		}
	}

	data = of_get_property(np, "panel-exit-sequence", &len);
	if (data) {
		desc->exit_seq = devm_kzalloc(dev, sizeof(*desc->exit_seq),
					      GFP_KERNEL);
		if (!desc->exit_seq)
			return -ENOMEM;

		err = panel_simple_parse_cmd_seq(dev, data, len,
						 desc->exit_seq);
		if (err) {
			dev_err(dev, "failed to parse exit sequence\n");
			return err;
		}
	}

	return 0;
}

static int panel_simple_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct of_device_id *id;
	const struct panel_desc *desc;
	struct panel_desc *d;
	int err;

	id = of_match_node(platform_of_match, pdev->dev.of_node);
	if (!id)
		return -ENODEV;

	if (!id->data) {
		d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
		if (!d)
			return -ENOMEM;

		err = panel_simple_of_get_desc_data(dev, d);
		if (err) {
			dev_err(dev, "failed to get desc data: %d\n", err);
			return err;
		}
	}

	desc = id->data ? id->data : d;

	return panel_simple_probe(&pdev->dev, desc);
}

static int panel_simple_platform_remove(struct platform_device *pdev)
{
	return panel_simple_remove(&pdev->dev);
}

static void panel_simple_platform_shutdown(struct platform_device *pdev)
{
	panel_simple_shutdown(&pdev->dev);
}

static struct platform_driver panel_simple_platform_driver = {
	.driver = {
		.name = "panel-simple",
		.of_match_table = platform_of_match,
	},
	.probe = panel_simple_platform_probe,
	.remove = panel_simple_platform_remove,
	.shutdown = panel_simple_platform_shutdown,
};

struct panel_desc_dsi {
	struct panel_desc desc;

	unsigned long flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;
};

static const struct drm_display_mode boe_tv080wum_nl0_mode = {
	.clock = 160000,
	.hdisplay = 1200,
	.hsync_start = 1200 + 120,
	.hsync_end = 1200 + 120 + 20,
	.htotal = 1200 + 120 + 20 + 21,
	.vdisplay = 1920,
	.vsync_start = 1920 + 21,
	.vsync_end = 1920 + 21 + 3,
	.vtotal = 1920 + 21 + 3 + 18,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc_dsi boe_tv080wum_nl0 = {
	.desc = {
		.modes = &boe_tv080wum_nl0_mode,
		.num_modes = 1,
		.size = {
			.width = 107,
			.height = 172,
		},
		.connector_type = DRM_MODE_CONNECTOR_DSI,
	},
	.flags = MIPI_DSI_MODE_VIDEO |
		 MIPI_DSI_MODE_VIDEO_BURST |
		 MIPI_DSI_MODE_VIDEO_SYNC_PULSE,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};

static const struct of_device_id dsi_of_match[] = {
	{
		.compatible = "simple-panel-dsi",
		.data = NULL,
	}, {
		.compatible = "boe,tv080wum-nl0",
		.data = &boe_tv080wum_nl0
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, dsi_of_match);

static int panel_simple_dsi_of_get_desc_data(struct device *dev,
					     struct panel_desc_dsi *desc)
{
	struct device_node *np = dev->of_node;
	u32 val;
	int err;
	const struct drm_display_mode *mode;
	const char *format_str = "Unknown";
	const char *fw_path = "/lib/firmware/vis-timing.bin";

	err = panel_simple_of_get_firmware_desc_data_binfile(fw_path,dev, &desc->desc);
	if (!err) {
		dev_info(dev,  "Using panel parameters from firmware\n");
	} else {
		dev_info(dev, "not found firmware or Bad firmware,use device-tree config\n");
		err = panel_simple_of_get_desc_data(dev, &desc->desc);
		if (err)
			return err;
	}

	if (!of_property_read_u32(np, "dsi,flags", &val))
		desc->flags = val;
	if (!of_property_read_u32(np, "dsi,format", &val))
		desc->format = val;
	if (!of_property_read_u32(np, "dsi,lanes", &val))
		desc->lanes = val;
	if (!of_property_read_u32(np, "size,width", &val))
		desc->desc.size.width = val;
	if (!of_property_read_u32(np, "size,height", &val))
		desc->desc.size.height = val;

	/* Print all parameters in categorized format */
	dev_info(dev, "===== Panel Configuration Summary =====\n");

	/* 1. Basic Display Parameters */
	if (desc->desc.modes && desc->desc.num_modes > 0) {
		mode = &desc->desc.modes[0];
		dev_info(dev, "[Display Timing]\n");
		dev_info(dev, "  clock: %d kHz\n", mode->clock);
		dev_info(dev, "  hdisplay: %d, vdisplay: %d\n",
			 mode->hdisplay, mode->vdisplay);
		dev_info(dev, "  hsync: start=%d end=%d len=%d\n",
			 mode->hsync_start, mode->hsync_end,
			 mode->hsync_end - mode->hsync_start);
		dev_info(dev, "  hback-porch: %d, hfront-porch: %d\n",
			 mode->htotal - mode->hsync_end,
			 mode->hsync_start - mode->hdisplay);
		dev_info(dev, "  vsync: start=%d end=%d len=%d\n",
			 mode->vsync_start, mode->vsync_end,
			 mode->vsync_end - mode->vsync_start);
		dev_info(dev, "  vback-porch: %d, vfront-porch: %d\n",
			 mode->vtotal - mode->vsync_end,
			 mode->vsync_start - mode->vdisplay);
	}

	/* 2. Physical Characteristics */
	dev_info(dev, "[Physical Properties]\n");
	dev_info(dev, "  width: %d mm, height: %d mm\n",
		 desc->desc.size.width, desc->desc.size.height);
	/* 3. DSI Specific Parameters */
	dev_info(dev, "[DSI Parameters]\n");
	dev_info(dev, "  dsi,flags: 0x%lx\n", desc->flags);

	/* Manual mapping for DSI pixel format */
	switch (desc->format) {
	case MIPI_DSI_FMT_RGB888: format_str = "RGB888"; break;
	case MIPI_DSI_FMT_RGB666: format_str = "RGB666"; break;
	case MIPI_DSI_FMT_RGB666_PACKED: format_str = "RGB666 (Packed)"; break;
	case MIPI_DSI_FMT_RGB565: format_str = "RGB565"; break;
	}
	dev_info(dev, "  dsi,format: %d (%s)\n", desc->format, format_str);
	dev_info(dev, "  dsi,lanes: %d\n", desc->lanes);

	/* 4. Power Sequence */
	dev_info(dev, "[Power Sequence]\n");
	dev_info(dev, "  prepare: %d ms, enable: %d ms\n",
		 desc->desc.delay.prepare, desc->desc.delay.enable);
	dev_info(dev, "  disable: %d ms, unprepare: %d ms\n",
		 desc->desc.delay.disable, desc->desc.delay.unprepare);
	dev_info(dev, "  reset: %d ms, init: %d ms\n",
		 desc->desc.delay.reset, desc->desc.delay.init);

	/* 5. Command Sequences (if exist) */
	if (desc->desc.init_seq) {
		dev_info(dev, "[Init Sequence] %d commands\n",
			 desc->desc.init_seq->cmd_cnt);
	}

	dev_info(dev, "======================================\n");
	return 0;
}

static int panel_simple_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct panel_simple *panel;
	struct device *dev = &dsi->dev;
	const struct panel_desc_dsi *desc;
	struct panel_desc_dsi *d;
	const struct of_device_id *id;
	int err;

	id = of_match_node(dsi_of_match, dsi->dev.of_node);
	if (!id)
		return -ENODEV;

	if (!id->data) {
		d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
		if (!d)
			return -ENOMEM;

		err = panel_simple_dsi_of_get_desc_data(dev, d);
		if (err) {
			dev_err(dev, "failed to get desc data: %d\n", err);
			return err;
		}
	}

	desc = id->data ? id->data : d;

	err = panel_simple_probe(&dsi->dev, &desc->desc);
	if (err < 0)
		return err;

	panel = dev_get_drvdata(dev);
	panel->dsi = dsi;

	if (!panel->base.backlight) {
		struct backlight_properties props;

		memset(&props, 0, sizeof(props));
		props.type = BACKLIGHT_RAW;
		props.brightness = 255;
		props.max_brightness = 255;

		panel->base.backlight =
			devm_backlight_device_register(dev, dev_name(dev),
						       dev, panel, &dcs_bl_ops,
						       &props);
		if (IS_ERR(panel->base.backlight)) {
			err = PTR_ERR(panel->base.backlight);
			dev_err(dev, "failed to register dcs backlight: %d\n",
				err);
			return err;
		}
	}

	dsi->mode_flags = desc->flags;
	dsi->format = desc->format;
	dsi->lanes = desc->lanes;

	err = mipi_dsi_attach(dsi);
	if (err) {
		struct panel_simple *panel = dev_get_drvdata(&dsi->dev);

		drm_panel_remove(&panel->base);
	}

	return err;
}

static int panel_simple_dsi_remove(struct mipi_dsi_device *dsi)
{
	int err;

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", err);

	return panel_simple_remove(&dsi->dev);
}

static void panel_simple_dsi_shutdown(struct mipi_dsi_device *dsi)
{
	panel_simple_shutdown(&dsi->dev);
}

static struct mipi_dsi_driver panel_simple_dsi_driver = {
	.driver = {
		.name = "panel-simple-dsi",
		.of_match_table = dsi_of_match,
	},
	.probe = panel_simple_dsi_probe,
	.remove = panel_simple_dsi_remove,
	.shutdown = panel_simple_dsi_shutdown,
};

static int panel_simple_spi_read(struct device *dev, const u8 cmd, u8 *data)
{
	return 0;
}

static int panel_simple_spi_write_word(struct device *dev, u16 data)
{
	struct spi_device *spi = to_spi_device(dev);
	struct spi_transfer xfer = {
		.len	= 2,
		.tx_buf = &data,
	};
	struct spi_message msg;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	return spi_sync(spi, &msg);
}

static int panel_simple_spi_write(struct device *dev, const u8 *data, size_t len, u8 type)
{
	int ret = 0;
	int i;
	u16 mask = type ? 0x100 : 0;

	for (i = 0; i < len; i++) {
		ret = panel_simple_spi_write_word(dev, *data | mask);
		if (ret) {
			dev_err(dev, "failed to write spi seq: %*ph\n", (int)len, data);
			return ret;
		}
		data++;
	}

	return ret;
}

static const struct of_device_id panel_simple_spi_of_match[] = {
	{ .compatible = "simple-panel-spi", .data = NULL },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, panel_simple_spi_of_match);

static int panel_simple_spi_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	const struct of_device_id *id;
	const struct panel_desc *desc;
	struct panel_desc *d;
	int ret;

	id = of_match_node(panel_simple_spi_of_match, dev->of_node);
	if (!id)
		return -ENODEV;

	if (!id->data) {
		d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
		if (!d)
			return -ENOMEM;

		ret = panel_simple_of_get_desc_data(dev, d);
		if (ret) {
			dev_err(dev, "failed to get desc data: %d\n", ret);
			return ret;
		}

		d->spi_write = panel_simple_spi_write;
		d->spi_read = panel_simple_spi_read;
		d->cmd_type = CMD_TYPE_SPI;
	}
	desc = id->data ? id->data : d;

	/*
	 * Set spi to 3 lines and 9bits/word mode.
	 */
	spi->bits_per_word = 9;
	spi->mode = SPI_MODE_3;
	ret = spi_setup(spi);
	if (ret < 0) {
		dev_err(dev, "spi setup failed.\n");
		return ret;
	}

	return panel_simple_probe(dev, desc);
}

static int panel_simple_spi_remove(struct spi_device *spi)
{
	return panel_simple_remove(&spi->dev);
}

static void panel_simple_spi_shutdown(struct spi_device *spi)
{
	panel_simple_shutdown(&spi->dev);
}

static struct spi_driver panel_simple_spi_driver = {
	.driver	= {
		.name		= "panel-simple-spi",
		.of_match_table = panel_simple_spi_of_match,
	},
	.probe			= panel_simple_spi_probe,
	.remove			= panel_simple_spi_remove,
	.shutdown		= panel_simple_spi_shutdown,
};

static int __init panel_simple_init(void)
{
	int err;

	err = platform_driver_register(&panel_simple_platform_driver);
	if (err < 0)
		return err;

	if (IS_ENABLED(CONFIG_SPI_MASTER)) {
		err = spi_register_driver(&panel_simple_spi_driver);
		if (err < 0)
			return err;
	}

	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI)) {
		err = mipi_dsi_driver_register(&panel_simple_dsi_driver);
		if (err < 0)
			return err;
	}

	return 0;
}
// module_init(panel_simple_init);
late_initcall(panel_simple_init);

static void __exit panel_simple_exit(void)
{
	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI))
		mipi_dsi_driver_unregister(&panel_simple_dsi_driver);

	if (IS_ENABLED(CONFIG_SPI_MASTER))
		spi_unregister_driver(&panel_simple_spi_driver);

	platform_driver_unregister(&panel_simple_platform_driver);
}
module_exit(panel_simple_exit);

MODULE_AUTHOR("Thierry Reding <treding@nvidia.com>");
MODULE_DESCRIPTION("DRM Driver for Simple Panels");
MODULE_LICENSE("GPL and additional rights");
