# `vis-timing.bin` 协议文档（基于当前内核实现）

> 说明：本协议文档是根据当前源码实现反推整理，主要依据：
>
> - `drivers/gpu/drm/panel/panel-simple.c`
> - `drivers/gpu/drm/rockchip/dw-mipi-dsi2-rockchip.c`
>
> 结论适用于当前项目里的 bin 生成与解析逻辑，不代表上游通用标准。

---

# 1. 文件用途

`vis-timing.bin` 用于在系统启动时向显示驱动提供一组可替代设备树的面板参数，主要包括：

- 显示时序参数（timing）
- 面板初始化命令（init sequence）
- 面板退出命令（exit sequence）
- DSI 参数（flags / format / lanes）
- DSC 参数（可选）

如果 bin 加载成功，则相关参数会优先使用 bin 中的数据；如果加载失败，则回退到设备树配置。

---

# 2. 适用代码路径

## 2.1 `panel-simple.c`
主要负责解析：

- timing_entry
- init_seq_entry
- eixt_seq_entry
- other_info_entry

核心函数：

```c
panel_simple_of_get_firmware_desc_data_binfile()
```

---

## 2.2 `dw-mipi-dsi2-rockchip.c`
主要负责解析：

- vesa_dsc_entry
- 部分 init sequence 数据

核心函数：

```c
dw_mipi_dsi2_get_dsc_params_from_firmware()
```

---

# 3. 文件整体布局

`vis-timing.bin` 由两部分组成：

1. 固定长度 Header
2. 多个数据区（section）

逻辑布局如下：

```text
+-------------------------+
| firmware_header         |
+-------------------------+
| timing_entry data       |
+-------------------------+
| init_seq_entry data     |
+-------------------------+
| eixt_seq_entry data     |
+-------------------------+
| touchscreen_entry data  |
+-------------------------+
| vesa_dsc_entry data     |
+-------------------------+
| other_info_entry data   |
+-------------------------+
```

每个 section 是否存在、位于哪里，由 header 中的 `offset + length` 指定。

---

# 4. Header 定义

## 4.1 `struct entry`

```c
struct entry {
    u32 offset;
    u32 length;
};
```

### 字段说明
| 字段 | 类型 | 含义 |
|---|---|---|
| offset | u32 | section 数据相对于文件起始位置的偏移 |
| length | u32 | section 数据长度（字节） |

---

## 4.2 `struct firmware_header`

```c
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

---

## 4.3 Header 字段说明

| 字段 | 类型 | 长度 | 说明 |
|---|---|---:|---|
| magic | u32 | 4 | 固件魔数，用于校验有效性 |
| vendor | u8[16] | 16 | 厂商字符串 |
| model | u8[16] | 16 | 型号字符串 |
| version | u8[8] | 8 | 版本字符串 |
| timing_entry | entry | 8 | timing 区描述 |
| init_seq_entry | entry | 8 | init sequence 区描述 |
| eixt_seq_entry | entry | 8 | exit sequence 区描述（代码里拼写即 `eixt`） |
| touchscreen_entry | entry | 8 | 触摸相关数据区 |
| vesa_dsc_entry | entry | 8 | DSC 数据区 |
| other_info_entry | entry | 8 | DSI 扩展参数区 |
| firmware_size | u32 | 4 | 固件总大小 |

---

## 4.4 Header 总长度

按当前结构计算：

- `magic` = 4
- `vendor` = 16
- `model` = 16
- `version` = 8
- 6 个 `entry` = 6 × 8 = 48
- `firmware_size` = 4

总长度：

```text
4 + 16 + 16 + 8 + 48 + 4 = 96 bytes
```

因此在当前实现中：

- **Header 大小固定为 96 字节**

---

# 5. 固件校验规则

在 `panel-simple.c` 中，基本校验逻辑为：

```c
if (header->magic != 0xA5A55A5A || header->firmware_size <= 0)
    return -EINVAL;

if (header->firmware_size < sizeof(*header))
    return -EINVAL;
```

### 当前要求
| 项目 | 要求 |
|---|---|
| magic | 必须为 `0xA5A55A5A` |
| firmware_size | 必须大于 0 |
| firmware_size | 不能小于 96（header 大小） |

---

# 6. timing_entry 数据格式

## 6.1 类型
`timing_entry` 数据区存储的是：

```c
struct videomode
```

在当前 arm64 平台上，`unsigned long` 为 8 字节，因此本项目样例中该结构大小为：

```text
48 bytes
```

---

## 6.2 字段顺序（按当前解析逻辑）

从当前实现反推，字段顺序为：

```c
unsigned long pixelclock;
u32 hactive;
u32 hfront_porch;
u32 hback_porch;
u32 hsync_len;
u32 vactive;
u32 vfront_porch;
u32 vback_porch;
u32 vsync_len;
u32 flags;
```

---

## 6.3 字段说明

| 字段 | 类型 | 说明 |
|---|---|---|
| pixelclock | unsigned long | 像素时钟，单位 Hz |
| hactive | u32 | 水平有效像素数 |
| hfront_porch | u32 | 水平前廊 |
| hback_porch | u32 | 水平后廊 |
| hsync_len | u32 | HSYNC 宽度 |
| vactive | u32 | 垂直有效行数 |
| vfront_porch | u32 | 垂直前廊 |
| vback_porch | u32 | 垂直后廊 |
| vsync_len | u32 | VSYNC 宽度 |
| flags | u32 | display flags |

---

## 6.4 驱动中的使用方式

读取后会执行：

```c
drm_display_mode_from_videomode(vm, mode);
drm_bus_flags_from_videomode(vm, &bus_flags);
```

并将其写入：

```c
desc->modes = mode;
desc->num_modes = 1;
desc->bus_flags = bus_flags;
```

因此 timing_entry 实际控制：
- 分辨率
- porch / sync
- 像素时钟
- bus flags

---

# 7. init_seq_entry / eixt_seq_entry 数据格式

## 7.1 单条命令头格式

```c
struct panel_cmd_header {
    u8 data_type;
    u8 delay;
    u8 payload_length;
} __packed;
```

每条命令编码格式：

```text
+------------+--------+----------------+----------------------+
| data_type  | delay  | payload_length | payload bytes[...]    |
+------------+--------+----------------+----------------------+
   1 byte      1 byte      1 byte         payload_length bytes
```

---

## 7.2 字段说明

| 字段 | 类型 | 说明 |
|---|---|---|
| data_type | u8 | MIPI DSI / SPI 命令类型 |
| delay | u8 | 该命令发送完成后的延时，单位 ms |
| payload_length | u8 | payload 数据长度 |
| payload | u8[] | 实际命令参数 |

---

## 7.3 序列组织方式

一个 sequence（init 或 exit）由多条命令顺序拼接组成，不带额外尾标记。

解析算法：
1. 读取 3 字节 header
2. 根据 `payload_length` 读取 payload
3. 移动到下一条命令
4. 直到数据区耗尽

若存在：
- payload 不完整
- 剩余字节无法构成完整命令头

则判定为非法。

---

## 7.4 驱动中的解析函数

```c
panel_simple_parse_cmd_seq(struct device *dev,
                           const u8 *data, int length,
                           struct panel_cmd_seq *seq)
```

解析后得到：

```c
struct panel_cmd_seq {
    struct panel_cmd_desc *cmds;
    unsigned int cmd_cnt;
};
```

---

## 7.5 常见命令类型示例

在当前项目样例中，常见 data_type 包括：

| data_type | 常见含义 |
|---|---|
| `0x39` | DCS Long Write / Generic Long Write |
| `0x05` | DCS Short Write |

具体语义仍需结合 MIPI DCS/DSI 定义和 payload 内容理解。

---

# 8. other_info_entry 数据格式

当前驱动只解析了其中部分字段。

## 8.1 已使用字段偏移

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 1 | dsi_flags low | DSI flags 低字节 |
| 1 | 1 | dsi_flags high | DSI flags 高字节 |
| 4 | 1 | format_type | 像素格式 |
| 5 | 1 | lanes | MIPI lane 数 |

解析代码：

```c
dsi_flags = (other_info_data[1] << 8) | other_info_data[0];
format_type = other_info_data[4];
lanes = other_info_data[5];
```

---

## 8.2 format_type 映射

| format_type | 映射值 |
|---:|---|
| 0 | `MIPI_DSI_FMT_RGB888` |
| 1 | `MIPI_DSI_FMT_RGB666` |
| 2 | `MIPI_DSI_FMT_RGB565` |
| 其他 | 默认回退到 `MIPI_DSI_FMT_RGB888` |

---

## 8.3 驱动中的使用结果

```c
desc_dsi->flags = dsi_flags;
desc_dsi->format = ...;
desc_dsi->lanes = lanes;
```

因此 other_info_entry 实际控制：
- DSI flags
- DSI pixel format
- DSI lane 数量

---

# 9. vesa_dsc_entry 数据格式

`dw-mipi-dsi2-rockchip.c` 中按固定偏移读取：

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 1 | c_option | DSC c_option |
| 1 | 1 | scrambling_en | 是否启用 scrambling |
| 2 | 1 | dsc_enable | 是否启用 DSC |
| 3 | 1 | version_major | DSC 主版本号 |
| 4 | 1 | version_minor | DSC 次版本号 |
| 8 | 4 | slice_width | 小端格式 |
| 12 | 4 | slice_height | 小端格式 |

解析代码逻辑：

```c
dsi2->c_option = dsc_data[0] != 0;
dsi2->scrambling_en = dsc_data[1] != 0;
dsi2->dsc_enable = dsc_data[2] != 0;
dsi2->version_major = dsc_data[3];
dsi2->version_minor = dsc_data[4];
dsi2->slice_width = get_unaligned_le32(&dsc_data[8]);
dsi2->slice_height = get_unaligned_le32(&dsc_data[12]);
```

---

# 10. touchscreen_entry

Header 中预留了：

```c
struct entry touchscreen_entry;
```

但在本次分析路径中未看到其在关键启动流程中的实际消费逻辑，因此：

- **当前协议中保留此 section**
- **是否生效取决于项目其他代码**

若不使用，可置：

- `offset = 0`
- `length = 0`

---

# 11. 当前样例 `vis-timing.bin` 解析示例

文件：

```bash
/home/vis/Documents/vis-timing.bin
```

---

## 11.1 Header

- magic = `0xA5A55A5A`
- vendor = `Visonox890123456`
- model = `DSI-Panel0123456`
- version = `1.234567`
- firmware_size = `233`

section 分布：

| section | offset | length |
|---|---:|---:|
| timing | 96 | 48 |
| init_seq | 144 | 49 |
| exit_seq | 193 | 8 |
| touchscreen | 0 | 0 |
| vesa_dsc | 201 | 16 |
| other_info | 217 | 16 |

---

## 11.2 timing

- pixelclock = `28270000 Hz`
- hactive = `900`
- hfront_porch = `24`
- hback_porch = `24`
- hsync_len = `4`
- vactive = `960`
- vfront_porch = `8`
- vback_porch = `6`
- vsync_len = `2`
- flags = `0x55`

计算：

- HTOTAL = `952`
- VTOTAL = `976`
- refresh ≈ `30.43 Hz`

---

## 11.3 init_seq

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

## 11.4 exit_seq

```text
05 78 01 28
05 00 01 10
```

---

## 11.5 other_info

原始数据：

```text
00 0A 00 00 00 02 00 56 69 73 00 00 00 00 00 00
```

解析：

- dsi_flags = `0x0A00`
- format_type = `0`
- lanes = `2`

映射后：

- format = RGB888
- lane = 2

---

## 11.6 vesa_dsc

原始数据：

```text
00 00 00 01 01 00 00 00 D0 02 00 00 41 00 00 00
```

解析：

- c_option = `0`
- scrambling_en = `0`
- dsc_enable = `0`
- version = `1.1`
- slice_width = `720`
- slice_height = `65`

---

# 12. 当前协议中需要重点注意的问题

## 12.1 `eixt_seq_entry` 拼写
代码中字段名是：

```c
eixt_seq_entry
```

不是 `exit_seq_entry`。生成工具若按字段名硬编码，需要注意保持一致。

---

## 12.2 timing 与 init_seq 可能不完全一致
当前样例中：

- timing 表示分辨率是 `900x960`
- init_seq 中 `2A/2B` 设置的地址窗口更像 `128x400`

说明：
- init_seq 中的窗口设置不一定直接等于 timing 分辨率
- 生成 bin 时，不能简单假设两者一定一致
- 需要结合面板协议确认命令含义

---

## 12.3 平台相关性
`timing_entry` 中使用了 `struct videomode`，其大小依赖平台 ABI，尤其：

- `unsigned long pixelclock`

在 arm64 平台上通常为 8 字节。若 bin 生成工具与目标平台结构体布局不一致，可能导致 timing 解析错误。

因此建议：

- 生成工具固定采用当前内核结构布局
- 或在协议层面彻底改成显式定长字段，而不是直接二进制镜像 `struct videomode`

---

# 13. bin 生成建议

## 13.1 最低要求
生成 `vis-timing.bin` 时至少应保证：

- magic 正确：`0xA5A55A5A`
- firmware_size 正确
- 各 section 的 offset / length 正确
- timing_entry 长度与目标平台 `struct videomode` 一致
- init_seq / exit_seq 数据可被 `panel_simple_parse_cmd_seq()` 正常解析
- other_info 至少包含：
  - `other_info[0..1]` = dsi_flags
  - `other_info[4]` = format_type
  - `other_info[5]` = lanes

---

## 13.2 推荐校验项
建议在生成工具中增加：

1. Header 长度校验
2. 各 section 越界检查
3. `firmware_size == 文件总长度`
4. init/exit 命令流完整性检查
5. timing 参数范围检查
6. other_info 最小长度检查（至少 6 字节）

---

# 14. dmesg 调试建议

当前项目已将命令解析日志放到：

```c
panel_simple_parse_cmd_seq()
```

日志前缀：

```text
[mipi-timing]
```

建议使用：

```bash
dmesg | grep "\[mipi-timing\]"
```

只看 payload：

```bash
dmesg | grep "\[mipi-timing\].*payload:"
```

---

# 15. 后续可扩展方向

如后续继续演进该协议，建议考虑：

1. 在 header 中增加协议版本号（单独字段，不依赖 version 字符串）
2. timing 改为显式字段编码，避免直接镜像 `struct videomode`
3. other_info 增加更完整的字段说明文档
4. touchscreen_entry 明确结构定义
5. 增加 CRC / checksum 提高可靠性
6. 区分 init_seq 来源（bin / DT）并在日志中标注

---

# 16. 简要结论

`vis-timing.bin` 在当前项目里并不是单纯的 timing 文件，而是一个完整的 **panel parameter firmware**，至少承载：

- 时序参数
- 初始化命令
- 退出命令
- DSI flags / format / lanes
- DSC 参数

当前样例文件的核心参数为：

- 分辨率：`900x960`
- Pixel Clock：`28.27 MHz`
- 刷新率：约 `30.43 Hz`
- DSI 格式：`RGB888`
- Lane：`2`
- DSC：未启用

因此在调试显示异常时，应同时检查：
- timing
- init sequence
- other_info（尤其 lane / format）
- 是否被 GPIO 逻辑跳过
