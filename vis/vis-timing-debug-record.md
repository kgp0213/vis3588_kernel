# RK3588 `vis-timing.bin` 加载与调试记录

## 1. 背景

在 `kernel-5.10` 源码中，需要确认系统启动时如何从 `vis-timing.bin` 读取屏幕参数，并覆盖设备树中的显示配置。同时，需要增强 dmesg 调试能力，便于确认：

- bin 是否成功加载
- 实际加载到的 timing 参数是什么
- init / exit 命令序列具体内容是什么
- DSI lane / format / flags 是否来自 bin

源码目录：

```bash
/home/vis/vis3588/kernel-5.10
```

---

## 2. 相关源码位置

### 2.1 `panel-simple.c`
文件：

```bash
drivers/gpu/drm/panel/panel-simple.c
```

这是主加载路径，负责读取：

- timing
- init_seq
- exit_seq
- other_info（DSI flags / format / lanes）

关键函数：

```c
panel_simple_of_get_firmware_desc_data_binfile()
```

---

### 2.2 `dw-mipi-dsi2-rockchip.c`
文件：

```bash
drivers/gpu/drm/rockchip/dw-mipi-dsi2-rockchip.c
```

这里也读取同一个文件：

```c
/lib/firmware/vis-timing.bin
```

主要用于读取：

- DSC 参数
- 部分 init sequence 数据

---

## 3. 启动时加载逻辑

在 `panel-simple.c` 中流程如下：

1. 检测防呆 GPIO（`no-dsc-gpio`，默认 fallback GPIO108）
2. 如果 GPIO 条件满足，则跳过 bin 加载
3. 否则尝试加载：

```c
/lib/firmware/vis-timing.bin
```

4. 如果加载成功，则使用 bin 中的参数
5. 如果加载失败，则回退到设备树配置

对应逻辑：

```c
err = panel_simple_of_get_firmware_desc_data_binfile(fw_path, dev, &desc->desc, desc);
if (!err) {
    dev_info(dev, "Using panel parameters from firmware\n");
} else {
    dev_info(dev, "not found firmware or Bad firmware,use device-tree config\n");
    // fallback 到设备树
}
```

---

## 4. `vis-timing.bin` 文件格式

### 4.1 Header 结构

在 `panel-simple.c` 中定义：

```c
struct entry {
    u32 offset;
    u32 length;
};

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
```

说明：

- header 里不直接放数据内容
- 各 section 用 `offset + length` 指向

---

### 4.2 各 section 含义

#### timing_entry
内容为 `struct videomode`，包含：
- pixelclock
- hactive / vactive
- hfront_porch / hback_porch / hsync_len
- vfront_porch / vback_porch / vsync_len
- flags

---

#### init_seq_entry
内容为初始化命令流。单条命令格式：

```c
struct panel_cmd_header {
    u8 data_type;
    u8 delay;
    u8 payload_length;
} __packed;
```

后面跟 `payload_length` 字节 payload。

---

#### eixt_seq_entry
退出 / 关屏命令流，格式与 init_seq 相同。

---

#### other_info_entry
在代码里实际解析出：

```c
dsi_flags = (other_info_data[1] << 8) | other_info_data[0];
format_type = other_info_data[4];
lanes = other_info_data[5];
```

映射关系：
- `format_type = 0` → RGB888
- `format_type = 1` → RGB666
- `format_type = 2` → RGB565

---

#### vesa_dsc_entry
在 `dw-mipi-dsi2-rockchip.c` 中解析：
- c_option
- scrambling_en
- dsc_enable
- version_major/minor
- slice_width / slice_height

---

## 5. 关于 lane 数量的确认

在 `panel-simple.c` 中，lane 数量来自：

```c
lanes = other_info_data[5];
desc_dsi->lanes = lanes;
```

结论：

- `vis-timing.bin` 中确实包含 lane 数
- 会覆盖设备树中的 `dsi,lanes`

---

## 6. dmesg 调试改动过程

### 6.1 初始问题
原始代码中，从 bin 读取到的内容打印不完整，尤其是：
- timing 详细参数
- init_seq / exit_seq
- 完整 payload

导致仅靠 dmesg 很难确认到底加载了什么。

---

### 6.2 最终采用的方案
最终调试改动采用以下原则：

- **在 `panel_simple_parse_cmd_seq()` 中打印命令流**
- 不仅 bin 的 init_seq / exit_seq 能打印
- device-tree 里的命令流也能统一打印
- 日志前缀统一使用：

```text
[mipi-timing]
```

---

### 6.3 打印内容
在 `panel_simple_parse_cmd_seq()` 中打印：

- raw_len
- cmd_cnt
- 每条命令的：
  - `data_type`
  - `delay`
  - `payload_len`
  - `payload` 完整 hex 内容

异常情况打印：
- truncated payload
- trailing bytes

---

### 6.4 grep 方式
调试时建议使用：

```bash
dmesg | grep "\[mipi-timing\]"
```

只看命令：

```bash
dmesg | grep "\[mipi-timing\] cmd"
```

只看 payload：

```bash
dmesg | grep "\[mipi-timing\].*payload:"
```

---

## 7. 对 `/home/vis/Documents/2lane.bin` 的检查结果

尝试按 `panel-simple.c` 的 `firmware_header` 结构解析：

结果：

```text
magic=0xaa440000
firmware_size=0
other_off=0
other_len=0
```

说明：

- `2lane.bin` 不符合当前 `vis-timing.bin` 的格式
- 不能直接按 `panel-simple.c` 里的结构解析
- 因此无法用同样的方法可靠提取 lane 参数

---

## 8. 对 `/home/vis/Documents/vis-timing.bin` 的完整解读

### 8.1 文件基本信息

文件：

```bash
/home/vis/Documents/vis-timing.bin
```

解析结果：

- **magic** = `0xA5A55A5A`
- **vendor** = `Visonox890123456`
- **model** = `DSI-Panel0123456`
- **version** = `1.234567`
- **firmware_size** = `233`

---

### 8.2 section 布局

| section | offset | length |
|---|---:|---:|
| timing | 96 | 48 |
| init_seq | 144 | 49 |
| exit_seq | 193 | 8 |
| touchscreen | 0 | 0 |
| vesa_dsc | 201 | 16 |
| other_info | 217 | 16 |

---

### 8.3 timing 参数

解析得到：

- **pixelclock** = `28270000 Hz`
- **hactive** = `900`
- **hfront_porch** = `24`
- **hback_porch** = `24`
- **hsync_len** = `4`
- **vactive** = `960`
- **vfront_porch** = `8`
- **vback_porch** = `6`
- **vsync_len** = `2`
- **flags** = `0x55`

计算结果：

- **分辨率** = `900 x 960`
- **HTOTAL** = `952`
- **VTOTAL** = `976`
- **刷新率** ≈ `30.43 Hz`

---

### 8.4 init_seq 解析

完整命令序列如下：

```text
cmd[0] dtype=0x39 delay=0 plen=2 payload=fe00
cmd[1] dtype=0x39 delay=0 plen=2 payload=3500
cmd[2] dtype=0x39 delay=0 plen=2 payload=5320
cmd[3] dtype=0x39 delay=0 plen=2 payload=51ff
cmd[4] dtype=0x39 delay=0 plen=2 payload=63ff
cmd[5] dtype=0x39 delay=0 plen=5 payload=2a0000007f
cmd[6] dtype=0x39 delay=0 plen=5 payload=2b0000018f
cmd[7] dtype=0x05 delay=120 plen=1 payload=11
cmd[8] dtype=0x05 delay=0 plen=1 payload=29
```

压缩写法：

```text
39 00 02 FE 00
39 00 02 35 00
39 00 02 53 20
39 00 02 51 FF
39 00 02 63 FF
39 00 05 2A 00 00 00 7F
39 00 05 2B 00 00 01 8F
05 78 01 11
05 00 01 29
```

---

### 8.5 init_seq 的语义观察

其中比较像标准 MIPI DCS 的命令包括：

- `35 00`
- `53 20`
- `51 FF`
- `63 FF`
- `2A 00 00 00 7F`
- `2B 00 00 01 8F`
- `11`（sleep out）
- `29`（display on）

注意点：

- `2A` 地址窗口对应 `0x0000 ~ 0x007F`
- `2B` 地址窗口对应 `0x0000 ~ 0x018F`

这与 timing 区解析出的 `900x960` 并不一致。  
这一点需要重点关注，说明：
- init_seq 里的窗口设置不一定代表最终全屏参数
- 或者这份 bin 不是严格匹配的一整套量产参数
- 也可能是某种测试/中间版本

---

### 8.6 exit_seq 解析

完整命令：

```text
cmd[0] dtype=0x05 delay=120 plen=1 payload=28
cmd[1] dtype=0x05 delay=0 plen=1 payload=10
```

压缩写法：

```text
05 78 01 28
05 00 01 10
```

语义上很典型：

- `28` → display off
- 延时 120ms
- `10` → enter sleep mode

---

### 8.7 other_info 解析

原始数据：

```text
000a0000000200566973000000000000
```

解析结果：

- **dsi_flags** = `0x0A00`
- **format_type** = `0`
- **lanes** = `2`

映射：

- **format** = `RGB888`
- **lane 数** = `2`

---

### 8.8 vesa_dsc 解析

原始数据：

```text
0000000101000000d002000041000000
```

解析结果：

- **c_option** = `0`
- **scrambling_en** = `0`
- **dsc_enable** = `0`
- **version** = `1.1`
- **slice_width** = `720`
- **slice_height** = `65`

结论：

- bin 中 **DSC 未启用**
- 但预留了 DSC 参数区

---

## 9. 本次结论

### 9.1 `vis-timing.bin` 包含的关键参数
该 bin 文件实际包含：

- 屏幕时序（resolution / porch / sync / pixelclock）
- 初始化命令（init_seq）
- 退出命令（exit_seq）
- DSI flags / format / lanes
- DSC 参数

---

### 9.2 当前样例 `vis-timing.bin` 的核心内容

#### 显示时序
- 分辨率：`900 x 960`
- Pixel Clock：`28.27 MHz`
- HFP / HBP / HSYNC：`24 / 24 / 4`
- VFP / VBP / VSYNC：`8 / 6 / 2`
- 刷新率：约 `30.43 Hz`

#### DSI
- 格式：`RGB888`
- Lane：`2`
- Flags：`0x0A00`

#### DSC
- `dsc_enable = 0`

#### Init Code
```text
39 00 02 FE 00
39 00 02 35 00
39 00 02 53 20
39 00 02 51 FF
39 00 02 63 FF
39 00 05 2A 00 00 00 7F
39 00 05 2B 00 00 01 8F
05 78 01 11
05 00 01 29
```

#### Exit Code
```text
05 78 01 28
05 00 01 10
```

---

## 10. 备注

1. 这次调试中曾经有一版提交直接在 bin 加载路径里增加大量打印，后续已按要求撤销。
2. 最终认可的方向是：
   - 在 `panel_simple_parse_cmd_seq()` 中打印完整命令
   - 日志前缀统一为 `[mipi-timing]`
3. 后续如果要进一步区分“来自 bin”还是“来自 device-tree”，可以再增加来源标记。
