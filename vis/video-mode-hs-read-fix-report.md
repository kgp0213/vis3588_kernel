# RK3588 DSI2 Video 模式 HS 读修复报告

## 一、背景

### 1.1 平台

| 项目 | 详情 |
|------|------|
| SoC | Rockchip RK3588 |
| 板卡 | vis-3588 8K Platform，Ubuntu 20.04 LTS |
| 内核 | Linux 5.10.209-rk3588 |
| CPU | 4×Cortex-A55 + 4×Cortex-A76，8 核 |
| 屏幕 | MIPI DSI、1260×2800、4-lane DPHY、panel-dsi-vis |

### 1.2 问题

video 模式下通过 `/dev/mipi_dsi0` 读取 DCS 寄存器失败：

```
DSI-CMD dt=0x06 tx_mode=LP payload=[51] ret=-110
[drm:dw_mipi_dsi2_transfer] *ERROR* CRI has no available read data
```

| 操作 | 现象 |
|------|------|
| LP 读 | 始终失败，无数据返回 |
| LP 写 | 命令无报错，但面板无反应 |
| HS 写 | 正常工作，亮度变化可感知 |

> 用户确认：面板不支持 video↔command 模式运行时切换，只支持 video 模式下的 HS 回读。

---

## 二、显示模式

通过 DTBO overlay 切换。修改 `/boot/uEnv/uEnv.txt` 后重启：

```
# CMD 模式
dtoverlay =/dtb/overlay/vis-dsi0-vp2-cmd-4lane-m559.dtbo

# Video 模式
dtoverlay =/dtb/overlay/vis-dsi0-vp2-video-4lane-m680.dtbo
```

| 参数 | CMD 模式 | Video 模式 |
|------|----------|-----------|
| 刷新率 | ~47 Hz | 60 Hz |
| 像素时钟 | 180.6 MHz | 260.53 MHz |
| DSI 带宽 | 4,334 Mbps | 6,947 Mbps |
| `dsi,flags` | `0xa00` | `0xa03`（来自 firmware） |

---

## 三、根因

### 3.1 数据通路

```
VOP2 → IPI → DSI2 Host Controller → DPHY → Panel
```

video 模式下 VOP 持续通过 IPI 推送像素，DSI 链路始终处于 HS 状态。LP 命令所需的 BTA（Bus Turn Around，总线翻转）无法在消隐期内完成。

### 3.2 内核 Bug（`dw-mipi-dsi2-rockchip.c`）

**函数：** `dsi_raw_write()` — `/dev/mipi_dsiX` 的写处理函数。

**Bug 1 — DCS_READ 硬编码 LP：**

```c
msg.flags = MIPI_DSI_MSG_USE_LPM;  // 始终 LP，无视用户 -hs 标记
```

即使用户要求 HS 模式，读命令仍被强制 LP 发送。video 模式下 LP 命令到不了面板。

**Bug 2 — 5 字节 HS 读命令解析错误：**

4 字节头格式为 `[data_type, delay, payload_len, flags]`。5 字节 HS 读命令 `[0x06, 0x00, 0x02, 0x00, 0x51]` 中 `cmd_buf[2]=2` 恰好等于 `count-3=2`，被错误匹配为 legacy 3 字节头，导致 payload 指向 `cmd_buf[3]=0x00`（flags）而不是寄存器地址 `0x51`。

**Bug 3 — `SET_MAXIMUM_RETURN_PACKET_SIZE` 硬编码 LP：**

```c
if (msg.type == MIPI_DSI_SET_MAXIMUM_RETURN_PACKET_SIZE)
    msg.flags = MIPI_DSI_MSG_USE_LPM;  // 同样无视 HS 标记
```

---

## 四、修复

### 4.1 内核修改（3 处）

#### 修改 1：Header 解析优先

```c
if (cmd_buf[0] == MIPI_DSI_DCS_READ && count == 5) {
    header_len = 4;
    legacy_hdr = false;
}
```

#### 修改 2：payload 指针 + 取消 LP 硬编码

```c
msg.tx_buf = &cmd_buf[count - 1];           // 取最后一个字节作寄存器地址
msg.flags = legacy_hdr ? MIPI_DSI_MSG_USE_LPM :
            (hdr->flags & 0x1) ? MIPI_DSI_MSG_USE_LPM : 0;
```

#### 修改 3：`0x37` 不再硬编码 LP

```c
msg.flags = legacy_hdr ? MIPI_DSI_MSG_USE_LPM :
            (hdr->flags & 0x1) ? MIPI_DSI_MSG_USE_LPM : 0;
```

**flags 字节（bit0）约定：** `0`=HS、`1`=LP。

### 4.2 vismpwr 修改

`read_register()` 新增 HS 分支：

```c
if (cmd->use_lpm) {
    // 3 字节头 LP：{0x37, 0x00, 0x02, read_len, 0x00}
    //              {0x06, 0x00, read_len, reg}
} else {
    // 4 字节头 HS：{0x37, 0x00, 0x02, 0x00, read_len, 0x00}
    //              {0x06, 0x00, read_len, 0x00, reg}
}
```

### 4.3 外部调研

RK 官方 `rockchip-linux/kernel` 各分支（5.10、6.1、6.6）、主线 Linux、LubanCat 等下游仓库均未解决此问题。本修复为首次实现。

---

## 五、命令格式拆解

### HS 读（`vismpwr -r -hs 02 51`）

```
set_max_pkt (0x37):
  0x37  0x00  0x02  0x00  0x02  0x00
 ┬──── ┬──── ┬──── ┬──── ┬──────────
 │     │     │     │     └ payload (read_len=2, 0x00)
 │     │     │     └ flags=0 → HS
 │     │     └ payload_len=2
 │     └ delay
 └ dt=0x37

DCS_READ (0x06):
  0x06  0x00  0x02  0x00  0x51
 ───── ───── ───── ───── ─────
 dt    delay rx_len flags reg_addr
                   =HS
```

内核解析：
- `header_len = 4`、`legacy_hdr = false`
- `tx_buf = &buf[4] = [0x51]` ✓
- `flags = (0x00 & 0x1) ? LP : 0 = 0 → HS` ✓

### LP 读（`vismpwr -r 02 51`，行为不变）

```
0x37  0x00  0x02  0x02  0x00         5 字节 3 字节头 → legacy → LP
0x06  0x00  0x02  0x51               4 字节 3 字节头 → legacy → LP
```

CMD 模式下正常工作。

---

## 六、测试结果

### Video 模式

| 操作 | 命令 | 结果 |
|------|------|------|
| HS 读 | `-r -hs 02 51` | `2f 34` ✓ |
| HS 写 | `-hs 39 00 03 51 02 00` + HS 读回 | `02 00` 一致 ✓ |
| LP 读 | `-r 02 51` | 失败（预期） |
| `0x37` 模式 | — | `tx_mode=HS` ✓ |

### CMD 模式

LP/HS 全线正常，无回归。

### 典型日志

```
DSI-CMD dt=0x37 payload_len=2 tx_mode=HS payload=[02 00]
DSI-CMD dt=0x06 payload_len=1 tx_mode=HS payload=[51]
```

---

## 七、局限性

1. ***LP 读在 video 模式下仍不可用。** MIPI DSI 协议约束，BTA 在视频流中无法完成。
2. ***HS 读时序敏感。** 偶见首次超时（-110），vismpwr 内置 5 次重试可透明处理。

---

## 八、经验

1. **修改 DSI 驱动前必须理解整条硬件链路。** VOP→IPI→控制器→DPHY→Panel 构成流水线，任意一环不能随意暂停。
2. **HS 命令通道在 video 模式下正常工作。** 控制器可在像素包间隙插入 HS 命令包，面板正常响应——是 video 模式下寄存器读写的正确途径。
3. **RK 官方到 6.6 内核均未解决此问题。** 本修复为首次实现。
