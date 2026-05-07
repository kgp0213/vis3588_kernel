# RK3588 直接推送内核升级方法

## 原理

设备采用 **extlinux 启动方案**，boot 分区是 ext4 文件系统，U-Boot 按 extlinux.conf 配置加载文件启动：

```
/boot/extlinux/extlinux.conf
───────────────────────────────────
label kernel-5.10.209-rk3588
    kernel /Image-5.10.209-rk3588
    devicetreedir /
    append root=/dev/mmcblk0p3 ...
───────────────────────────────────
```

内核是一个普通文件。替换文件 + 重启即完成升级。**不需要**瑞芯微刷机工具，**不需要**生成 `boot.img`。DSI2 驱动是 built-in（`CONFIG_ROCKCHIP_DW_MIPI_DSI=y`），不涉及内核模块 .ko。

---

## 完整流程

### 1. 修改源码

```bash
vim kernel-5.10/drivers/gpu/drm/rockchip/dw-mipi-dsi2-rockchip.c
```

### 2. 编译

```bash
export PATH="/path/to/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin:$PATH"
cd kernel-5.10

# 编译单文件 .o
make CROSS_COMPILE=aarch64-none-linux-gnu- ARCH=arm64 \
    drivers/gpu/drm/rockchip/dw-mipi-dsi2-rockchip.o

# 链接生成 Image
make CROSS_COMPILE=aarch64-none-linux-gnu- ARCH=arm64 Image

# 产物：arch/arm64/boot/Image（约 38MB）
```

> 编译产物的文件名叫 `Image`（Linux 内核 Makefile 写死在 `arch/arm64/boot/Makefile` 中）。
> 板端 Bootloader 按 `extlinux.conf` 找的是 `Image-5.10.209-rk3588`。
> 推送时需要重命名。

### 3. 部署

```bash
# 推送（adb push 自动完成重命名）
adb push arch/arm64/boot/Image /boot/Image-5.10.209-rk3588
adb shell sync && adb shell reboot
```

> boot 分区满时先清理：
> ```bash
> adb shell rm -f /boot/Image-5.10.209-rk3588.bak /boot/logo*.bmp
> ```

### 4. 验证

```bash
adb shell uname -a
# Linux vis3588 5.10.209-rk3588 #26 SMP ...
```

---

## 与传统 boot.img 方案对比

| | boot.img 方案 | 直接推送方案 |
|--|-------------|------------|
| 产物 | extboot.img（128MB，整个分区镜像） | Image（38MB，纯内核） |
| 工具 | `rkdeveloptool` / `upgrade_tool` | `adb push` |
| 设备状态 | 需进入 Loader / Maskrom 模式 | 正常运行中 |
| 适用场景 | 工厂烧录、分区表变更 | 开发调试、内核迭代 |

### boot.img 为什么 128MB

`boot.img` 是一个 **ext2 文件系统镜像**，大小等于 boot 分区物理大小（128MB）：

```
extboot.img (128MB) = ext2 元数据 + 所有文件 + zero-padding
```

普通单个文件（Image）是 38MB，整个分区镜像因填充到分区边界而增大到 128MB。

---

## 注意事项

1. **版本号不变不自动重链：** 如果 `uname -r` 未变，`make Image` 不会重新链接。强制重链：
   ```bash
   rm -f vmlinux.o vmlinux arch/arm64/boot/Image && make Image
   ```

2. **备份：** `adb pull /boot/Image-5.10.209-rk3588` 先备份一份。

3. **模块 vs built-in：** DSI2 驱动是 `=y`（built-in），只需更新 Image。如果是 `=m`（模块），还需同步 `/lib/modules/` 下的 .ko。
