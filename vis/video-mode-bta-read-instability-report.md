# RK3588 DSI2 VIDEO 模式 BTA 回读不稳定问题报告

## 一、问题描述

VIDEO 模式下通过 `/dev/mipi_dsi0` 读取 DCS 寄存器存在以下现象：

| 操作 | 现象 |
|------|------|
| LP 读 | 100% 失败，`CRI has no available read data` / `ret=-110` |
| HS 读 | 约 70~80% 成功率，偶发超时失败，但重试后可恢复 |
| HS 写 | 正常工作，无问题 |

## 二、根因分析

### 2.1 硬件约束

VIDEO 模式下 VOP2 持续通过 IPI 向 DSI 控制器推送像素数据，DSI 链路始终处于 HS 高速传输状态：

```
VOP2 → IPI → DSI2 Host Controller → DPHY → Panel
```

LP 命令所需的 BTA（Bus Turn Around，总线翻转）需要在消隐期内完成，但 VIDEO 模式下的消隐窗口不足以可靠完成 BTA 流程。

### 2.2 驱动未配置关键寄存器

RK3588 TRM Part2 中定义了以下与 BTA/读操作相关的寄存器，但 `dw-mipi-dsi2-rockchip.c` 驱动中**仅有宏定义，未实际写入配置**：

| 寄存器 | 地址 | 功能 | 驱动状态 |
|--------|------|------|---------|
| `DSI2_TIMEOUT_HSTX_CFG` | 0x0048 | HS TX 超时 | 有宏定义，未写入 |
| `DSI2_TIMEOUT_HSTXRDY_CFG` | 0x004c | HS TX Ready 超时 | 有宏定义，未写入 |
| `DSI2_TIMEOUT_LPRX_CFG` | 0x0050 | LP RX 超时 | 有宏定义，未写入 |
| `DSI2_TIMEOUT_LPTXRDY_CFG` | 0x0054 | LP TX Ready 超时 | 有宏定义，未写入 |
| `DSI2_TIMEOUT_LPTXTRIG_CFG` | 0x0058 | LP TX Trigger 超时 | 有宏定义，未写入 |
| `DSI2_TIMEOUT_LPTXULPS_CFG` | 0x005c | LP TX ULPS 超时 | 有宏定义，未写入 |
| `DSI2_TIMEOUT_BTA_CFG` | 0x0060 | BTA 超时 | 有宏定义，未写入 |
| `DSI2_PHY_MAX_RD_T_MAN_CFG` | 0x011c | PHY 最大读取时间 | 有宏定义，未写入 |

驱动源码第 972 行原文注释：
```c
/* there may be some timeout registers may be configured if desired */
```

此后的配置代码为空，说明该 TODO 未完成。

### 2.3 TRM 约束

RK3588 TRM 中明确指出 VIDEO 模式下 display command 的约束：

> - Up to one display command request can be made per line
> - Last line cannot receive display command requests
> - Display command requests must occur after the HSYNC period

其中 `lpdt_display_cmd_en` 位（`DSI_VID_TX_CFG` 寄存器）控制命令传输模式。

### 2.4 原始驱动的 Bug（已修复）

`dsi_raw_write()` 函数中存在三个 bug，导致 HS 读分支走不通：

1. **DCS_READ 硬编码 LP**：读命令始终使用 `MIPI_DSI_MSG_USE_LPM`，无视用户 -hs 标记
2. **5 字节 HS 读命令解析错误**：`cmd_buf[2] == count - 3` 意外匹配 legacy 3 字节头，payload 指向 flags 字节而非寄存器地址
3. **SET_MAXIMUM_RETURN_PACKET_SIZE 硬编码 LP**：0x37 命令同样无视 HS 标记

修复见 `vis/vismpwr.c` 和 `dw-mipi-dsi2-rockchip.c` 中 `dsi_raw_write()` 的当前实现。

## 三、实测数据

### 3.1 测试环境

| 项目 | 值 |
|------|-----|
| 内核 | `5.10.209-rk3588` |
| 面板 | 1260×2800, 4-lane DPHY |
| 模式 | VIDEO（Burst, HSA/HBP/HFP） |
| 工具 | `vismpwr` (HS 读模式) |

### 3.2 20 轮 HS 读测试结果

```
前 10 轮（首次测试）：
  0x2F: 0c     (10/10 ✅)
  0xFA: 01~07  (6/10 ✅, 4 fail)
  0xC2: 08     (7/10 ✅, 3 fail)
  0x51: 2f30~2f34 (10/10 ✅)

后 10 轮（第二次测试）：
  0x2F: 0c     (10/10 ✅)
  0xFA: 01     (10/10 ✅)
  0xC2: 08     (10/10 ✅)
  0x51: 2f34   (10/10 ✅)
```

### 3.3 关键结论

- **HS 读模式整体可用**，偶发失败在 vismpwr 重试机制下可透明处理
- **0x2F、0xC2 等 page 0 寄存器值与初始化序列一致**
- 0xFA 写入 07 回读 01，可能为只写寄存器或硬件自动清除
- 0x51（亮度寄存器）读数稳定在 2f30~2f34 范围

## 四、已知限制

1. **LP 读在 VIDEO 模式下不可用。** MIPI DSI 协议约束，BTA 在视频流中无法完成。
2. **HS 读时序敏感。** 偶见首次超时（-110），约 20% 失败率。
3. **不可 VIDEO↔CMD 模式切换。** 面板不支持运行时模式切换，会破坏显示流水线。
4. **RK 官方到 6.6 内核均未解决此问题。** 当前修复为首次实现。

## 五、关联文档

| 文档 | 路径 | 说明 |
|------|------|------|
| HS 读修复报告 | `vis/video-mode-hs-read-fix-report.md` | HS 读修复实现细节 |
| 回读调查记录 | (openclaw 报告) | TRM 寄存器级分析 |
| 驱动源码 | `drivers/gpu/drm/rockchip/dw-mipi-dsi2-rockchip.c` | 驱动实现 |
| 测试工具 | `vis/vismpwr.c` | 用户态 DSI 读写工具 |
