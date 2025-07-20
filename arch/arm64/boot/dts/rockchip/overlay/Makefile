# SPDX-License-Identifier: GPL-2.0
ifeq ($(CONFIG_ARCH_ROCKCHIP), y)

dtbo-$(CONFIG_CPU_RK3588) += \
	rk3588-lubancat-i2c2-m0-overlay.dtbo \
	rk3588-lubancat-i2c2-m4-overlay.dtbo \
	rk3588-lubancat-i2c3-m0-overlay.dtbo \
	rk3588-lubancat-i2c3-m1-overlay.dtbo \
	rk3588-lubancat-i2c4-m3-overlay.dtbo \
	rk3588-lubancat-i2c5-m3-overlay.dtbo \
	rk3588-lubancat-i2c6-m3-overlay.dtbo \
	rk3588-lubancat-i2c7-m3-overlay.dtbo \
	rk3588-lubancat-i2c8-m2-overlay.dtbo \
	rk3588-lubancat-pwm0-m0-overlay.dtbo \
	rk3588-lubancat-pwm0-m2-overlay.dtbo \
	rk3588-lubancat-pwm1-m0-overlay.dtbo \
	rk3588-lubancat-pwm1-m2-overlay.dtbo \
	rk3588-lubancat-pwm3-ir-m3-overlay.dtbo \
	rk3588-lubancat-pwm10-m2-overlay.dtbo \
	rk3588-lubancat-pwm11-ir-m3-overlay.dtbo \
	rk3588-lubancat-pwm13-m1-overlay.dtbo \
	rk3588-lubancat-pwm13-m2-overlay.dtbo \
	rk3588-lubancat-pwm14-m1-overlay.dtbo \
	rk3588-lubancat-pwm14-m2-overlay.dtbo \
	rk3588-lubancat-pwm15-ir-m3-overlay.dtbo \
	rk3588-lubancat-spi0-m1-overlay.dtbo \
	rk3588-lubancat-spi0-m2-overlay.dtbo \
	rk3588-lubancat-uart0-m2-overlay.dtbo \
	rk3588-lubancat-uart1-m1-overlay.dtbo \
	rk3588-lubancat-uart3-m0-overlay.dtbo \
	rk3588-lubancat-uart4-m2-overlay.dtbo \
	rk3588-lubancat-uart5-m1-overlay.dtbo \
	rk3588-lubancat-uart6-m1-overlay.dtbo \
	rk3588-lubancat-uart7-m1-overlay.dtbo \
	rk3588-lubancat-uart7-m2-overlay.dtbo \
	rk3588-lubancat-uart9-m2-overlay.dtbo \
	rk3588-lubancat-5-dsi1-vp3-1080p-overlay.dtbo \
	rk3588-lubancat-5-dsi0-vp2-1024x600-overlay.dtbo \
	rk3588-lubancat-5-dsi1-vp3-1024x600-overlay.dtbo \
	rk3588-lubancat-5-dsi0-vp2-800x1280-overlay.dtbo \
	rk3588-lubancat-5-dsi1-vp3-800x1280-overlay.dtbo \
	rk3588-lubancat-5-edp-vp2-overlay.dtbo \
	rk3588-lubancat-dsi1-vp3-virtual-screen-overlay.dtbo \
	rk3588-lubancat-5-dp0-vp1-disabled-overlay.dtbo \
	vis-dsi0-vp2-cmd-4lane-m559.dtbo \
	vis-dsi0-vp2-cmd-4lane-dsc-m559.dtbo \
	vis-dsi0-vp2-cmd-8lane-c39.dtbo

endif

dtbotxt-$(CONFIG_ARCH_ROCKCHIP) += \
	README.rockchip-overlays.md

targets += $(dtbo-y) $(dtbotxt-y)

always-y		:= $(dtbo-y) $(dtbotxt-y)
clean-files		:= *.dtbo
