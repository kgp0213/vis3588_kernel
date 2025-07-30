# SPDX-License-Identifier: GPL-2.0
ifeq ($(CONFIG_ARCH_ROCKCHIP), y)

dtbo-$(CONFIG_CPU_RK3588) += \
	rk3588-i2c3-m3-overlay.dtbo \
	rk3588-i2c4-m2-overlay.dtbo \
	rk3588-i2c8-m3-overlay.dtbo \
	rk3588-i2c6-m4-overlay.dtbo \
	rk3588-i2c8-m1-overlay.dtbo \
	rk3588-spi4-m1-1cs-overlay.dtbo \
	rk3588-spi4-m1-2cs-overlay.dtbo \
	rk3588-5-dsi1-vp3-1080p-overlay.dtbo \
	rk3588-5-edp-vp2-overlay.dtbo \
	rk3588-dsi1-vp3-virtual-screen-overlay.dtbo \
	rk3588-5-dp0-vp1-disabled-overlay.dtbo \
	vis-dsi0-vp2-cmd-4lane-m559.dtbo \
	vis-dsi0-vp2-cmd-8lane-c39.dtbo

endif

dtbotxt-$(CONFIG_ARCH_ROCKCHIP) += \
	README.rockchip-overlays.md

targets += $(dtbo-y) $(dtbotxt-y)

always-y		:= $(dtbo-y) $(dtbotxt-y)
clean-files		:= *.dtbo
