// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) Rockchip Electronics Co.Ltd
 * Author:
 *      Guochun Huang <hero.huang@rock-chips.com>
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/gpio.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pm_runtime.h>
#include <linux/printk.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/mfd/syscon.h>
#include <linux/phy/phy.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_dsc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <video/mipi_display.h>
#include <video/videomode.h>
#include <asm/unaligned.h>
#include <uapi/linux/videodev2.h>
#include <drm/drm_panel.h>
#include <drm/drm_connector.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#include "rockchip_drm_drv.h"
#include "rockchip_drm_vop.h"
#include <linux/miscdevice.h>
#include <linux/uaccess.h>

#define UPDATE(v, h, l)			(((v) << (l)) & GENMASK((h), (l)))

#define DSI2_PWR_UP			0x000c
#define RESET				0
#define POWER_UP			BIT(0)
#define CMD_TX_MODE(x)			UPDATE(x,  24,  24)
#define DSI2_SOFT_RESET			0x0010
#define SYS_RSTN			BIT(2)
#define PHY_RSTN			BIT(1)
#define IPI_RSTN			BIT(0)
#define INT_ST_MAIN			0x0014
#define DSI2_MODE_CTRL			0x0018
#define DSI2_MODE_STATUS		0x001c
#define DSI2_CORE_STATUS		0x0020
#define PRI_RD_DATA_AVAIL		BIT(26)
#define PRI_FIFOS_NOT_EMPTY		BIT(25)
#define PRI_BUSY			BIT(24)
#define CRI_RD_DATA_AVAIL		BIT(18)
#define CRT_FIFOS_NOT_EMPTY		BIT(17)
#define CRI_BUSY			BIT(16)
#define IPI_FIFOS_NOT_EMPTY		BIT(9)
#define IPI_BUSY			BIT(8)
#define CORE_FIFOS_NOT_EMPTY		BIT(1)
#define CORE_BUSY			BIT(0)
#define MANUAL_MODE_CFG			0x0024
#define MANUAL_MODE_EN			BIT(0)
#define DSI2_TIMEOUT_HSTX_CFG		0x0048
#define TO_HSTX(x)			UPDATE(x, 15, 0)
#define DSI2_TIMEOUT_HSTXRDY_CFG	0x004c
#define TO_HSTXRDY(x)			UPDATE(x, 15, 0)
#define DSI2_TIMEOUT_LPRX_CFG		0x0050
#define TO_LPRXRDY(x)			UPDATE(x, 15, 0)
#define DSI2_TIMEOUT_LPTXRDY_CFG	0x0054
#define TO_LPTXRDY(x)			UPDATE(x, 15, 0)
#define DSI2_TIMEOUT_LPTXTRIG_CFG	0x0058
#define TO_LPTXTRIG(x)			UPDATE(x, 15, 0)
#define DSI2_TIMEOUT_LPTXULPS_CFG	0x005c
#define TO_LPTXULPS(x)			UPDATE(x, 15, 0)
#define DSI2_TIMEOUT_BTA_CFG		0x60
#define TO_BTA(x)			UPDATE(x, 15, 0)

#define DSI2_PHY_MODE_CFG		0x0100
#define PPI_WIDTH(x)			UPDATE(x, 9, 8)
#define PHY_LANES(x)			UPDATE(x - 1, 5, 4)
#define PHY_TYPE(x)			UPDATE(x, 0, 0)
#define DSI2_PHY_CLK_CFG		0X0104
#define PHY_LPTX_CLK_DIV(x)		UPDATE(x, 12, 8)
#define CLK_TYPE_MASK			BIT(0)
#define NON_CONTINUOUS_CLK		BIT(0)
#define CONTIUOUS_CLK			0
#define DSI2_PHY_LP2HS_MAN_CFG		0x010c
#define PHY_LP2HS_TIME(x)		UPDATE(x, 28, 0)
#define DSI2_PHY_HS2LP_MAN_CFG		0x0114
#define PHY_HS2LP_TIME(x)		UPDATE(x, 28, 0)
#define DSI2_PHY_MAX_RD_T_MAN_CFG	0x011c
#define PHY_MAX_RD_TIME(x)		UPDATE(x, 26, 0)
#define DSI2_PHY_ESC_CMD_T_MAN_CFG	0x0124
#define PHY_ESC_CMD_TIME(x)		UPDATE(x, 28, 0)
#define DSI2_PHY_ESC_BYTE_T_MAN_CFG	0x012c
#define PHY_ESC_BYTE_TIME(x)		UPDATE(x, 28, 0)

#define DSI2_PHY_IPI_RATIO_MAN_CFG	0x0134
#define PHY_IPI_RATIO(x)		UPDATE(x, 21, 0)
#define DSI2_PHY_SYS_RATIO_MAN_CFG	0x013C
#define PHY_SYS_RATIO(x)		UPDATE(x, 16, 0)

#define DSI2_DSI_GENERAL_CFG		0x0200
#define BTA_EN				BIT(1)
#define EOTP_TX_EN			BIT(0)
#define DSI2_DSI_VCID_CFG		0x0204
#define TX_VCID(x)			UPDATE(x, 1, 0)
#define DSI2_DSI_SCRAMBLING_CFG		0x0208
#define SCRAMBLING_SEED(x)		UPDATE(x, 31, 16)
#define SCRAMBLING_EN			BIT(0)
#define DSI2_DSI_VID_TX_CFG		0x020c
#define LPDT_DISPLAY_CMD_EN		BIT(20)
#define BLK_VFP_HS_EN			BIT(14)
#define BLK_VBP_HS_EN			BIT(13)
#define BLK_VSA_HS_EN			BIT(12)
#define BLK_HFP_HS_EN			BIT(6)
#define BLK_HBP_HS_EN			BIT(5)
#define BLK_HSA_HS_EN			BIT(4)
#define VID_MODE_TYPE(x)		UPDATE(x, 1, 0)
#define DSI2_CRI_TX_HDR			0x02c0
#define CMD_TX_MODE(x)			UPDATE(x, 24, 24)
#define DSI2_CRI_TX_PLD			0x02c4
#define DSI2_CRI_RX_HDR			0x02c8
#define DSI2_CRI_RX_PLD			0x02cc

#define DSI2_IPI_COLOR_MAN_CFG		0x0300
#define IPI_DEPTH(x)			UPDATE(x, 7, 4)
#define IPI_DEPTH_5_6_5_BITS		0x02
#define IPI_DEPTH_6_BITS		0x03
#define IPI_DEPTH_8_BITS		0x05
#define IPI_DEPTH_10_BITS		0x06
#define IPI_FORMAT(x)			UPDATE(x, 3, 0)
#define IPI_FORMAT_RGB			0x0
#define IPI_FORMAT_DSC			0x0b
#define DSI2_IPI_VID_HSA_MAN_CFG	0x0304
#define VID_HSA_TIME(x)			UPDATE(x, 29, 0)
#define DSI2_IPI_VID_HBP_MAN_CFG	0x030c
#define VID_HBP_TIME(x)			UPDATE(x, 29, 0)
#define DSI2_IPI_VID_HACT_MAN_CFG	0x0314
#define VID_HACT_TIME(x)		UPDATE(x, 29, 0)
#define DSI2_IPI_VID_HLINE_MAN_CFG	0x031c
#define VID_HLINE_TIME(x)		UPDATE(x, 29, 0)
#define DSI2_IPI_VID_VSA_MAN_CFG	0x0324
#define VID_VSA_LINES(x)		UPDATE(x, 9, 0)
#define DSI2_IPI_VID_VBP_MAN_CFG	0X032C
#define VID_VBP_LINES(x)		UPDATE(x, 9, 0)
#define DSI2_IPI_VID_VACT_MAN_CFG	0X0334
#define VID_VACT_LINES(x)		UPDATE(x, 13, 0)
#define DSI2_IPI_VID_VFP_MAN_CFG	0X033C
#define VID_VFP_LINES(x)		UPDATE(x, 9, 0)
#define DSI2_IPI_PIX_PKT_CFG		0x0344
#define MAX_PIX_PKT(x)			UPDATE(x, 15, 0)

#define DSI2_INT_ST_PHY			0x0400
#define DSI2_INT_MASK_PHY		0x0404
#define DSI2_INT_ST_TO			0x0410
#define DSI2_INT_MASK_TO		0x0414
#define DSI2_INT_ST_ACK			0x0420
#define DSI2_INT_MASK_ACK		0x0424
#define DSI2_INT_ST_IPI			0x0430
#define DSI2_INT_MASK_IPI		0x0434
#define DSI2_INT_ST_FIFO		0x0440
#define DSI2_INT_MASK_FIFO		0x0444
#define DSI2_INT_ST_PRI			0x0450
#define DSI2_INT_MASK_PRI		0x0454
#define DSI2_INT_ST_CRI			0x0460
#define DSI2_INT_MASK_CRI		0x0464
#define DSI2_INT_FORCE_CRI		0x0468
#define DSI2_MAX_REGISGER		DSI2_INT_FORCE_CRI

#define MODE_STATUS_TIMEOUT_US		10000
#define CMD_PKT_STATUS_TIMEOUT_US	20000
#define PSEC_PER_SEC			1000000000000LL

#define GRF_REG_FIELD(reg, lsb, msb)	(((reg) << 16) | ((lsb) << 8) | (msb))

enum vid_mode_type {
	VID_MODE_TYPE_NON_BURST_SYNC_PULSES,
	VID_MODE_TYPE_NON_BURST_SYNC_EVENTS,
	VID_MODE_TYPE_BURST,
};

enum mode_ctrl {
	IDLE_MODE,
	AUTOCALC_MODE,
	COMMAND_MODE,
	VIDEO_MODE,
	DATA_STREAM_MODE,
	VIDE_TEST_MODE,
	DATA_STREAM_TEST_MODE,
};

enum grf_reg_fields {
	TXREQCLKHS_EN,
	GATING_EN,
	IPI_SHUTDN,
	IPI_COLORM,
	IPI_COLOR_DEPTH,
	IPI_FORMAT,
	MAX_FIELDS,
};

enum phy_type {
	DPHY,
	CPHY,
};

enum ppi_width {
	PPI_WIDTH_8_BITS,
	PPI_WIDTH_16_BITS,
	PPI_WIDTH_32_BITS,
};

struct cmd_header {
	u8 cmd_type;
	u8 delay;
	u8 payload_length;
};

struct dw_mipi_dsi2_plat_data {
	const u32 *dsi0_grf_reg_fields;
	const u32 *dsi1_grf_reg_fields;
	unsigned long long dphy_max_bit_rate_per_lane;
	unsigned long long cphy_max_symbol_rate_per_lane;

};

struct dw_mipi_dsi2 {
	struct drm_device *drm_dev;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_bridge *bridge;
	struct mipi_dsi_host host;
	struct drm_panel *panel;
	struct drm_display_mode mode;
	struct device *dev;
	struct device_node *client;
	struct regmap *grf;
	struct clk *pclk;
	struct clk *sys_clk;
	bool phy_enabled;
	struct phy *dcphy;
	union phy_configure_opts phy_opts;

	bool disable_hold_mode;
	bool auto_calc_mode;
	bool c_option;
	bool scrambling_en;
	unsigned int slice_width;
	unsigned int slice_height;
	bool dsc_enable;
	u8 version_major;
	u8 version_minor;

	struct drm_dsc_picture_parameter_set *pps;
	struct regmap *regmap;
	struct reset_control *apb_rst;
	int irq;
	int id;

	/* dual-channel */
	struct dw_mipi_dsi2 *master;
	struct dw_mipi_dsi2 *slave;
	bool data_swap;

	unsigned int lane_hs_rate; /* Mbps or Msps per lane */
	u32 channel;
	u32 lanes;
	u32 format;
	unsigned long mode_flags;
	u64 mipi_pixel_rate;
	const struct dw_mipi_dsi2_plat_data *pdata;
	struct rockchip_drm_sub_dev sub_dev;

	struct gpio_desc *te_gpio;

	struct miscdevice miscdev;
	u8 *rx_buf;		// 接收缓冲区 dsi_raw_read 使用
	size_t rx_len;	// 当前有效数据长度 dsi_raw_read 使用
	bool data_ready;// 数据就绪标志
	/* split with other display interface */
	bool dual_connector_split;
	bool left_display;
	u32 split_area;

	bool support_psr;
	bool enabled;
};

static inline struct dw_mipi_dsi2 *host_to_dsi2(struct mipi_dsi_host *host)
{
	return container_of(host, struct dw_mipi_dsi2, host);
}

static inline struct dw_mipi_dsi2 *con_to_dsi2(struct drm_connector *con)
{
	return container_of(con, struct dw_mipi_dsi2, connector);
}

static inline struct dw_mipi_dsi2 *encoder_to_dsi2(struct drm_encoder *encoder)
{
	return container_of(encoder, struct dw_mipi_dsi2, encoder);
}

/*
 * 把 DSI2 最终拿到的 PPS（Picture Parameter Set）完整打出来。
 * 这样可以同时看到：
 * 1) 原始 0x0A 数据包负载；
 * 2) 驱动解析后的关键字段；
 * 方便和 vis-timing.bin / panel-init-sequence / 实际运行参数三边对照。
 */
static void dw_mipi_dsi2_dump_dsc_pps(struct device *dev,
				      const char *source,
				      const struct drm_dsc_picture_parameter_set *pps,
				      u32 payload_len)
{
	u16 bits_per_pixel_x16;
	u16 initial_xmit_delay;
	u16 scale_decrement_interval;

	if (!pps) {
		dev_info(dev, "[dsi2] %s PPS: <null>\n", source);
		return;
	}

	dev_info(dev, "[dsi2] ==================== %s PPS BEGIN ====================\n",
		 source);
	dev_info(dev, "[dsi2] payload_len           : %u\n", payload_len);
	dev_info(dev, "[dsi2] raw:\n");
	print_hex_dump(KERN_INFO, "[dsi2]   ", DUMP_PREFIX_OFFSET,
		       16, 1, pps, payload_len, false);

	if (payload_len < sizeof(*pps)) {
		dev_warn(dev,
			 "[dsi2] %s PPS too short: %u < %zu, skip parsed dump\n",
			 source, payload_len, sizeof(*pps));
		dev_info(dev,
			 "[dsi2] ===================== %s PPS END =====================\n",
			 source);
		return;
	}

	/*
	 * 下面这些字段最常用于确认 DSC 是否按预期生效：
	 * - dsc_version：协议版本
	 * - pic/slice 宽高：整图与分片参数
	 * - bits_per_pixel：目标压缩位宽（Q4 格式，需 /16 看整数值）
	 */
	/*
	 * 这棵 5.10 内核里的 struct drm_dsc_picture_parameter_set 没有把
	 * bits_per_component / line_buf_depth / bits_per_pixel / initial_xmit_delay
	 * 全部拆成独立字段，而是保留了 PPS 原始字节布局，因此这里按位拼回人类可读值。
	 */
	bits_per_pixel_x16 = ((pps->pps_4 & 0x3) << 8) | pps->bits_per_pixel_low;
	initial_xmit_delay = ((pps->initial_xmit_delay_high & 0x3) << 8) |
			     pps->initial_xmit_delay_low;
	scale_decrement_interval =
		((pps->scale_decrement_interval_high & 0x0f) << 8) |
		 pps->scale_decrement_interval_low;
	dev_info(dev, "[dsi2] parsed:\n");
	dev_info(dev, "[dsi2]   dsc_version         : 0x%02x (1.%u)\n",
		 pps->dsc_version, pps->dsc_version & 0x0f);
	dev_info(dev, "[dsi2]   bits_per_component  : %u\n",
		 (pps->pps_3 >> 4) & 0x0f);
	dev_info(dev, "[dsi2]   line_buf_depth      : %u\n",
		 pps->pps_3 & 0x0f);
	dev_info(dev, "[dsi2]   block_pred_enable   : %u\n",
		 !!(pps->pps_4 & 0x20));
	dev_info(dev, "[dsi2]   bits_per_pixel_x16  : %u\n",
		 bits_per_pixel_x16);
	dev_info(dev, "[dsi2]   bits_per_pixel      : %u\n",
		 bits_per_pixel_x16 / 16);
	dev_info(dev, "[dsi2]   pic_width           : %u\n",
		 be16_to_cpu(pps->pic_width));
	dev_info(dev, "[dsi2]   pic_height          : %u\n",
		 be16_to_cpu(pps->pic_height));
	dev_info(dev, "[dsi2]   slice_width         : %u\n",
		 be16_to_cpu(pps->slice_width));
	dev_info(dev, "[dsi2]   slice_height        : %u\n",
		 be16_to_cpu(pps->slice_height));
	dev_info(dev, "[dsi2]   chunk_size          : %u\n",
		 be16_to_cpu(pps->chunk_size));
	dev_info(dev, "[dsi2]   initial_xmit_delay  : %u\n",
		 initial_xmit_delay);
	dev_info(dev, "[dsi2]   initial_dec_delay   : %u\n",
		 be16_to_cpu(pps->initial_dec_delay));
	dev_info(dev, "[dsi2]   initial_scale_value : %u\n",
		 pps->initial_scale_value);
	dev_info(dev, "[dsi2]   scale_increment_int : %u\n",
		 pps->scale_increment_interval);
	dev_info(dev, "[dsi2]   scale_decrement_int : %u\n",
		 scale_decrement_interval);
	dev_info(dev, "[dsi2] ===================== %s PPS END =====================\n",
		 source);
}

static void grf_field_write(struct dw_mipi_dsi2 *dsi2, enum grf_reg_fields index,
			    unsigned int val)
{
	const u32 field = dsi2->id ?
			  dsi2->pdata->dsi1_grf_reg_fields[index] :
			  dsi2->pdata->dsi0_grf_reg_fields[index];
	u16 reg;
	u8 msb, lsb;

	if (!field)
		return;

	reg = (field >> 16) & 0xffff;
	lsb = (field >>  8) & 0xff;
	msb = (field >>  0) & 0xff;

	regmap_write(dsi2->grf, reg, (val << lsb) | (GENMASK(msb, lsb) << 16));
}

static int dw_mipi_dsi2_is_cmd_mode(struct dw_mipi_dsi2 *dsi2)
{
	if (!(dsi2->mode_flags & MIPI_DSI_MODE_VIDEO))
		return true;

	return false;
}

static int cri_fifos_wait_avail(struct dw_mipi_dsi2 *dsi2)
{
	u32 sts, mask;
	int ret;

	mask = CRI_BUSY | CRT_FIFOS_NOT_EMPTY;
	ret = regmap_read_poll_timeout(dsi2->regmap, DSI2_CORE_STATUS, sts,
				       !(sts & mask), 0,
				       CMD_PKT_STATUS_TIMEOUT_US);
	if (ret < 0) {
		DRM_DEV_ERROR(dsi2->dev, "command interface is busy\n");
		return ret;
	}

	return 0;
}

static void dw_mipi_dsi2_irq_enable(struct dw_mipi_dsi2 *dsi2, bool enable)
{
	if (enable) {
		regmap_write(dsi2->regmap, DSI2_INT_MASK_PHY, 0x1);
		regmap_write(dsi2->regmap, DSI2_INT_MASK_TO, 0xf);
		regmap_write(dsi2->regmap, DSI2_INT_MASK_ACK, 0x1);
		regmap_write(dsi2->regmap, DSI2_INT_MASK_IPI, 0x1);
		regmap_write(dsi2->regmap, DSI2_INT_MASK_FIFO, 0x1);
		regmap_write(dsi2->regmap, DSI2_INT_MASK_PRI, 0x1);
		regmap_write(dsi2->regmap, DSI2_INT_MASK_CRI, 0x1);
	} else {
		regmap_write(dsi2->regmap, DSI2_INT_MASK_PHY, 0x0);
		regmap_write(dsi2->regmap, DSI2_INT_MASK_TO, 0x0);
		regmap_write(dsi2->regmap, DSI2_INT_MASK_ACK, 0x0);
		regmap_write(dsi2->regmap, DSI2_INT_MASK_IPI, 0x0);
		regmap_write(dsi2->regmap, DSI2_INT_MASK_FIFO, 0x0);
		regmap_write(dsi2->regmap, DSI2_INT_MASK_PRI, 0x0);
		regmap_write(dsi2->regmap, DSI2_INT_MASK_CRI, 0x0);
	};
}

static void mipi_dcphy_power_on(struct dw_mipi_dsi2 *dsi2)
{
	if (dsi2->phy_enabled)
		return;

	if (dsi2->dcphy)
		phy_power_on(dsi2->dcphy);

	dsi2->phy_enabled = true;
}

static void mipi_dcphy_power_off(struct dw_mipi_dsi2 *dsi2)
{
	if (!dsi2->phy_enabled)
		return;

	if (dsi2->dcphy)
		phy_power_off(dsi2->dcphy);

	dsi2->phy_enabled = false;
}

static void dw_mipi_dsi2_set_vid_mode(struct dw_mipi_dsi2 *dsi2)
{
	u32 val = 0, mode;
	int ret;

	if (dsi2->mode_flags & MIPI_DSI_MODE_VIDEO_HFP)
		val |= BLK_HFP_HS_EN;

	if (dsi2->mode_flags & MIPI_DSI_MODE_VIDEO_HBP)
		val |= BLK_HBP_HS_EN;

	if (dsi2->mode_flags & MIPI_DSI_MODE_VIDEO_HSA)
		val |= BLK_HSA_HS_EN;

	if (dsi2->mode_flags & MIPI_DSI_MODE_VIDEO_BURST)
		val |= VID_MODE_TYPE_BURST;
	else if (dsi2->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)
		val |= VID_MODE_TYPE_NON_BURST_SYNC_PULSES;
	else
		val |= VID_MODE_TYPE_NON_BURST_SYNC_EVENTS;

	regmap_write(dsi2->regmap, DSI2_DSI_VID_TX_CFG, val);

	regmap_write(dsi2->regmap, DSI2_MODE_CTRL, VIDEO_MODE);
	ret = regmap_read_poll_timeout(dsi2->regmap, DSI2_MODE_STATUS,
				       mode, mode & VIDEO_MODE,
				       1000, MODE_STATUS_TIMEOUT_US);
	if (ret < 0)
		dev_err(dsi2->dev, "failed to enter video mode\n");
}

static void dw_mipi_dsi2_set_data_stream_mode(struct dw_mipi_dsi2 *dsi2)
{
	u32 mode;
	int ret;

	regmap_write(dsi2->regmap, DSI2_MODE_CTRL, DATA_STREAM_MODE);
	ret = regmap_read_poll_timeout(dsi2->regmap, DSI2_MODE_STATUS,
				       mode, mode & DATA_STREAM_MODE,
				       1000, MODE_STATUS_TIMEOUT_US);
	if (ret < 0)
		dev_err(dsi2->dev, "failed to enter data stream mode\n");
}

static void dw_mipi_dsi2_set_cmd_mode(struct dw_mipi_dsi2 *dsi2)
{
	u32 mode;
	int ret;

	regmap_write(dsi2->regmap, DSI2_MODE_CTRL, COMMAND_MODE);
	ret = regmap_read_poll_timeout(dsi2->regmap, DSI2_MODE_STATUS,
				       mode, mode & COMMAND_MODE,
				       1000, MODE_STATUS_TIMEOUT_US);
	if (ret < 0)
		dev_err(dsi2->dev, "failed to enter command mode\n");
}

static void dw_mipi_dsi2_disable(struct dw_mipi_dsi2 *dsi2)
{
	if (!dsi2->enabled)
		goto out;

	regmap_write(dsi2->regmap, DSI2_IPI_PIX_PKT_CFG, 0);
	dw_mipi_dsi2_set_cmd_mode(dsi2);

out:
	if (dsi2->slave)
		dw_mipi_dsi2_disable(dsi2->slave);
}

static void dw_mipi_dsi2_post_disable(struct dw_mipi_dsi2 *dsi2)
{
	if (!dsi2->enabled)
		goto out;

	dw_mipi_dsi2_irq_enable(dsi2, 0);
	regmap_write(dsi2->regmap, DSI2_PWR_UP, RESET);
	mipi_dcphy_power_off(dsi2);
	pm_runtime_put(dsi2->dev);

out:
	if (dsi2->slave)
		dw_mipi_dsi2_post_disable(dsi2->slave);
}

static struct drm_crtc *dw_mipi_dsi2_get_new_crtc(struct dw_mipi_dsi2 *dsi2,
						  struct drm_atomic_state *state)
{
	struct drm_encoder *encoder = &dsi2->encoder;
	struct drm_connector *connector;
	struct drm_connector_state *conn_state;

	connector = drm_atomic_get_new_connector_for_encoder(state, encoder);
	if (!connector)
		return NULL;

	conn_state = drm_atomic_get_new_connector_state(state, connector);
	if (!conn_state)
		return NULL;

	return conn_state->crtc;
}

static void dw_mipi_dsi2_encoder_atomic_disable(struct drm_encoder *encoder,
						struct drm_atomic_state *old_state)
{
	struct dw_mipi_dsi2 *dsi2 = encoder_to_dsi2(encoder);
	struct rockchip_crtc_state *s = to_rockchip_crtc_state(encoder->crtc->state);
	struct drm_crtc *new_crtc;
	struct drm_crtc_state *new_crtc_state = NULL;

	new_crtc = dw_mipi_dsi2_get_new_crtc(dsi2, old_state);
	if (new_crtc)
		new_crtc_state = drm_atomic_get_new_crtc_state(old_state, new_crtc);

	if (new_crtc_state && new_crtc_state->self_refresh_active)
		dev_dbg(dsi2->dev, "%s:%d: psr entry\n", __func__, __LINE__);

	if (!new_crtc_state || !new_crtc_state->self_refresh_active) {
		if (dsi2->panel)
			drm_panel_disable(dsi2->panel);
	}

	if (dw_mipi_dsi2_is_cmd_mode(dsi2))
		rockchip_drm_crtc_standby(encoder->crtc, 1);

	dw_mipi_dsi2_disable(dsi2);

	if (dw_mipi_dsi2_is_cmd_mode(dsi2))
		rockchip_drm_crtc_standby(encoder->crtc, 0);

	if (!new_crtc_state || !new_crtc_state->self_refresh_active) {
		if (dsi2->panel)
			drm_panel_unprepare(dsi2->panel);
	}

	dw_mipi_dsi2_post_disable(dsi2);

	dsi2->enabled = false;
	if (dsi2->slave)
		dsi2->slave->enabled = false;

	if (!encoder->crtc->state->active_changed)
		return;

	if (dsi2->slave)
		s->output_if &= ~(VOP_OUTPUT_IF_MIPI1 | VOP_OUTPUT_IF_MIPI0);
	else
		s->output_if &= ~(dsi2->id ? VOP_OUTPUT_IF_MIPI1 : VOP_OUTPUT_IF_MIPI0);
}

static void dw_mipi_dsi2_get_lane_rate(struct dw_mipi_dsi2 *dsi2)
{
	struct device *dev = dsi2->dev;
	const struct drm_display_mode *mode = &dsi2->mode;
	u64 max_lane_rate;
	u64 lane_rate, target_pclk;
	u32 value;
	int bpp, lanes;
	u64 tmp;

	max_lane_rate = (dsi2->c_option) ?
			 dsi2->pdata->cphy_max_symbol_rate_per_lane :
			 dsi2->pdata->dphy_max_bit_rate_per_lane;

	lanes = (dsi2->slave || dsi2->master) ? dsi2->lanes * 2 : dsi2->lanes;
	bpp = mipi_dsi_pixel_format_to_bpp(dsi2->format);
	if (bpp < 0)
		bpp = 24;

	/*
	 * optional override of the desired bandwidth
	 * High-Speed mode: Differential and terminated: 80Mbps ~ 4500 Mbps.
	 */
	if (!of_property_read_u32(dev->of_node, "rockchip,lane-rate", &value)) {
		if (value >= 80000 && value <= 4500000)
			lane_rate = value * MSEC_PER_SEC;
		else if (value >= 80 && value <= 4500)
			lane_rate = value * USEC_PER_SEC;
		else
			lane_rate = 80 * USEC_PER_SEC;
	} else {
		tmp = (u64)mode->crtc_clock * 1000 * bpp;
		do_div(tmp, lanes);

		/*
		 * Multiple bits are encoded into each symbol epoch,
		 * the data rate is ~2.28x the symbol rate.
		 */
		if (dsi2->c_option)
			tmp = DIV_ROUND_CLOSEST_ULL(tmp * 100, 228);

		/* set BW a little larger only in video burst mode in
		 * consideration of the protocol overhead and HS mode
		 * switching to BLLP mode, take 1 / 0.9, since Mbps must
		 * big than bandwidth of RGB
		 */
		if (dsi2->mode_flags & MIPI_DSI_MODE_VIDEO_BURST) {
			tmp *= 10;
			do_div(tmp, 9);
		}

		if (tmp > max_lane_rate)
			lane_rate = max_lane_rate;
		else
			lane_rate = tmp;
	}

	target_pclk = DIV_ROUND_CLOSEST_ULL(lane_rate * lanes, bpp);
	phy_mipi_dphy_get_default_config(target_pclk, bpp, lanes,
					 &dsi2->phy_opts.mipi_dphy);
	if (dsi2->slave)
		phy_mipi_dphy_get_default_config(target_pclk, bpp, lanes,
						 &dsi2->slave->phy_opts.mipi_dphy);
}

static void dw_mipi_dsi2_set_lane_rate(struct dw_mipi_dsi2 *dsi2)
{
	unsigned long hs_clk_rate;

	if (dsi2->dcphy)
		if (!dsi2->c_option)
			phy_set_mode(dsi2->dcphy, PHY_MODE_MIPI_DPHY);

	phy_configure(dsi2->dcphy, &dsi2->phy_opts);
	hs_clk_rate = dsi2->phy_opts.mipi_dphy.hs_clk_rate;
	dsi2->lane_hs_rate = DIV_ROUND_UP(hs_clk_rate, MSEC_PER_SEC);
}

static void dw_mipi_dsi2_host_softrst(struct dw_mipi_dsi2 *dsi2)
{
	if (dsi2->apb_rst) {
		reset_control_assert(dsi2->apb_rst);
		usleep_range(10, 20);
		reset_control_deassert(dsi2->apb_rst);
	}

	regmap_write(dsi2->regmap, DSI2_SOFT_RESET, 0x0);
	udelay(100);
	regmap_write(dsi2->regmap, DSI2_SOFT_RESET,
		     SYS_RSTN | PHY_RSTN | IPI_RSTN);

}

static void dw_mipi_dsi2_phy_mode_cfg(struct dw_mipi_dsi2 *dsi2)
{
	u32 val = 0;

	/* PPI width is fixed to 16 bits in DCPHY */
	val |= PPI_WIDTH(PPI_WIDTH_16_BITS) | PHY_LANES(dsi2->lanes);
	val |= PHY_TYPE(dsi2->c_option ? CPHY : DPHY);
	regmap_write(dsi2->regmap, DSI2_PHY_MODE_CFG, val);
}

static void dw_mipi_dsi2_phy_clk_mode_cfg(struct dw_mipi_dsi2 *dsi2)
{
	u32 sys_clk, esc_clk_div;
	u32 val = 0;

	/*
	 * clk_type should be NON_CONTINUOUS_CLK before
	 * initial deskew calibration be sent.
	 */
	val |= NON_CONTINUOUS_CLK;

	/* The maximum value of the escape clock frequency is 20MHz */
	sys_clk = clk_get_rate(dsi2->sys_clk) / USEC_PER_SEC;
	esc_clk_div = DIV_ROUND_UP(sys_clk, 20 * 2);
	val |= PHY_LPTX_CLK_DIV(esc_clk_div);

	regmap_write(dsi2->regmap, DSI2_PHY_CLK_CFG, val);
}

static void dw_mipi_dsi2_phy_ratio_cfg(struct dw_mipi_dsi2 *dsi2)
{
	u64 sys_clk = clk_get_rate(dsi2->sys_clk);
	u64 ipi_clk, phy_hsclk;
	u64 tmp;

	/*
	 * in DPHY mode, the phy_hstx_clk is exactly 1/16 the Lane high-speed
	 * data rate; In CPHY mode, the phy_hstx_clk is exactly 1/7 the trio
	 * high speed symbol rate.
	 */
	if (dsi2->c_option)
		phy_hsclk = DIV_ROUND_CLOSEST_ULL(dsi2->lane_hs_rate * MSEC_PER_SEC, 7);
	else
		phy_hsclk = DIV_ROUND_CLOSEST_ULL(dsi2->lane_hs_rate * MSEC_PER_SEC, 16);

	/* IPI_RATIO_MAN_CFG = PHY_HSTX_CLK / IPI_CLK */
	ipi_clk = dsi2->mipi_pixel_rate;
	if (!sys_clk || !ipi_clk)
		return;

	tmp = DIV_ROUND_CLOSEST_ULL(phy_hsclk << 16, ipi_clk);
	regmap_write(dsi2->regmap, DSI2_PHY_IPI_RATIO_MAN_CFG,
		     PHY_IPI_RATIO(tmp));

	/*
	 * SYS_RATIO_MAN_CFG = MIPI_DCPHY_HSCLK_Freq / MIPI_DCPHY_HSCLK_Freq
	 */
	tmp = DIV_ROUND_CLOSEST_ULL(phy_hsclk << 16, sys_clk);
	regmap_write(dsi2->regmap, DSI2_PHY_SYS_RATIO_MAN_CFG,
		     PHY_SYS_RATIO(tmp));
}

static void dw_mipi_dsi2_lp2hs_or_hs2lp_cfg(struct dw_mipi_dsi2 *dsi2)
{
	struct phy_configure_opts_mipi_dphy *cfg = &dsi2->phy_opts.mipi_dphy;
	unsigned long long tmp, ui;
	unsigned long long hstx_clk;

	hstx_clk = DIV_ROUND_CLOSEST_ULL(dsi2->lane_hs_rate * MSEC_PER_SEC, 16);

	ui = ALIGN(PSEC_PER_SEC, hstx_clk);
	do_div(ui, hstx_clk);

	/* PHY_LP2HS_TIME = (TLPX + THS-PREPARE + THS-ZERO) / Tphy_hstx_clk */
	tmp = cfg->lpx + cfg->hs_prepare + cfg->hs_zero;
	tmp = DIV_ROUND_CLOSEST_ULL(tmp << 16, ui);
	regmap_write(dsi2->regmap, DSI2_PHY_LP2HS_MAN_CFG, PHY_LP2HS_TIME(tmp));

	/* PHY_HS2LP_TIME = (THS-TRAIL + THS-EXIT) / Tphy_hstx_clk */
	tmp = cfg->hs_trail + cfg->hs_exit;
	tmp = DIV_ROUND_CLOSEST_ULL(tmp << 16, ui);
	regmap_write(dsi2->regmap, DSI2_PHY_HS2LP_MAN_CFG, PHY_HS2LP_TIME(tmp));
}

static void dw_mipi_dsi2_phy_init(struct dw_mipi_dsi2 *dsi2)
{
	dw_mipi_dsi2_phy_mode_cfg(dsi2);
	dw_mipi_dsi2_phy_clk_mode_cfg(dsi2);

	if (dsi2->auto_calc_mode)
		return;

	dw_mipi_dsi2_phy_ratio_cfg(dsi2);
	dw_mipi_dsi2_lp2hs_or_hs2lp_cfg(dsi2);

	/* phy configuration 8 - 10 */
}

static void dw_mipi_dsi2_tx_option_set(struct dw_mipi_dsi2 *dsi2)
{
	u32 val;

	val = BTA_EN | EOTP_TX_EN;

	if (dsi2->mode_flags & MIPI_DSI_MODE_EOT_PACKET)
		val &= ~EOTP_TX_EN;

	regmap_write(dsi2->regmap, DSI2_DSI_GENERAL_CFG, val);
	regmap_write(dsi2->regmap, DSI2_DSI_VCID_CFG, TX_VCID(dsi2->channel));

	if (dsi2->scrambling_en)
		regmap_write(dsi2->regmap, DSI2_DSI_SCRAMBLING_CFG,
			     SCRAMBLING_EN);
}

static void dw_mipi_dsi2_ipi_color_coding_cfg(struct dw_mipi_dsi2 *dsi2)
{
	u32 val, color_depth;

	switch (dsi2->format) {
	case MIPI_DSI_FMT_RGB666:
	case MIPI_DSI_FMT_RGB666_PACKED:
		color_depth = IPI_DEPTH_6_BITS;
		break;
	case MIPI_DSI_FMT_RGB565:
		color_depth = IPI_DEPTH_5_6_5_BITS;
		break;
	case MIPI_DSI_FMT_RGB888:
	default:
		color_depth = IPI_DEPTH_8_BITS;
		break;
	}

	val = IPI_DEPTH(color_depth) |
	      IPI_FORMAT(dsi2->dsc_enable ? IPI_FORMAT_DSC : IPI_FORMAT_RGB);
	regmap_write(dsi2->regmap, DSI2_IPI_COLOR_MAN_CFG, val);
	grf_field_write(dsi2, IPI_COLOR_DEPTH, color_depth);

	if (dsi2->dsc_enable)
		grf_field_write(dsi2, IPI_FORMAT, IPI_FORMAT_DSC);
}

static void dw_mipi_dsi2_ipi_set(struct dw_mipi_dsi2 *dsi2)
{
	struct drm_display_mode *mode = &dsi2->mode;
	u32 hline, hsa, hbp, hact;
	u64 hline_time, hsa_time, hbp_time, hact_time, tmp;
	u64 pixel_clk, phy_hs_clk;
	u32 vact, vsa, vfp, vbp;
	u16 val;

	if (dsi2->slave || dsi2->master)
		val = mode->hdisplay / 2;
	else
		val = mode->hdisplay;

	regmap_write(dsi2->regmap, DSI2_IPI_PIX_PKT_CFG, MAX_PIX_PKT(val));

	dw_mipi_dsi2_ipi_color_coding_cfg(dsi2);

	if (dsi2->auto_calc_mode)
		return;

	/*
	 * if the controller is intended to operate in data stream mode,
	 * no more steps are required.
	 */
	if (dw_mipi_dsi2_is_cmd_mode(dsi2))
		return;

	vact = mode->vdisplay;
	vsa = mode->vsync_end - mode->vsync_start;
	vfp = mode->vsync_start - mode->vdisplay;
	vbp = mode->vtotal - mode->vsync_end;
	hact = mode->hdisplay;
	hsa = mode->hsync_end - mode->hsync_start;
	hbp = mode->htotal - mode->hsync_end;
	hline = mode->htotal;

	pixel_clk = mode->crtc_clock * MSEC_PER_SEC;

	if (dsi2->c_option)
		phy_hs_clk = DIV_ROUND_CLOSEST_ULL(dsi2->lane_hs_rate * MSEC_PER_SEC, 7);
	else
		phy_hs_clk = DIV_ROUND_CLOSEST_ULL(dsi2->lane_hs_rate * MSEC_PER_SEC, 16);

	tmp = hsa * phy_hs_clk;
	hsa_time = DIV_ROUND_CLOSEST_ULL(tmp << 16, pixel_clk);
	regmap_write(dsi2->regmap, DSI2_IPI_VID_HSA_MAN_CFG,
		     VID_HSA_TIME(hsa_time));

	tmp = hbp * phy_hs_clk;
	hbp_time = DIV_ROUND_CLOSEST_ULL(tmp << 16, pixel_clk);
	regmap_write(dsi2->regmap, DSI2_IPI_VID_HBP_MAN_CFG,
		     VID_HBP_TIME(hbp_time));

	tmp = hact * phy_hs_clk;
	hact_time = DIV_ROUND_CLOSEST_ULL(tmp << 16, pixel_clk);
	regmap_write(dsi2->regmap, DSI2_IPI_VID_HACT_MAN_CFG,
		     VID_HACT_TIME(hact_time));

	tmp = hline * phy_hs_clk;
	hline_time = DIV_ROUND_CLOSEST_ULL(tmp << 16, pixel_clk);
	regmap_write(dsi2->regmap, DSI2_IPI_VID_HLINE_MAN_CFG,
		     VID_HLINE_TIME(hline_time));

	regmap_write(dsi2->regmap, DSI2_IPI_VID_VSA_MAN_CFG,
		     VID_VSA_LINES(vsa));
	regmap_write(dsi2->regmap, DSI2_IPI_VID_VBP_MAN_CFG,
		     VID_VBP_LINES(vbp));
	regmap_write(dsi2->regmap, DSI2_IPI_VID_VACT_MAN_CFG,
		     VID_VACT_LINES(vact));
	regmap_write(dsi2->regmap, DSI2_IPI_VID_VFP_MAN_CFG,
		     VID_VFP_LINES(vfp));
}

static void
dw_mipi_dsi2_work_mode(struct dw_mipi_dsi2 *dsi2, u32 mode)
{
	/*
	 * select controller work in Manual mode
	 * Manual: MANUAL_MODE_EN
	 * Automatic: 0
	 */
	regmap_write(dsi2->regmap, MANUAL_MODE_CFG, mode);
}

static void dw_mipi_dsi2_pre_enable(struct dw_mipi_dsi2 *dsi2)
{
	if (dsi2->enabled)
		goto out;

	pm_runtime_get_sync(dsi2->dev);

	dw_mipi_dsi2_host_softrst(dsi2);
	regmap_write(dsi2->regmap, DSI2_PWR_UP, RESET);

	/* there may be some timeout registers may be configured if desired */

	dw_mipi_dsi2_work_mode(dsi2, dsi2->auto_calc_mode ? 0 : MANUAL_MODE_EN);
	dw_mipi_dsi2_phy_init(dsi2);
	dw_mipi_dsi2_tx_option_set(dsi2);
	dw_mipi_dsi2_irq_enable(dsi2, 1);
	mipi_dcphy_power_on(dsi2);

	regmap_write(dsi2->regmap, DSI2_PWR_UP, POWER_UP);
	dw_mipi_dsi2_set_cmd_mode(dsi2);

out:
	if (dsi2->slave)
		dw_mipi_dsi2_pre_enable(dsi2->slave);
}

static void dw_mipi_dsi2_clk_management(struct dw_mipi_dsi2 *dsi2)
{
	u32 clk_type;

	/*
	 * initial deskew calibration is send after phy_power_on,
	 * then we can configure clk_type.
	 */
	if (dsi2->mode_flags & MIPI_DSI_CLOCK_NON_CONTINUOUS)
		clk_type = NON_CONTINUOUS_CLK;
	else
		clk_type = CONTIUOUS_CLK;

	regmap_update_bits(dsi2->regmap, DSI2_PHY_CLK_CFG,
			   CLK_TYPE_MASK, clk_type);
}

static void dw_mipi_dsi2_enable(struct dw_mipi_dsi2 *dsi2)
{
	u32 mode;
	int ret;

	if (dsi2->enabled)
		goto out;

	dw_mipi_dsi2_clk_management(dsi2);
	dw_mipi_dsi2_ipi_set(dsi2);

	if (dsi2->auto_calc_mode) {
		regmap_write(dsi2->regmap, DSI2_MODE_CTRL, AUTOCALC_MODE);
		ret = regmap_read_poll_timeout(dsi2->regmap, DSI2_MODE_STATUS,
					       mode, mode == IDLE_MODE,
					       1000, MODE_STATUS_TIMEOUT_US);
		if (ret < 0)
			dev_err(dsi2->dev, "auto calculation training failed\n");
	}

	if (dsi2->mode_flags & MIPI_DSI_MODE_VIDEO)
		dw_mipi_dsi2_set_vid_mode(dsi2);
	else
		dw_mipi_dsi2_set_data_stream_mode(dsi2);

out:
	if (dsi2->slave)
		dw_mipi_dsi2_enable(dsi2->slave);
}

static void dw_mipi_dsi2_get_mipi_pixel_clk(struct dw_mipi_dsi2 *dsi2,
					    struct rockchip_crtc_state *s)
{
	struct drm_display_mode *mode = &dsi2->mode;
	u8 k = dsi2->slave ? 2 : 1;

	/* 1.When MIPI works in uncompressed mode:
	 * (Video Timing Pixel Rate)/(4)=(MIPI Pixel ClockxK)=(dclk_out×K)=dclk_core
	 * 2.When MIPI works in compressed mode:
	 * MIPI Pixel Clock = cds_clk / 2
	 * MIPI is configured as double channel display mode, K=2, otherwise K=1.
	 */
	if (dsi2->dsc_enable) {
		dsi2->mipi_pixel_rate = s->dsc_cds_clk_rate / 2;
		if (dsi2->slave)
			dsi2->slave->mipi_pixel_rate = dsi2->mipi_pixel_rate;

		return;
	}

	dsi2->mipi_pixel_rate = (mode->crtc_clock * MSEC_PER_SEC) / (4 * k);
	if (dsi2->slave)
		dsi2->slave->mipi_pixel_rate = dsi2->mipi_pixel_rate;
}

static int dw_mipi_dsi2_encoder_mode_set(struct dw_mipi_dsi2 *dsi2,
					 struct drm_atomic_state *state)
{
	struct drm_encoder *encoder = &dsi2->encoder;
	struct drm_connector *connector;
	struct drm_connector_state *conn_state;
	struct drm_crtc_state *crtc_state;
	struct rockchip_crtc_state *vcstate;
	const struct drm_display_mode *adjusted_mode;
	struct drm_display_mode *mode = &dsi2->mode;

	connector = drm_atomic_get_new_connector_for_encoder(state, encoder);
	if (!connector)
		return -ENODEV;

	conn_state = drm_atomic_get_new_connector_state(state, connector);
	if (!conn_state)
		return -ENODEV;

	crtc_state = drm_atomic_get_new_crtc_state(state, conn_state->crtc);
	if (!crtc_state) {
		dev_err(dsi2->dev, "failed to get crtc state\n");
		return -ENODEV;
	}

	vcstate = to_rockchip_crtc_state(crtc_state);
	adjusted_mode = &crtc_state->adjusted_mode;
	drm_mode_copy(mode, adjusted_mode);

	if (dsi2->dual_connector_split)
		drm_mode_convert_to_origin_mode(mode);

	if (dsi2->slave)
		drm_mode_copy(&dsi2->slave->mode, mode);

	dw_mipi_dsi2_get_mipi_pixel_clk(dsi2, vcstate);

	return 0;
}

static void dw_mipi_dsi2_encoder_atomic_enable(struct drm_encoder *encoder,
					       struct drm_atomic_state *state)
{
	struct dw_mipi_dsi2 *dsi2 = encoder_to_dsi2(encoder);
	struct drm_crtc *crtc;
	struct drm_crtc_state *old_crtc_state = NULL;
	int ret;

	crtc = dw_mipi_dsi2_get_new_crtc(dsi2, state);
	if (crtc)
		old_crtc_state = drm_atomic_get_old_crtc_state(state, crtc);

	if (old_crtc_state && old_crtc_state->self_refresh_active) {
		rockchip_drm_crtc_standby(encoder->crtc, 1);
		dev_dbg(dsi2->dev, "%s:%d: psr exit\n", __func__, __LINE__);
	}

	ret = dw_mipi_dsi2_encoder_mode_set(dsi2, state);
	if (ret) {
		dev_err(dsi2->dev, "failed to set dsi2 mode\n");
		return;
	}

	dw_mipi_dsi2_get_lane_rate(dsi2);

	if (dsi2->dcphy)
		dw_mipi_dsi2_set_lane_rate(dsi2);

	if (dsi2->slave && dsi2->slave->dcphy)
		dw_mipi_dsi2_set_lane_rate(dsi2->slave);

	dw_mipi_dsi2_pre_enable(dsi2);

	if (!(old_crtc_state && old_crtc_state->self_refresh_active)) {
		if (dsi2->panel)
			drm_panel_prepare(dsi2->panel);
	}

	dw_mipi_dsi2_enable(dsi2);

	if (old_crtc_state && old_crtc_state->self_refresh_active)
		rockchip_drm_crtc_standby(encoder->crtc, 0);

	dsi2->enabled = true;
	if (dsi2->slave)
		dsi2->slave->enabled = true;

	if (!(old_crtc_state && old_crtc_state->self_refresh_active)) {
		if (dsi2->panel)
			drm_panel_enable(dsi2->panel);
	}

	DRM_DEV_INFO(dsi2->dev, "final DSI-Link bandwidth: %u x %d %s\n",
		     dsi2->lane_hs_rate,
		     dsi2->slave ? dsi2->lanes * 2 : dsi2->lanes,
		     dsi2->c_option ? "Ksps" : "Kbps");
}

static int
dw_mipi_dsi2_encoder_atomic_check(struct drm_encoder *encoder,
				  struct drm_crtc_state *crtc_state,
				  struct drm_connector_state *conn_state)
{

	struct rockchip_crtc_state *s = to_rockchip_crtc_state(crtc_state);
	struct dw_mipi_dsi2 *dsi2 = encoder_to_dsi2(encoder);
	struct drm_connector *connector = conn_state->connector;
	struct drm_display_info *info = &connector->display_info;

	switch (dsi2->format) {
	case MIPI_DSI_FMT_RGB888:
		s->output_mode = ROCKCHIP_OUT_MODE_P888;
		break;
	case MIPI_DSI_FMT_RGB666:
		s->output_mode = ROCKCHIP_OUT_MODE_P666;
		break;
	case MIPI_DSI_FMT_RGB565:
		s->output_mode = ROCKCHIP_OUT_MODE_P565;
		break;
	default:
		WARN_ON(1);
		return -EINVAL;
	}

	if (info->num_bus_formats)
		s->bus_format = info->bus_formats[0];
	else
		s->bus_format = MEDIA_BUS_FMT_RGB888_1X24;

	s->output_type = DRM_MODE_CONNECTOR_DSI;
	s->output_if |= dsi2->id ? VOP_OUTPUT_IF_MIPI1 : VOP_OUTPUT_IF_MIPI0;
	s->bus_flags = info->bus_flags;

	s->tv_state = &conn_state->tv;
	s->color_space = V4L2_COLORSPACE_DEFAULT;

	if (dw_mipi_dsi2_is_cmd_mode(dsi2)) {
		s->output_flags |= ROCKCHIP_OUTPUT_MIPI_DS_MODE;
		s->soft_te = dsi2->te_gpio ? true : false;
		s->hold_mode = dsi2->disable_hold_mode ? false : true;
	}

	if (dsi2->slave) {
		s->output_flags |= ROCKCHIP_OUTPUT_DUAL_CHANNEL_LEFT_RIGHT_MODE;
		if (dsi2->data_swap)
			s->output_flags |= ROCKCHIP_OUTPUT_DATA_SWAP;

		s->output_if |= VOP_OUTPUT_IF_MIPI1;
	}

	if (dsi2->dual_connector_split) {
		s->output_flags |= ROCKCHIP_OUTPUT_DUAL_CONNECTOR_SPLIT_MODE;

		if (dsi2->left_display)
			s->output_if_left_panel |= dsi2->id ?
						   VOP_OUTPUT_IF_MIPI1 :
						   VOP_OUTPUT_IF_MIPI0;
	}

	if (dsi2->dsc_enable) {
		s->dsc_enable = 1;
		s->dsc_sink_cap.version_major = dsi2->version_major;
		s->dsc_sink_cap.version_minor = dsi2->version_minor;
		s->dsc_sink_cap.slice_width = dsi2->slice_width;
		s->dsc_sink_cap.slice_height = dsi2->slice_height;
		/* only can support rgb888 panel now */
		s->dsc_sink_cap.target_bits_per_pixel_x16 = 8 << 4;
		s->dsc_sink_cap.native_420 = 0;

		memcpy(&s->pps, dsi2->pps, sizeof(struct drm_dsc_picture_parameter_set));
	}

	return 0;
}

static void dw_mipi_dsi2_loader_protect(struct dw_mipi_dsi2 *dsi2, bool on)
{
	if (on) {
		pm_runtime_get_sync(dsi2->dev);
		phy_init(dsi2->dcphy);
		dsi2->phy_enabled = true;
		if (dsi2->dcphy)
			dsi2->dcphy->power_count++;

		dsi2->enabled = true;
	} else {
		pm_runtime_put(dsi2->dev);
		phy_exit(dsi2->dcphy);
		dsi2->phy_enabled = false;
		if (dsi2->dcphy)
			dsi2->dcphy->power_count--;

		dsi2->enabled = false;
	}

	if (dsi2->slave)
		dw_mipi_dsi2_loader_protect(dsi2->slave, on);
}

static int dw_mipi_dsi2_encoder_loader_protect(struct drm_encoder *encoder,
					      bool on)
{
	struct dw_mipi_dsi2 *dsi2 = encoder_to_dsi2(encoder);

	if (dsi2->panel)
		panel_simple_loader_protect(dsi2->panel);

	dw_mipi_dsi2_loader_protect(dsi2, on);

	return 0;
}

static const struct drm_encoder_helper_funcs
dw_mipi_dsi2_encoder_helper_funcs = {
	.atomic_enable = dw_mipi_dsi2_encoder_atomic_enable,
	.atomic_disable = dw_mipi_dsi2_encoder_atomic_disable,
	.atomic_check = dw_mipi_dsi2_encoder_atomic_check,
};

static int dw_mipi_dsi2_connector_get_modes(struct drm_connector *connector)
{
	struct dw_mipi_dsi2 *dsi2 = con_to_dsi2(connector);

	if (dsi2->bridge && (dsi2->bridge->ops & DRM_BRIDGE_OP_MODES))
		return drm_bridge_get_modes(dsi2->bridge, connector);

	if (dsi2->panel)
		return drm_panel_get_modes(dsi2->panel, connector);

	return -EINVAL;
}

static enum drm_mode_status
dw_mipi_dsi2_connector_mode_valid(struct drm_connector *connector,
				  struct drm_display_mode *mode)
{
	struct dw_mipi_dsi2 *dsi2 = con_to_dsi2(connector);
	struct videomode vm;
	u8 min_pixels = dsi2->slave ? 8 : 4;

	drm_display_mode_to_videomode(mode, &vm);

	if (vm.vactive > 16383)
		return MODE_VIRTUAL_Y;

	if (vm.vsync_len > 1023)
		return MODE_VSYNC_WIDE;

	if (vm.vback_porch > 1023 || vm.vfront_porch > 1023)
		return MODE_VBLANK_WIDE;

	/*
	 * the minimum region size (HSA,HBP,HACT,HFP) is 4 pixels
	 * which is the ip known issues and limitations.
	 */
	if (!(vm.hsync_len < min_pixels || vm.hback_porch < min_pixels ||
	    vm.hfront_porch < min_pixels || vm.hactive < min_pixels))
		return MODE_OK;

	if (vm.hsync_len < min_pixels)
		vm.hsync_len = min_pixels;

	if (vm.hback_porch < min_pixels)
		vm.hback_porch = min_pixels;

	if (vm.hfront_porch < min_pixels)
		vm.hfront_porch = min_pixels;

	if (vm.hactive < min_pixels)
		vm.hactive = min_pixels;

	drm_display_mode_from_videomode(&vm, mode);

	return MODE_OK;
}

static int dw_mipi_dsi2_connector_atomic_check(struct drm_connector *connector,
					       struct drm_atomic_state *state)
{
	struct dw_mipi_dsi2 *dsi2 = con_to_dsi2(connector);
	struct drm_connector_state *conn_state;
	struct drm_crtc_state *crtc_state;

	conn_state = drm_atomic_get_new_connector_state(state, connector);
	if (WARN_ON(!conn_state))
		return -ENODEV;

	conn_state->self_refresh_aware = dsi2->support_psr && dw_mipi_dsi2_is_cmd_mode(dsi2);

	if (!conn_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, conn_state->crtc);
	if (!crtc_state)
		return 0;

	if (crtc_state->self_refresh_active && !dw_mipi_dsi2_is_cmd_mode(dsi2))
		return -EINVAL;

	return 0;
}

static struct drm_connector_helper_funcs dw_mipi_dsi2_connector_helper_funcs = {
	.get_modes = dw_mipi_dsi2_connector_get_modes,
	.mode_valid = dw_mipi_dsi2_connector_mode_valid,
	.atomic_check = dw_mipi_dsi2_connector_atomic_check,
};

static enum drm_connector_status
dw_mipi_dsi2_connector_detect(struct drm_connector *connector, bool force)
{
	struct dw_mipi_dsi2 *dsi2 = con_to_dsi2(connector);

	if (dsi2->bridge && (dsi2->bridge->ops & DRM_BRIDGE_OP_DETECT))
		return drm_bridge_detect(dsi2->bridge);

	return connector_status_connected;
}

static void dw_mipi_dsi2_drm_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static int
dw_mipi_dsi2_atomic_connector_get_property(struct drm_connector *connector,
					   const struct drm_connector_state *state,
					   struct drm_property *property,
					   uint64_t *val)
{
	struct rockchip_drm_private *private = connector->dev->dev_private;
	struct dw_mipi_dsi2 *dsi2 = con_to_dsi2(connector);

	if (property == private->split_area_prop) {
		switch (dsi2->split_area) {
		case 1:
			*val = ROCKCHIP_DRM_SPLIT_LEFT_SIDE;
			break;
		case 2:
			*val = ROCKCHIP_DRM_SPLIT_RIGHT_SIDE;
			break;
		default:
			*val = ROCKCHIP_DRM_SPLIT_UNSET;
			break;
		}
	}

	return 0;
}

static const struct drm_connector_funcs dw_mipi_dsi2_atomic_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.detect = dw_mipi_dsi2_connector_detect,
	.destroy = dw_mipi_dsi2_drm_connector_destroy,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.atomic_get_property = dw_mipi_dsi2_atomic_connector_get_property,
};

static int dw_mipi_dsi2_dual_channel_probe(struct dw_mipi_dsi2 *dsi2)
{
	struct device_node *np;
	struct platform_device *secondary;

	np = of_parse_phandle(dsi2->dev->of_node, "rockchip,dual-channel", 0);
	if (np) {
		dsi2->data_swap = of_property_read_bool(dsi2->dev->of_node,
						       "rockchip,data-swap");
		secondary = of_find_device_by_node(np);
		dsi2->slave = platform_get_drvdata(secondary);
		of_node_put(np);

		if (!dsi2->slave)
			return -EPROBE_DEFER;

		dsi2->slave->master = dsi2;
		dsi2->lanes /= 2;

		dsi2->slave->auto_calc_mode = dsi2->auto_calc_mode;
		dsi2->slave->lanes = dsi2->lanes;
		dsi2->slave->channel = dsi2->channel;
		dsi2->slave->format = dsi2->format;
		dsi2->slave->mode_flags = dsi2->mode_flags;
		dsi2->slave->support_psr = dsi2->support_psr;
	}

	return 0;
}

static irqreturn_t dw_mipi_dsi2_te_irq_handler(int irq, void *dev_id)
{
	struct dw_mipi_dsi2 *dsi2 = (struct dw_mipi_dsi2 *)dev_id;
	struct drm_encoder *encoder = &dsi2->encoder;

	if (encoder->crtc)
		rockchip_drm_te_handle(encoder->crtc);

	return IRQ_HANDLED;
}
/*
 * 固件里的每个数据块都用 offset + length 描述，表示：
 * - offset：该数据块在 bin 文件中的起始偏移
 * - length：该数据块的字节长度
 */
struct entry {
    u32 offset;
    u32 length;
};
/**
 * @magic: LCD 配置固件魔数，用于判断文件是否是合法的 vis-timing.bin。
 * @vendor: 面板厂商名。
 * @model: 面板型号名。
 * @version: 固件版本字符串。
 * @timing_entry: timing 表所在位置。
 * @init_seq_entry: init sequence 所在位置。
 * @eixt_seq_entry: exit sequence 所在位置。
 * @touchscreen_entry: 触摸屏相关参数所在位置。
 * @vesa_dsc_entry: VESA DSC 配置所在位置。
 * @other_info_entry: 其他扩展信息所在位置。
 * @firmware_size: 固件总大小。
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
/*
 * 从 vis-timing.bin 中提取两类和 DSC 相关的数据：
 * 1. vesa_dsc_entry：用于得到 DSC 开关、版本、slice 等基础参数；
 * 2. init_seq_entry：后面继续从中解析 0x0A PPS 数据包。
 *
 * 之所以两者都要拿，是因为“DSC 基本参数”和“真正下发给面板的 PPS”
 * 分别存放在两个区域里，缺一边都不方便排查。
 */
static int dw_mipi_dsi2_get_dsc_params_from_firmware(const char *fw_path,struct device *dev,
				struct dw_mipi_dsi2 *dsi2,const u8 **init_seq_data,int *init_seq_len)
{
    struct file *fp;
    struct firmware_header *header;
    const u8 *dsc_data;
    loff_t pos = 0;
    int ret;

    /* 1. 打开文件 */
    dev_info(dev, "=========dw_mipi_dsi2_get_dsc_params_from_firmware=========");
    dev_info(dev, "Loading DSC config from: %s\n", fw_path);
    fp = filp_open(fw_path, O_RDONLY, 0);
    if (IS_ERR(fp)) {
        dev_err(dev, "Can't open %s: err=%ld\n", fw_path, PTR_ERR(fp));
        return PTR_ERR(fp);
    }

    /* 2. 分配并读取文件头 */
    header = (struct firmware_header *)devm_kzalloc(dev, sizeof(*header), GFP_KERNEL);
    if (!header) {
        ret = -ENOMEM;
        goto err_close;
    }

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

    dev_info(dev, "firmware magic: %x\n", header->magic);
    dev_info(dev, "firmware version: %.*s, size: %d\n",8,
             header->version, header->firmware_size);

    /*
     * vesa_dsc_entry 至少要能覆盖当前使用的基础字段：
     * - 字节0~4：开关/版本
     * - 字节8~11：slice_width
     * - 字节12~15：slice_height
     * 因此长度小于 16 时，后面的解析结果一定不可靠，直接判错。
     */
    if (header->vesa_dsc_entry.length < 16) {
        dev_err(dev, "Invalid VESA DSC entry length: %d\n", header->vesa_dsc_entry.length);
        ret = -EINVAL;
        goto err_close;
    }

    /* 5. 读取DSC数据 */
    dsc_data = devm_kzalloc(dev, header->vesa_dsc_entry.length, GFP_KERNEL);
    if (!dsc_data) {
        ret = -ENOMEM;
        goto err_close;
    }

    pos = header->vesa_dsc_entry.offset;
    ret = kernel_read(fp, (void *)dsc_data, header->vesa_dsc_entry.length, &pos);
    if (ret < 0) {
        dev_err(dev, "Failed to read VESA DSC data: %d\n", ret);
        goto err_close;
    }

    /*
     * 打印 VESA DSC 条目的原始数据和对齐后的解析结果，
     * 便于直接和 vis-timing.bin 里的 vesa_dsc_entry 逐字节比对。
     */
    dev_info(dev, "[dsi2] ==================== VESA DSC ENTRY BEGIN ====================\n");
    dev_info(dev, "[dsi2] source: %s\n", fw_path);
    dev_info(dev, "[dsi2] entry: vesa_dsc_entry\n");
    dev_info(dev, "[dsi2] offset: 0x%08X\n", header->vesa_dsc_entry.offset);
    dev_info(dev, "[dsi2] length: %u\n", header->vesa_dsc_entry.length);

    /* raw: 每行 16 字节，固定宽度，便于和二进制文件逐行对应 */
    dev_info(dev, "[dsi2] raw:\n");
    print_hex_dump(KERN_INFO, "[dsi2]   ", DUMP_PREFIX_OFFSET,
		   16, 1, dsc_data, header->vesa_dsc_entry.length, false);

    /*
     * 这里解析的是厂商固件里额外保存的一份“简化 DSC 参数表”，
     * 它不是标准 PPS 全量结构，但足够先决定：
     * - 是否启用 DSC
     * - 使用哪个版本
     * - slice 宽高是多少
     */
    dsi2->c_option = dsc_data[0] != 0;
    dsi2->scrambling_en = dsc_data[1] != 0;
    dsi2->dsc_enable = dsc_data[2] != 0;
    dsi2->version_major = dsc_data[3];
    dsi2->version_minor = dsc_data[4];

    /* 字节5-7未使用 */

    /* 读取slice_width (字节8-11, 小端格式) */
    dsi2->slice_width = get_unaligned_le32(&dsc_data[8]);

    /* 读取slice_height (字节12-15, 小端格式) */
    dsi2->slice_height = get_unaligned_le32(&dsc_data[12]);

    /* parsed: 解析后的字段，便于阅读 */
    dev_info(dev, "[dsi2] parsed:\n");
    dev_info(dev, "[dsi2]   c_option      : %d\n", dsi2->c_option);
    dev_info(dev, "[dsi2]   scrambling_en : %d\n", dsi2->scrambling_en);
    dev_info(dev, "[dsi2]   dsc_enable    : %d\n", dsi2->dsc_enable);
    dev_info(dev, "[dsi2]   version       : %d.%d\n",
	     dsi2->version_major, dsi2->version_minor);
    dev_info(dev, "[dsi2]   slice_width   : %u\n", dsi2->slice_width);
    dev_info(dev, "[dsi2]   slice_height  : %u\n", dsi2->slice_height);
    dev_info(dev, "[dsi2] ===================== VESA DSC ENTRY END =====================\n");

	/* 读取 panel-init-sequence，后面继续从中提取 0x0A PPS 数据包。 */
    if (init_seq_data && init_seq_len &&
        header->init_seq_entry.length > 60) {
        *init_seq_data = devm_kzalloc(dev, header->init_seq_entry.length, GFP_KERNEL);
        if (!*init_seq_data) {
            ret = -ENOMEM;
            goto err_close;
        }

        pos = header->init_seq_entry.offset;
        ret = kernel_read(fp, (void *)*init_seq_data,
                         header->init_seq_entry.length, &pos);
        if (ret < 0) {
            dev_err(dev, "Failed to read init sequence: %d\n", ret);
            goto err_close;
        }
        *init_seq_len = header->init_seq_entry.length;
    }

    filp_close(fp, NULL);
    return 0;

err_close:
    filp_close(fp, NULL);
    return ret;
}

static int dw_mipi_dsi2_get_dsc_params_from_sink(struct dw_mipi_dsi2 *dsi2,
						 struct drm_panel *panel,
						 struct drm_bridge *bridge,struct device *dev)
{
	struct drm_dsc_picture_parameter_set *pps = NULL;
	struct device_node *np = NULL;
	struct cmd_header *header;
	const void *data;
	char *d;
	uint8_t *dsc_packed_pps;
	u32 pps_payload_len = 0;
	int len,ret,gpio_value;
	int init_seq_len = 0;
	const u8 *init_seq_data = NULL;
	const char *fw_path = "/lib/firmware/vis-timing.bin";
	u16 pic_width;

	if (!panel && !bridge)
		return -ENODEV;

	if (panel)
		np = panel->dev->of_node;
	else
		np = bridge->of_node;

	/*
	 * 这里保留一个“人工逃生口”：
	 * 如果某份 DSC 参数会把显示链路卡死，调试时可以通过按键/GPIO
	 * 在驱动加载阶段临时跳过 DSC，避免每次都必须改代码或重刷固件。
	 */
	{
		int dsc_det_gpio;
		enum of_gpio_flags flags;
		dev_info(dev, "start detect GPIO for block dsc params in panel bin\n");
		/* 1. 查找 GPIO 配置节点 */
		np = of_find_node_by_name(NULL, "no-dsc-gpio");
		if (!np) {
			dev_warn(dsi2->dev, "No DSC GPIO node found, using fallback\n");
			dsc_det_gpio = 108; /* 默认 GPIO108 */
		} else {
			/* 2. 从节点获取 GPIO 配置 */
			dsc_det_gpio = of_get_named_gpio_flags(np, "nodsc-pin", 0, &flags);
			of_node_put(np); // 减少引用计数

			if (dsc_det_gpio < 0) {
				dev_err(dsi2->dev, "Invalid GPIO in no-dsc-gpio node, using fallback\n");
				dsc_det_gpio = 108;
			}
		}
		/*
		 * flags 只是辅助观察当前引脚属性；
		 * 这里真正决定是否跳过 DSC 的，仍然是实际读出来的电平值。
		 */
		dev_info(dsi2->dev, "Using GPIO%d (flags: 0x%x) for DSC detection\n",dsc_det_gpio, flags);

		/* 3. 请求 GPIO */
		ret = gpio_request(dsc_det_gpio, "dsc_det_gpio");
		if (ret)
			dev_err(dsi2->dev, "Failed to request GPIO %d: %d\n", dsc_det_gpio, ret);

		/* 4. 配置 GPIO 方向 */
		ret = gpio_direction_input(dsc_det_gpio);
		if (ret) {
			dev_err(dsi2->dev, "Failed to set GPIO%d direction: %d\n", dsc_det_gpio, ret);
			goto err_free_gpio;
		}

		/*
		 * 5. 读取 GPIO 值。
		 * 约定：低电平表示“本次启动跳过 DSC”；高电平表示正常按 DSC 流程继续。
		 */
		gpio_value = gpio_get_value(dsc_det_gpio);
		dev_info(dsi2->dev, "GPIO%d state: %s (raw: %d)\n",
					dsc_det_gpio, gpio_value ? "HIGH" : "LOW", gpio_get_value(dsc_det_gpio));

		if (gpio_value == 0) {
			dev_info(dsi2->dev, "DSC detection: GPIO is LOW (button pressed?)\n");
		} else {
			dev_info(dsi2->dev, "DSC detection: GPIO is HIGH (button released)\n");
		}

		err_free_gpio:
    	/* 7. 无论成功失败都把 GPIO 释放掉，避免影响后续重试。 */
    	gpio_free(dsc_det_gpio);
    }
	if (gpio_value == 0){
		/* 用户主动要求跳过 DSC：主从控制器的 dsc_enable 都同步拉低。 */
		dsi2->dsc_enable = false;
		if (dsi2->slave)
			dsi2->slave->dsc_enable = false;
		dev_info(dsi2->dev, "DSC feature will be disabled\n");
		return 0;
	}

	/*
	 * 优先尝试从固件 bin 读取 DSC 参数。
	 * 这样做的好处是：如果量产配置已经固化在 vis-timing.bin 里，
	 * 调试日志就能直接反映“本次启动真正使用的那份配置”。
	 */
	ret = dw_mipi_dsi2_get_dsc_params_from_firmware(fw_path,dsi2->dev, dsi2,
	 							&init_seq_data,&init_seq_len);
    if (ret) {
        /* 如果从固件读取失败，则回退到设备树中的静态参数。 */
        dev_info(dsi2->dev, "Failed to get DSC params from firmware, fallback to DT\n");

        dsi2->c_option = of_property_read_bool(np, "phy-c-option");
        dsi2->scrambling_en = of_property_read_bool(np, "scrambling-enable");
        dsi2->dsc_enable = of_property_read_bool(np, "compressed-data");

        if (dsi2->slave) {
            dsi2->slave->c_option = dsi2->c_option;
            dsi2->slave->scrambling_en = dsi2->scrambling_en;
            dsi2->slave->dsc_enable = dsi2->dsc_enable;
        }
        if (!dsi2->dsc_enable)
            return 0;

        of_property_read_u32(np, "slice-width", &dsi2->slice_width);
        of_property_read_u32(np, "slice-height", &dsi2->slice_height);
        of_property_read_u8(np, "version-major", &dsi2->version_major);
        of_property_read_u8(np, "version-minor", &dsi2->version_minor);
    }
    else if (dsi2->slave) {
        /* 如果成功从固件读取，再把基础 DSC 参数同步给从控制器。 */
        dsi2->slave->c_option = dsi2->c_option;
        dsi2->slave->scrambling_en = dsi2->scrambling_en;
        dsi2->slave->dsc_enable = dsi2->dsc_enable;
    }

	/* DSC 未开启就直接返回，后续也不需要再找 PPS。 */
	if (!dsi2->dsc_enable)
		return 0;

	/*
	 * 优先级说明：
	 * 1. 如果成功从 firmware 里拿到了 init_seq_data，就优先解析 firmware；
	 * 2. 如果 firmware 读取失败，或者虽然读到了 DSC 基本参数但没有拿到
	 *    init_seq_data，则回退到设备树里的 panel-init-sequence。
	 *
	 * 这样做的目的，是保证“最终送给面板的 0x0A PPS 包”总能尽量找到来源，
	 * 便于把 firmware / DT / runtime 三个层面的参数串起来看。
	 */
	if (ret || !init_seq_data) {	/* 下面开始解析设备树里的初始化代码 */
	data = of_get_property(np, "panel-init-sequence", &len);
	if (!data)
		return -EINVAL;

	d = devm_kmemdup(dsi2->dev, data, len, GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	while (len > sizeof(*header)) {
		header = (struct cmd_header *)d;
		d += sizeof(*header);
		len -= sizeof(*header);

		/*
		 * header 里的 payload_length 是“紧随其后的负载长度”。
		 * 如果它比当前剩余缓冲区还大，说明序列本身已经损坏，
		 * 这里直接报错返回，避免继续按错误偏移往后解析。
		 */
		if (header->payload_length > len)
			return -EINVAL;

		if (header->cmd_type == MIPI_DSI_PICTURE_PARAMETER_SET) {
			/*
			 * 识别到 MIPI_DSI_PICTURE_PARAMETER_SET（0x0A）后，
			 * 这里提取的就是最终要下发给面板的 PPS 负载。
			 */
			if (header->payload_length < sizeof(*pps)) {
				dev_err(dev,
					"[dsi2] dt-init-seq PPS too short: %u < %zu\n",
					header->payload_length, sizeof(*pps));
				return -EINVAL;
			}

			dsc_packed_pps = devm_kmemdup(dsi2->dev, d,
						      header->payload_length, GFP_KERNEL);
			if (!dsc_packed_pps)
				return -ENOMEM;

			pps = (struct drm_dsc_picture_parameter_set *)dsc_packed_pps;
			pps_payload_len = header->payload_length;
			dw_mipi_dsi2_dump_dsc_pps(dev, "dt-init-seq",
						pps,
						pps_payload_len);
			break;
		}

		d += header->payload_length;
		len -= header->payload_length;
	}
	} else {	/* 使用从固件获取的 init sequence 数据 */
		/*
		 * 这里解析的是 firmware 里的 panel init sequence。
		 * 如果其中包含 0x0A（MIPI_DSI_PICTURE_PARAMETER_SET），
		 * 那它就是实际更值得相信的 PPS 来源，因为它和本次加载的
		 * vis-timing.bin 属于同一份配置数据。
		 */
		d = (char *)init_seq_data;
		len = init_seq_len;
		dev_info(dev, "found init_seq_data,length is %d\n", len);
		while (len > sizeof(*header)) {
			header = (struct cmd_header *)d;
			dev_dbg(dev, "Checking header: type=0x%02x, delay=%u, payload_len=%u\n",
           		header->cmd_type, header->delay, header->payload_length);
			d += sizeof(*header);
			len -= sizeof(*header);

			if (header->payload_length > len)
				return -EINVAL;

			if (header->cmd_type == MIPI_DSI_PICTURE_PARAMETER_SET) {
				if (header->payload_length < sizeof(*pps)) {
					dev_err(dev,
						"[dsi2] fw-init-seq PPS too short: %u < %zu\n",
						header->payload_length, sizeof(*pps));
					return -EINVAL;
				}

				dsc_packed_pps = devm_kmemdup(dev, d,
							      header->payload_length, GFP_KERNEL);
				dev_dbg(dev, "Found PPS header\n");
				if (!dsc_packed_pps)
					return -ENOMEM;

				pps = (struct drm_dsc_picture_parameter_set *)dsc_packed_pps;
				pps_payload_len = header->payload_length;
				dw_mipi_dsi2_dump_dsc_pps(dev, "fw-init-seq",
							pps,
							pps_payload_len);
				break;
			}
			d += header->payload_length;
			len -= header->payload_length;
		}
	}

	/*
	 * 如果 DSC 已经被宣告开启，但无论 firmware/DT 都找不到 0x0A PPS，
	 * 后续链路就会进入“控制器按 DSC 方式工作、面板却没有拿到完整 PPS”
	 * 的不一致状态；这种情况下继续跑下去很容易把显示链路拖死。
	 * 所以这里直接把 dsc_enable 拉低，并返回错误，避免进入半配置状态。
	 */
	if (!pps) {
		dev_err(dsi2->dev, "not found dsc pps definition\n");
		dsi2->dsc_enable = false;
		if (dsi2->slave)
			dsi2->slave->dsc_enable = false;
		return -EINVAL;
	}

	dsi2->pps = pps;

	if (dsi2->slave) {
		pic_width = be16_to_cpu(pps->pic_width) / 2;

		/*
		 * 双 DSI 分屏场景下，面板总宽度需要折半写回 PPS，
		 * 否则从控制器仍会按整屏宽度发送，导致左右链路参数不一致。
		 */
		dsi2->pps->pic_width = cpu_to_be16(pic_width);
		dev_info(dsi2->dev, "[dsi2] dsc pic_width change from %d to %d\n",
			 pic_width * 2, pic_width);
		dw_mipi_dsi2_dump_dsc_pps(dev, "runtime-adjusted",
					 dsi2->pps,
					 pps_payload_len);
	}

	return 0;
}

static int dw_mipi_dsi2_connector_init(struct dw_mipi_dsi2 *dsi2)
{
	struct drm_encoder *encoder = &dsi2->encoder;
	struct drm_connector *connector = &dsi2->connector;
	struct drm_device *drm_dev = dsi2->drm_dev;
	struct device *dev = dsi2->dev;
	int ret;

	ret = drm_connector_init(drm_dev, connector,
				 &dw_mipi_dsi2_atomic_connector_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret) {
		DRM_DEV_ERROR(dev, "Failed to initialize connector\n");
		return ret;
	}

	drm_connector_helper_add(connector,
				 &dw_mipi_dsi2_connector_helper_funcs);
	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret < 0) {
		DRM_DEV_ERROR(dev, "Failed to attach encoder: %d\n", ret);
		goto connector_cleanup;
	}

	return 0;

connector_cleanup:
	connector->funcs->destroy(connector);

	return ret;
}

static int dw_mipi_dsi2_register_sub_dev(struct dw_mipi_dsi2 *dsi2,
					 struct drm_connector *connector)
{
	struct rockchip_drm_private *private;
	struct device *dev = dsi2->dev;

	private = connector->dev->dev_private;

	if (dsi2->split_area)
		drm_object_attach_property(&connector->base,
					   private->split_area_prop,
					   dsi2->split_area);

	dsi2->sub_dev.connector = connector;
	dsi2->sub_dev.of_node = dev->of_node;
	dsi2->sub_dev.loader_protect = dw_mipi_dsi2_encoder_loader_protect;
	rockchip_drm_register_sub_dev(&dsi2->sub_dev);

	return 0;
}

static int dw_mipi_dsi2_bind(struct device *dev, struct device *master,
			    void *data)
{
	struct dw_mipi_dsi2 *dsi2 = dev_get_drvdata(dev);
	struct drm_device *drm_dev = data;
	struct drm_encoder *encoder = &dsi2->encoder;
	struct device_node *of_node = dsi2->dev->of_node;
	struct drm_connector *connector = NULL;
	enum drm_bridge_attach_flags flags;
	int ret;

	dsi2->drm_dev = drm_dev;
	ret = dw_mipi_dsi2_dual_channel_probe(dsi2);
	if (ret){
		DRM_DEV_INFO(dev, "Dual-channel probe err (slave not ready): %d\n", ret);
		return ret;}

	if (dsi2->master) {
        DRM_DEV_INFO(dev, "Dual-channel, dsi2->master done. \n");
        return 0;
    }

	DRM_DEV_INFO(dev, "Trying to find panel or bridge...\n");
	ret = drm_of_find_panel_or_bridge(dev->of_node, 1, -1,
					  &dsi2->panel, &dsi2->bridge);
	if (ret) {
		DRM_DEV_ERROR(dev, "Failed to find panel or bridge: %d\n", ret);
		return ret;
	}

	if (dsi2->panel) {
		DRM_DEV_INFO(dev, "Found panel: %s\n", dev_name(dsi2->panel->dev));
	} else if (dsi2->bridge) {
		struct device *bridge_dev = dev;
		if (dsi2->bridge->dev)
			 bridge_dev = (struct device *)dsi2->bridge->dev;  // 显式转换
		DRM_DEV_INFO(dev, "Found bridge: %s\n", dev_name(bridge_dev));
	}
	DRM_DEV_INFO(dev, "Trying to get dsc params from sink\n");
	dw_mipi_dsi2_get_dsc_params_from_sink(dsi2, dsi2->panel, dsi2->bridge,dev);
	encoder->possible_crtcs = rockchip_drm_of_find_possible_crtcs(drm_dev,
								      of_node);
	if (dsi2->dsc_enable) {
        DRM_DEV_INFO(dev, "DSC enabled (slice: %dx%d, version: %d.%d)\n",
                    dsi2->slice_width, dsi2->slice_height,
                    dsi2->version_major, dsi2->version_minor);
    }

	ret = drm_simple_encoder_init(drm_dev, encoder, DRM_MODE_ENCODER_DSI);
	if (ret) {
		DRM_ERROR("Failed to initialize encoder with drm\n");
		return ret;
	}

	drm_encoder_helper_add(encoder, &dw_mipi_dsi2_encoder_helper_funcs);

	if (dsi2->bridge) {
		struct list_head *connector_list =
			&drm_dev->mode_config.connector_list;

		dsi2->bridge->driver_private = &dsi2->host;
		dsi2->bridge->encoder = encoder;

		flags = dsi2->bridge->ops & DRM_BRIDGE_OP_MODES ?
			DRM_BRIDGE_ATTACH_NO_CONNECTOR : 0;
		ret = drm_bridge_attach(encoder, dsi2->bridge, NULL, flags);
		if (ret) {
			DRM_DEV_ERROR(dev,
				      "Failed to attach bridge: %d\n", ret);
			goto encoder_cleanup;
		}

		if (!(flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR))
			list_for_each_entry(connector, connector_list, head)
				if (drm_connector_has_possible_encoder(connector,
								       encoder))
					break;
	}

	if (dsi2->panel || (dsi2->bridge && (flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR))) {
		ret = dw_mipi_dsi2_connector_init(dsi2);
		if (ret)
			goto encoder_cleanup;

		connector = &dsi2->connector;
	}

	if (connector) {
		ret = dw_mipi_dsi2_register_sub_dev(dsi2, connector);
		if (ret)
			goto encoder_cleanup;
	}

	/*
	 * 这里先刻意不在 bind() 阶段再次调用 pm_runtime_enable()。
	 *
	 * 原因说明：
	 * 1. 当前驱动在 dw_mipi_dsi2_probe() 里已经执行过一次
	 *    pm_runtime_enable(dev)。
	 * 2. component 框架随后还会进入 dw_mipi_dsi2_bind()；如果这里再对
	 *    master/slave 各调用一次，就会把 runtime PM enable 计数继续加上去。
	 * 3. 但后续 remove/unbind 路径里的 disable 次数并不总能与这里形成严格
	 *    对称，实测会触发 "Unbalanced pm_runtime_enable!"。
	 * 这里选择保留 probe() 中的 enable，把 bind() 中这组重复 enable
	 * 注释掉，先消除内核内部 enable/disable 不平衡问题，再继续观察是否影响
	 * 点屏、DSC、以及后续 adb 恢复链路。
	 */
	/* pm_runtime_enable(dsi2->dev); */
	/* if (dsi2->slave) */
	/* 	pm_runtime_enable(dsi2->slave->dev); */

	if (dsi2->slave) {
		DRM_DEV_INFO(dev, "Dual-channel ready (master:%s + slave:%s)\n",
                    		dev_name(dev), dev_name(dsi2->slave->dev));
    } else {
		DRM_DEV_INFO(dev, "Running in SINGLE-channel mode (lanes: %d)\n",dsi2->lanes);
        DRM_DEV_INFO(dev, "Initialized in %s mode\n",dw_mipi_dsi2_is_cmd_mode(dsi2) ? "CMD" : "VIDEO");
    }
	return 0;

encoder_cleanup:
	encoder->funcs->destroy(encoder);

	return ret;
}

static void dw_mipi_dsi2_unbind(struct device *dev, struct device *master,
			       void *data)
{
	struct dw_mipi_dsi2 *dsi2 = dev_get_drvdata(dev);

	if (dsi2->sub_dev.connector) {
		rockchip_drm_unregister_sub_dev(&dsi2->sub_dev);

		if (dsi2->connector.funcs)
			dsi2->connector.funcs->destroy(&dsi2->connector);
	}

	pm_runtime_disable(dsi2->dev);
	if (dsi2->slave)
		pm_runtime_disable(dsi2->slave->dev);

	dsi2->encoder.funcs->destroy(&dsi2->encoder);
}

static const struct component_ops dw_mipi_dsi2_ops = {
	.bind	= dw_mipi_dsi2_bind,
	.unbind	= dw_mipi_dsi2_unbind,
};

struct dsi2_irq_data {
	u32 offeset;
	char *irq_src;
};

static const struct dsi2_irq_data dw_mipi_dsi2_irq_data[] = {
	{DSI2_INT_ST_PHY, "int_st_phy"},
	{DSI2_INT_ST_TO, "int_st_to"},
	{DSI2_INT_ST_ACK, "int_st_ack"},
	{DSI2_INT_ST_IPI, "int_st_ipi"},
	{DSI2_INT_ST_FIFO, "int_st_fifo"},
	{DSI2_INT_ST_PRI, "int_st_pri"},
	{DSI2_INT_ST_CRI, "int_st_cri"},
};

static irqreturn_t dw_mipi_dsi2_irq_handler(int irq, void *dev_id)
{

	struct dw_mipi_dsi2 *dsi2 = dev_id;
	u32 int_st;
	unsigned int i;

	regmap_read(dsi2->regmap, INT_ST_MAIN, &int_st);

	for (i = 0; i < ARRAY_SIZE(dw_mipi_dsi2_irq_data); i++)
		if (int_st & BIT(i))
			DRM_DEV_DEBUG(dsi2->dev, "%s\n",
				      dw_mipi_dsi2_irq_data[i].irq_src);

	return IRQ_HANDLED;
}

static const struct regmap_config dw_mipi_dsi2_regmap_config = {
	.name = "host",
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.fast_io = true,
	.max_register = DSI2_MAX_REGISGER,
};

static int dw_mipi_dsi2_host_attach(struct mipi_dsi_host *host,
				   struct mipi_dsi_device *device)
{
	struct dw_mipi_dsi2 *dsi2 = host_to_dsi2(host);

	if (dsi2->master)
		return 0;

	if (device->lanes < 1 || device->lanes > 8)
		return -EINVAL;

	dsi2->client = device->dev.of_node;
	dsi2->lanes = device->lanes;
	dsi2->channel = device->channel;
	dsi2->format = device->format;
	dsi2->mode_flags = device->mode_flags;

	return 0;
}

static int dw_mipi_dsi2_host_detach(struct mipi_dsi_host *host,
				   struct mipi_dsi_device *device)
{
	return 0;
}

static int dw_mipi_dsi2_read_from_fifo(struct dw_mipi_dsi2 *dsi2,
				      const struct mipi_dsi_msg *msg)
{
	u8 *payload = msg->rx_buf;
	u8 data_type;
	u16 wc;
	int i, j, ret, len = msg->rx_len;
	unsigned int vrefresh = drm_mode_vrefresh(&dsi2->mode);
	u32 val;

	ret = regmap_read_poll_timeout(dsi2->regmap, DSI2_CORE_STATUS,
				       val, val & CRI_RD_DATA_AVAIL,
				       0, DIV_ROUND_UP(1000000, vrefresh));
	if (ret) {
		DRM_DEV_ERROR(dsi2->dev, "CRI has no available read data\n");
		return ret;
	}

	regmap_read(dsi2->regmap, DSI2_CRI_RX_HDR, &val);
	data_type = val & 0x3f;

	if (mipi_dsi_packet_format_is_short(data_type)) {
		for (i = 0; i < len && i < 2; i++)
			payload[i] = (val >> (8 * (i + 1))) & 0xff;

		return 0;
	}

	wc = (val >> 8) & 0xffff;
	/* Receive payload */
	for (i = 0; i < len && i < wc; i += 4) {
		regmap_read(dsi2->regmap, DSI2_CRI_RX_PLD, &val);
		for (j = 0; j < 4 && j + i < len && j + i < wc; j++)
			payload[i + j] = val >> (8 * j);
	}

	return 0;
}

/**
* struct mipi_dsi_packet - 表示协议格式的 MIPI DSI 数据包
* @size：数据包的大小（以字节为单位）
* @header：构成标头的四个字节（数据 ID、字数或
* 数据包数据和 ECC）
* @payload_length：有效负载中的字节数
* @payload：指向包含有效负载的缓冲区（如果有）的指针
struct mipi_dsi_packet {
size_t size;
u8 header[4];
size_t payload_length;
const u8 *payload;
};*/
static ssize_t dw_mipi_dsi2_transfer(struct dw_mipi_dsi2 *dsi2,
				    const struct mipi_dsi_msg *msg)
{
	struct mipi_dsi_packet packet;
	int ret;
	u32 val;
	u32 mode;

	pm_runtime_get_sync(dsi2->dev);
	dw_mipi_dsi2_clk_management(dsi2);
	regmap_update_bits(dsi2->regmap, DSI2_DSI_VID_TX_CFG, LPDT_DISPLAY_CMD_EN,
			   msg->flags & MIPI_DSI_MSG_USE_LPM ? LPDT_DISPLAY_CMD_EN : 0);

	/* create a packet to the DSI protocol */
	ret = mipi_dsi_create_packet(&packet, msg);
	if (ret) {
		DRM_DEV_ERROR(dsi2->dev, "failed to create packet: %d\n", ret);
		goto err;
	}
	dev_info(dsi2->dev, "Transfer: type=0x%02x, tx_len=%zu, rx_len=%zu\n",msg->type, msg->tx_len, msg->rx_len);

	/* 缓冲区打印 */
	// if (msg->tx_buf) {
	// 	const u8 *tx_buf = msg->tx_buf;  // 转换为u8指针
	// 	dev_info(dsi2->dev, "TX buf ptr: %p, len: %zu\n", msg->tx_buf, msg->tx_len);        
	// 	if (msg->tx_len > 0) {
	// 		dev_info(dsi2->dev, "First 4 bytes: 0x%02x 0x%02x 0x%02x 0x%02x\n",
	// 				msg->tx_len > 0 ? tx_buf[0] : 0xFF,
	// 				msg->tx_len > 1 ? tx_buf[1] : 0xFF,
	// 				msg->tx_len > 2 ? tx_buf[2] : 0xFF,
	// 				msg->tx_len > 3 ? tx_buf[3] : 0xFF);
	// 		print_hex_dump(KERN_ERR, "TX buf: ", DUMP_PREFIX_OFFSET, 16, 1,
	// 						msg->tx_buf, min_t(size_t, msg->tx_len, 64), true);
	// 	}
	// }

	ret = cri_fifos_wait_avail(dsi2);
	if (ret)
		goto err;

	/* Send payload */
	while (DIV_ROUND_UP(packet.payload_length, 4)) {
		/* check cri interface is not busy */
		if (packet.payload_length < 4) {
			/* send residu payload */
			val = 0;
			memcpy(&val, packet.payload, packet.payload_length);
			regmap_write(dsi2->regmap, DSI2_CRI_TX_PLD, val);
			packet.payload_length = 0;
		} else {
			val = get_unaligned_le32(packet.payload);
			regmap_write(dsi2->regmap, DSI2_CRI_TX_PLD, val);
			packet.payload += 4;
			packet.payload_length -= 4;
		}
	}

	/* Send packet header */
	mode = CMD_TX_MODE(msg->flags & MIPI_DSI_MSG_USE_LPM ? 1 : 0);
	val = get_unaligned_le32(packet.header);

	regmap_write(dsi2->regmap, DSI2_CRI_TX_HDR, mode | val);

	ret = cri_fifos_wait_avail(dsi2);
	if (ret)
		goto err;

	if (msg->rx_len) {
		 if (!msg->rx_buf) {// 确保使用正确的缓冲区
            dev_err(dsi2->dev, "No rx buffer provided!");
            return -EINVAL;
        }
		dev_dbg(dsi2->dev, "prepare to read %zu bytes from FIFO\n", msg->rx_len);
		//udelay(100);
		msleep (1);  // 根据屏幕 IC 响应时间调整
		ret = dw_mipi_dsi2_read_from_fifo(dsi2, msg);
		if (ret < 0){
			DRM_DEV_DEBUG(dsi2->dev, "Read from fifo failed \n");
			goto err;
		}

		print_hex_dump(KERN_DEBUG, "rx_buf received: ", DUMP_PREFIX_OFFSET, 16, 1,msg->rx_buf, ((msg->rx_len<64)?msg->rx_len:64), true);
		// 打印 rx_buf 的内容（16进制 + ASCII）
		// print_hex_dump(
		// 	KERN_INFO,            // 日志级别为 INFO
		// 	"rx_buf: ",           // 每行前缀字符串
		// 	DUMP_PREFIX_OFFSET,   // 显示偏移量（如 00000000:）
		// 	16,                   // 每行16字节
		// 	1,                    // 不分组
		// 	msg->rx_buf,          // 数据缓冲区
		// 	msg->rx_len,          // 数据长度
		// 	true                  // 显示ASCII字符
		// );
	}

	pm_runtime_put(dsi2->dev);

	if (dsi2->slave)
		dw_mipi_dsi2_transfer(dsi2->slave, msg);

	dev_dbg(dsi2->dev, "dw_mipi_dsi2_transfer %zu byte done\n",msg->tx_len);
	return msg->tx_len;

err:
	pm_runtime_put(dsi2->dev);
	return ret;
}

static ssize_t dw_mipi_dsi2_host_transfer(struct mipi_dsi_host *host,
					 const struct mipi_dsi_msg *msg)
{
	struct dw_mipi_dsi2 *dsi2 = host_to_dsi2(host);

	return dw_mipi_dsi2_transfer(dsi2, msg);
}

static const struct mipi_dsi_host_ops dw_mipi_dsi2_host_ops = {
	.attach = dw_mipi_dsi2_host_attach,
	.detach = dw_mipi_dsi2_host_detach,
	.transfer = dw_mipi_dsi2_host_transfer,
};

// 新增/dev/mipi_dsi0 操作函数
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
static int dsi_raw_open(struct inode *inode, struct file *file)
{
	struct dw_mipi_dsi2 *dsi2 = container_of(file->private_data,
											struct dw_mipi_dsi2, miscdev);
	file->private_data = dsi2; // echo/cat共享控制器实例
	return 0;
}

static int dsi_raw_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t dsi_raw_read(struct file *file, char __user *buf,
                          size_t count, loff_t *ppos)
{
    // struct dw_mipi_dsi2 *dsi2 = container_of(file->private_data,
    //                                        struct dw_mipi_dsi2, miscdev);
	struct dw_mipi_dsi2 *dsi2 = file->private_data;
	struct device *dev = dsi2->dev;

	dev_dbg(dev, "Read request: count=%zu, rx_len=%zu\n",count, dsi2->rx_len);
	if (!dsi2->data_ready) {
		dev_dbg(dsi2->dev, "EOF or No data available for reading");
		return 0; // 没有数据可读,表示EOF（数据结束）cat执行本函数时，收到0：立即丢弃缓冲区并退出
	}

	/* 2. 检查可供cat读取的用户缓冲区大小 */
    if (count < dsi2->rx_len) {
        dev_warn(dsi2->dev, "User buffer too small (%zu < rx_len:%zu)", 
                count, dsi2->rx_len);
        return -ENOSPC;
    }

    // 3. 复制数据到用户空间
    if (copy_to_user(buf, dsi2->rx_buf, dsi2->rx_len)){
		return -EFAULT;
		dev_info(dev, "Read error\n");
	}else{
		dev_dbg(dev, "Returned %zd bytes to user\n", dsi2->rx_len);
	}

 	dev_dbg(dsi2->dev, "Read %zu bytes successfully", dsi2->rx_len);
	dsi2->data_ready=false;
	return dsi2->rx_len;   //收到N>0：处理前N字节
}
/**
 * struct mipi_dsi_msg - read/write DSI buffer
 * @channel: virtual channel id
 * @type: payload data type
 * @flags: flags controlling this message transmission
 * @ctrl: ctrl index to transmit on
 * @wait_ms: duration in ms to wait after message transmission
 * @tx_len: length of @tx_buf
 * @tx_buf: data to be written
 * @rx_len: length of @rx_buf
 * @rx_buf: data to be read, or NULL
struct mipi_dsi_msg {
	u8 channel;
	u8 type;
	u16 flags;
	u32 ctrl;
	u32 wait_ms;

	size_t tx_len;
	const void *tx_buf;

	size_t rx_len;
	void *rx_buf;
};*/
static ssize_t dsi_raw_write(struct file *file, const char __user *buf,
                           size_t count, loff_t *ppos)
{
	// struct dw_mipi_dsi2 *dsi2 = container_of(file->private_data,
	// 										struct dw_mipi_dsi2, miscdev);	
	struct dw_mipi_dsi2 *dsi2 = file->private_data;
    struct mipi_dsi_msg msg = {0};
    u8 *cmd_buf = NULL;
	struct panel_cmd_header *hdr=NULL;
    ssize_t  ret;
	struct device *dev = dsi2->dev;

    /* 1. 基础校验 */
    if (count < sizeof(struct panel_cmd_header)) {
        dev_err(dsi2->dev, "Invalid command length: %zu < %zu\n",
               count, sizeof(struct panel_cmd_header));
        return -EINVAL;
    }

    /* 2. 从用户空间拷贝数据 */
    cmd_buf = kmalloc(count, GFP_KERNEL);
    if (!cmd_buf)
        return -ENOMEM;

    if (copy_from_user(cmd_buf, buf, count)) {
        ret = -EFAULT;
        goto out_free;
    }

    /* 3. 解析命令头 */
    hdr = (struct panel_cmd_header *)cmd_buf;
    /* 校验payload长度 */
	if(hdr->data_type==MIPI_DSI_DCS_READ){
		dev_dbg(dsi2->dev, "DSI will read lenth: %u\n",hdr->payload_length);
	}else{
		if (hdr->payload_length != count - sizeof(*hdr)) {
			dev_err(dsi2->dev, "Payload length mismatch: %u != %zu\n",
				hdr->payload_length, count - sizeof(*hdr));
			ret = -EINVAL;
			goto out_free;
		}
	}

	/* 4. 构造DSI消息 */
	msg.type = hdr->data_type;
	msg.flags = MIPI_DSI_MSG_USE_LPM;

	// 特殊处理读命令 (0x06)
	if (msg.type == MIPI_DSI_DCS_READ) {
		msg.rx_len = hdr->payload_length;	//这里面此时用来传递读取的长度		
		msg.rx_buf = dsi2->rx_buf;
		msg.tx_len = 1;//必然为1，因此接下来只发送一个byte reg地址
		msg.tx_buf = cmd_buf + sizeof(*hdr);
		// 重置状态
		dsi2->rx_len = 0;
		dsi2->data_ready = false;
	}
	else if (msg.type == MIPI_DSI_SET_MAXIMUM_RETURN_PACKET_SIZE) {//0x37
		msg.tx_len = hdr->payload_length;
		msg.tx_buf = cmd_buf + sizeof(*hdr);
		dev_dbg(dsi2->dev, "set MIPI_DSI_SET_MAXIMUM_RETURN_PACKET_SIZE\n");
	}
	else{
		msg.tx_len = hdr->payload_length;
		msg.tx_buf = cmd_buf + sizeof(*hdr);
	}
    dev_dbg(dev, "Processing %s (0x%02x) command, payload len: %u\n",
			hdr->data_type == MIPI_DSI_DCS_READ ? "READ" : "WRITE",
			hdr->data_type, hdr->payload_length);

    /* 5. 发送命令 */
	ret = dw_mipi_dsi2_transfer(dsi2, &msg);
	if (ret < 0) {
		dev_err(dsi2->dev, "DSI transfer failed: %zd\n", ret);
		goto out_free;
	}
	/* 6. 写入读命令后更新状态 */
    if (ret >= 0 && msg.type == MIPI_DSI_DCS_READ) {
        dsi2->rx_len = msg.rx_len;      // 记录实际读取长度
        dsi2->data_ready = true;       // 标记数据就绪
    }
    /* 7. 处理延时 */ //强制每个命令发送后执行1ms的延时
    if (hdr->delay+1)
        msleep(hdr->delay+1);

out_free:
	kfree(cmd_buf);
	return ret<0?ret:count;// 返回错误 或者 成功写入的字节数

}

// 3. 文件操作结构体定义
static const struct file_operations dsi_raw_fops = {
    .owner = THIS_MODULE,
    .open = dsi_raw_open,
    .release = dsi_raw_release,
    .read = dsi_raw_read,
    .write = dsi_raw_write,
};
static int dw_mipi_dsi2_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dw_mipi_dsi2 *dsi2;
	struct resource *res;
	void __iomem *regs;
	int id;
	int ret;

	dsi2 = devm_kzalloc(dev, sizeof(*dsi2), GFP_KERNEL);
	if (!dsi2)
		return -ENOMEM;

	id = of_alias_get_id(dev->of_node, "dsi");
	if (id < 0)
		id = 0;

	dsi2->dev = dev;
	dsi2->id = id;
	dsi2->pdata = of_device_get_match_data(dev);
	platform_set_drvdata(pdev, dsi2);

	if (device_property_read_bool(dev, "auto-calculation-mode"))
		dsi2->auto_calc_mode = true;

	if (device_property_read_bool(dev, "disable-hold-mode"))
		dsi2->disable_hold_mode = true;

	if (device_property_read_bool(dev, "dual-connector-split")) {
		dsi2->dual_connector_split = true;

		if (device_property_read_bool(dev, "left-display"))
			dsi2->left_display = true;
	}

	if (device_property_read_u32(dev, "split-area", &dsi2->split_area))
		dsi2->split_area = 0;

	dsi2->support_psr = device_property_read_bool(dev, "support-psr");
	if (dsi2->support_psr && dsi2->auto_calc_mode) {
		dsi2->auto_calc_mode = false;
		dev_info(dev, "disable auto-calculation-mode in PSR mode\n");
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	dsi2->irq = platform_get_irq(pdev, 0);
	if (dsi2->irq < 0)
		return dsi2->irq;

	dsi2->pclk = devm_clk_get(dev, "pclk");
	if (IS_ERR(dsi2->pclk)) {
		ret = PTR_ERR(dsi2->pclk);
		DRM_DEV_ERROR(dev, "Unable to get pclk: %d\n", ret);
		return ret;
	}

	dsi2->sys_clk = devm_clk_get(dev, "sys_clk");
	if (IS_ERR(dsi2->sys_clk)) {
		ret = PTR_ERR(dsi2->sys_clk);
		DRM_DEV_ERROR(dev, "Unable to get sys_clk: %d\n", ret);
		return ret;
	}

	dsi2->regmap = devm_regmap_init_mmio(dev, regs,
					    &dw_mipi_dsi2_regmap_config);
	if (IS_ERR(dsi2->regmap)) {
		ret = PTR_ERR(dsi2->regmap);
		DRM_DEV_ERROR(dev, "failed to init register map: %d\n", ret);
		return ret;
	}

	dsi2->grf = syscon_regmap_lookup_by_phandle(dev->of_node,
						    "rockchip,grf");
	if (IS_ERR(dsi2->grf)) {
		ret = PTR_ERR(dsi2->grf);
		DRM_DEV_ERROR(dsi2->dev, "Unable to get grf: %d\n", ret);
		return ret;
	}

	dsi2->apb_rst = devm_reset_control_get(dev, "apb");
	if (IS_ERR(dsi2->apb_rst)) {
		ret = PTR_ERR(dsi2->apb_rst);
		DRM_DEV_ERROR(dev,
			      "Unable to get reset control: %d\n", ret);
		return ret;
	}

	dsi2->dcphy = devm_phy_optional_get(dev, "dcphy");
	if (IS_ERR(dsi2->dcphy)) {
		ret = PTR_ERR(dsi2->dcphy);
		DRM_DEV_ERROR(dev, "failed to get mipi dcphy: %d\n", ret);
		return ret;
	}

	dsi2->te_gpio = devm_gpiod_get_optional(dsi2->dev, "te", GPIOD_IN);
	if (IS_ERR(dsi2->te_gpio))
		dsi2->te_gpio = NULL;

	if (dsi2->te_gpio) {
		ret = devm_request_threaded_irq(dsi2->dev, gpiod_to_irq(dsi2->te_gpio),
						dw_mipi_dsi2_te_irq_handler, NULL,
						IRQF_TRIGGER_RISING | IRQF_ONESHOT,
						"PANEL-TE", dsi2);
		if (ret) {
			dev_err(dsi2->dev, "failed to request TE IRQ: %d\n", ret);
			return ret;
		}
	}

	ret = devm_request_irq(dev, dsi2->irq, dw_mipi_dsi2_irq_handler,
			       IRQF_SHARED, dev_name(dev), dsi2);
	if (ret) {
		DRM_DEV_ERROR(dev, "failed to request irq: %d\n", ret);
		return ret;
	}

	dsi2->host.ops = &dw_mipi_dsi2_host_ops;
	dsi2->host.dev = dev;
	ret = mipi_dsi_host_register(&dsi2->host);
	if (ret) {
		DRM_DEV_ERROR(dev, "Failed to register MIPI host: %d\n", ret);
		return ret;
	}

	/* 4. 添加misc设备（新增部分）*/
	dsi2->miscdev.minor = MISC_DYNAMIC_MINOR;
	dsi2->miscdev.name = kasprintf(GFP_KERNEL, "mipi_dsi%d", dsi2->id);
	if (!dsi2->miscdev.name) {
		ret = -ENOMEM;
		goto err_host_unregister;
	}

	dsi2->miscdev.fops = &dsi_raw_fops;
	dsi2->miscdev.parent = dev;

	ret = misc_register(&dsi2->miscdev);
	if (ret) {
		dev_err(dev, "Failed to register misc device: %d\n", ret);
		goto err_free_name;
	}

	/*
	 * runtime PM 只在 probe() 阶段使能一次。
	 *
	 * bind() 只是 component 组装显示链路的后续阶段，不再重复 enable，避免与
	 * unbind/remove 路径的 disable 次数失配，触发 "Unbalanced
	 * pm_runtime_enable!"。
	 */
	pm_runtime_enable(dev);
	dev_info(dev, "DSI%d initialized, device node: /dev/%s\n",dsi2->id, dsi2->miscdev.name);
	// 初始化接收缓冲区
	dsi2->rx_buf = kmalloc(1024, GFP_KERNEL);
	if (!dsi2->rx_buf)
		return -ENOMEM;
	dsi2->rx_len=0;
	//-----------以上是添加misc设备---------------------------------------------
	return component_add(&pdev->dev, &dw_mipi_dsi2_ops);

err_free_name:
	kfree(dsi2->miscdev.name);
	err_host_unregister:
	mipi_dsi_host_unregister(&dsi2->host);
	return ret;
}

static int dw_mipi_dsi2_remove(struct platform_device *pdev)
{
	struct dw_mipi_dsi2 *dsi2 = platform_get_drvdata(pdev);

	/* 1. 注销misc设备*/
	misc_deregister(&dsi2->miscdev);
	kfree(dsi2->miscdev.name);

	/* 3. 原有资源释放部分 */
	pm_runtime_disable(&pdev->dev);
	mipi_dsi_host_unregister(&dsi2->host);
	return 0;
}

static __maybe_unused int dw_mipi_dsi2_runtime_suspend(struct device *dev)
{
	struct dw_mipi_dsi2 *dsi2 = dev_get_drvdata(dev);

	clk_disable_unprepare(dsi2->pclk);
	clk_disable_unprepare(dsi2->sys_clk);

	return 0;
}

static __maybe_unused int dw_mipi_dsi2_runtime_resume(struct device *dev)
{
	struct dw_mipi_dsi2 *dsi2 = dev_get_drvdata(dev);

	clk_prepare_enable(dsi2->pclk);
	clk_prepare_enable(dsi2->sys_clk);

	return 0;
}

static const struct dev_pm_ops dw_mipi_dsi2_rockchip_pm_ops = {
	SET_RUNTIME_PM_OPS(dw_mipi_dsi2_runtime_suspend,
			   dw_mipi_dsi2_runtime_resume, NULL)
};

static const u32 rk3588_dsi0_grf_reg_fields[MAX_FIELDS] = {
	[TXREQCLKHS_EN]		= GRF_REG_FIELD(0x0000, 11, 11),
	[GATING_EN]		= GRF_REG_FIELD(0x0000, 10, 10),
	[IPI_SHUTDN]		= GRF_REG_FIELD(0x0000,  9,  9),
	[IPI_COLORM]		= GRF_REG_FIELD(0x0000,  8,  8),
	[IPI_COLOR_DEPTH]	= GRF_REG_FIELD(0x0000,  4,  7),
	[IPI_FORMAT]		= GRF_REG_FIELD(0x0000,  0,  3),
};

static const u32 rk3588_dsi1_grf_reg_fields[MAX_FIELDS] = {
	[TXREQCLKHS_EN]		= GRF_REG_FIELD(0x0004, 11, 11),
	[GATING_EN]		= GRF_REG_FIELD(0x0004, 10, 10),
	[IPI_SHUTDN]		= GRF_REG_FIELD(0x0004,  9,  9),
	[IPI_COLORM]		= GRF_REG_FIELD(0x0004,  8,  8),
	[IPI_COLOR_DEPTH]	= GRF_REG_FIELD(0x0004,  4,  7),
	[IPI_FORMAT]		= GRF_REG_FIELD(0x0004,  0,  3),
};

static const struct dw_mipi_dsi2_plat_data rk3588_mipi_dsi2_plat_data = {
	.dsi0_grf_reg_fields = rk3588_dsi0_grf_reg_fields,
	.dsi1_grf_reg_fields = rk3588_dsi1_grf_reg_fields,
	.dphy_max_bit_rate_per_lane = 4500000000ULL,
	.cphy_max_symbol_rate_per_lane = 2000000000ULL,
};

static const struct of_device_id dw_mipi_dsi2_dt_ids[] = {
	{
		.compatible = "rockchip,rk3588-mipi-dsi2",
		.data = &rk3588_mipi_dsi2_plat_data,
	},
	{}
};
MODULE_DEVICE_TABLE(of, dw_mipi_dsi2_dt_ids);

struct platform_driver dw_mipi_dsi2_rockchip_driver = {
	.probe	= dw_mipi_dsi2_probe,
	.remove = dw_mipi_dsi2_remove,
	.driver = {
		.of_match_table = dw_mipi_dsi2_dt_ids,
		.pm = &dw_mipi_dsi2_rockchip_pm_ops,
		.name = "dw-mipi-dsi2",
	},
};
