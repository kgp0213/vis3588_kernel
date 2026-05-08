# `vis-timing.bin` 生成工具开发说明

> 面向对象：
>
> - 上位机工具开发者
> - 产测工具开发者
> - 面板参数编辑器开发者
>
> 目标：
>
> 提供一份工程化说明，指导如何正确生成可被当前内核识别的 `vis-timing.bin` 文件。

---

# 1. 目标与职责

`vis-timing.bin` 生成工具的职责不是简单拼接一个二进制文件，而是生成一份能被内核启动阶段稳定解析、正确覆盖设备树屏幕参数的 **panel parameter firmware**。

生成工具至少应支持输出以下信息：

- 面板厂商/型号/版本
- timing 参数
- init sequence
- exit sequence
- DSI 参数（flags / format / lanes）
- DSC 参数（可选）

并且需要保证：

- header 合法
- section offset/length 正确
- 数据不会越界
- 命令序列可被 `panel_simple_parse_cmd_seq()` 正常解析

---

# 2. 对应内核消费者

生成工具输出的 bin 目前由以下两个驱动消费：

## 2.1 `panel-simple.c`
负责读取：
- timing_entry
- init_seq_entry
- eixt_seq_entry
- other_info_entry

## 2.2 `dw-mipi-dsi2-rockchip.c`
负责读取：
- vesa_dsc_entry

因此，生成工具输出的数据至少要同时兼容这两部分解析逻辑。

---

# 3. 生成目标文件结构

## 3.1 文件总体布局

推荐的输出顺序：

```text
[firmware_header]
[timing_entry data]
[init_seq_entry data]
[eixt_seq_entry data]
[touchscreen_entry data 可选]
[vesa_dsc_entry data 可选]
[other_info_entry data]
```

说明：
- 当前驱动不要求 section 顺序必须固定
- 但生成工具建议使用固定顺序，便于调试和 hexdump 分析

---

## 3.2 Header 固定大小

当前协议下 header 固定为：

```text
96 bytes
```

因此建议：
- section 数据区从 offset = 96 开始排布
- 所有 offset 都以文件起始为基准

---

# 4. Header 构造要求

## 4.1 结构定义

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

---

## 4.2 必填字段

### magic
固定写入：

```text
0xA5A55A5A
```

若 magic 不正确，驱动会直接判定为非法固件。

---

### vendor
- 固定 16 字节
- 建议使用 ASCII
- 不足部分补 `0x00`
- 超过 16 字节必须截断

示例：

```text
"Visonox890123456"
```

---

### model
- 固定 16 字节
- 建议使用 ASCII
- 不足补 0
- 超长截断

示例：

```text
"DSI-Panel0123456"
```

---

### version
- 固定 8 字节
- 建议使用 ASCII
- 不足补 0
- 超长截断

示例：

```text
"1.234567"
```

---

### firmware_size
- 必须等于最终文件总长度
- 驱动会做基本合法性检查

---

# 5. timing_entry 生成说明

## 5.1 数据来源

建议上位工具从结构化参数输入：

- pixelclock
- hactive
- hfront_porch
- hback_porch
- hsync_len
- vactive
- vfront_porch
- vback_porch
- vsync_len
- flags

而不是让用户直接编辑裸二进制。

---

## 5.2 当前实现注意事项

当前驱动期望 `timing_entry` 直接对应目标平台的 `struct videomode` 内存布局。

这意味着生成工具必须考虑 ABI：

- `unsigned long pixelclock` 在 arm64 上为 8 字节
- 当前项目下 timing 区总长度应为 **48 字节**

如果生成工具在 PC 侧直接按错误布局打包，内核会读错字段。

---

## 5.3 推荐生成方式

推荐不要“依赖宿主机 C 结构体布局”，而是明确按固定小端字段顺序写入：

```text
pixelclock      u64 little-endian
hactive         u32 little-endian
hfront_porch    u32 little-endian
hback_porch     u32 little-endian
hsync_len       u32 little-endian
vactive         u32 little-endian
vfront_porch    u32 little-endian
vback_porch     u32 little-endian
vsync_len       u32 little-endian
flags           u32 little-endian
```

这样更稳定，也更方便跨平台工具实现。

---

## 5.4 timing 生成时建议做的自动校验

生成工具应检查：

1. `pixelclock > 0`
2. `hactive / vactive > 0`
3. porch / sync 不应为负数（无符号字段）
4. `htotal = hactive + hfp + hbp + hsync` 不能为 0
5. `vtotal = vactive + vfp + vbp + vsync` 不能为 0
6. 自动计算刷新率，提示用户确认

建议 UI 中显示：

- 分辨率
- 时钟
- 计算刷新率
- htotal / vtotal

---

# 6. init_seq / exit_seq 生成说明

## 6.1 单条命令格式

每条命令在 bin 中编码为：

```text
[data_type:1][delay:1][payload_length:1][payload:N]
```

因此生成工具在内部可以使用如下抽象：

```json
{
  "data_type": 57,
  "delay": 0,
  "payload": [0x35, 0x00]
}
```

然后自动生成：

```text
39 00 02 35 00
```

---

## 6.2 建议的输入形式

建议支持两种输入方式：

### 方式 A：结构化表格输入
字段：
- type
- delay
- payload（hex string）

### 方式 B：逐行文本输入
例如：

```text
39 00 02 FE 00
39 00 02 35 00
05 78 01 11
05 00 01 29
```

工具负责检查：
- 每行至少 3 字节
- 第 3 字节等于 payload 实际长度

---

## 6.3 生成时的强校验

建议生成工具对每条命令做：

1. `payload_length == payload 实际字节数`
2. `delay` 范围 0~255
3. `data_type` 合法性检查（可选）
4. 整个命令流拼接后可完整反解析

生成完成后，建议再调用一次“本地反解析器”验证整个 sequence 是否能按驱动逻辑重新解析成功。

---

## 6.4 调试友好建议

为了便于后续 dmesg 比对，生成工具建议同时输出一份文本副本：

例如：

```text
init_seq.txt
exit_seq.txt
```

内容与 bin 中命令逐条对应。这样可以直接和内核日志对比。

---

# 7. other_info_entry 生成说明

## 7.1 当前驱动已使用字段

| 偏移 | 字段 | 说明 |
|---:|---|---|
| 0 | dsi_flags low | 低字节 |
| 1 | dsi_flags high | 高字节 |
| 4 | format_type | 格式类型 |
| 5 | lanes | lane 数 |

---

## 7.2 最小建议长度

虽然当前样例中 `other_info_entry.length = 16`，但从驱动解析角度，至少需要：

```text
6 bytes
```

但为了兼容现有实现，建议固定输出为：

```text
16 bytes
```

不足部分补 0。

---

## 7.3 format_type 映射

| format_type | 含义 |
|---:|---|
| 0 | RGB888 |
| 1 | RGB666 |
| 2 | RGB565 |

工具侧应避免直接让用户输入裸数字，建议提供下拉框：

- RGB888
- RGB666
- RGB565

再映射成对应值。

---

## 7.4 lanes 建议校验

lane 数建议限制在：

- 1
- 2
- 3
- 4

当前常见有效值通常为 2 或 4。

---

# 8. vesa_dsc_entry 生成说明

## 8.1 当前使用字段

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 1 | c_option |
| 1 | 1 | scrambling_en |
| 2 | 1 | dsc_enable |
| 3 | 1 | version_major |
| 4 | 1 | version_minor |
| 8 | 4 | slice_width |
| 12 | 4 | slice_height |

建议固定长度至少：

```text
16 bytes
```

---

## 8.2 DSC 未启用时的建议

如果产品不启用 DSC，建议：

- `dsc_enable = 0`
- 其他字段仍可填默认值
- 或全部清零后保留 16 字节区

---

# 9. touchscreen_entry 生成建议

当前主流程中未观察到强依赖使用，因此工具中建议：

- 支持“禁用 touchscreen section”
- 禁用时：
  - offset = 0
  - length = 0

若后续项目引入触摸相关配置，再单独扩展该 section 定义。

---

# 10. section 排布策略建议

## 10.1 推荐排布顺序

建议始终按以下顺序连续排布：

1. header（96 bytes）
2. timing
3. init_seq
4. exit_seq
5. touchscreen
6. vesa_dsc
7. other_info

优点：
- hexdump 易读
- 排查 offset 错误方便
- 和当前工程样例一致性更高

---

## 10.2 offset 计算规则

假设：

- header_size = 96
- timing_len = A
- init_len = B
- exit_len = C
- touch_len = D
- dsc_len = E
- other_len = F

则：

```text
timing.offset = 96
init.offset   = 96 + A
exit.offset   = 96 + A + B
touch.offset  = 96 + A + B + C
dsc.offset    = 96 + A + B + C + D
other.offset  = 96 + A + B + C + D + E
firmware_size = 96 + A + B + C + D + E + F
```

如果某 section 长度为 0，建议：
- offset 也写 0
- length 写 0

不要给空 section 分配无意义偏移。

---

# 11. 推荐生成流程

建议 bin 生成工具采用如下流程：

## 步骤 1：接收结构化输入
输入模型建议包含：
- 基本信息（vendor/model/version）
- timing 参数
- init_seq 列表
- exit_seq 列表
- DSI 参数
- DSC 参数

---

## 步骤 2：构造各 section 原始二进制
分别生成：
- timing_blob
- init_blob
- exit_blob
- touch_blob
- dsc_blob
- other_blob

---

## 步骤 3：计算 offset / length
按固定顺序计算所有 entry。

---

## 步骤 4：构造 header
填入：
- magic
- vendor/model/version
- 各 entry
- firmware_size

---

## 步骤 5：拼接成最终 bin
最终输出：

```text
header + timing + init + exit + touch + dsc + other
```

---

## 步骤 6：本地反解析校验
生成完成后，必须用“反解析器”重新检查：

- magic 正确
- firmware_size 正确
- 各 section 不越界
- timing 可解析
- init/exit 可逐条解析
- other_info 最小字段存在
- DSC 字段可读

若反解析失败，应直接报错，禁止导出。

---

# 12. 推荐校验清单

生成工具建议至少做以下校验：

## Header 级别
- [ ] magic == `0xA5A55A5A`
- [ ] firmware_size == 最终文件大小
- [ ] 所有非空 section 都满足 `offset + length <= firmware_size`

## timing 级别
- [ ] timing 长度 == 48（当前 arm64 项目）
- [ ] 分辨率 > 0
- [ ] pixelclock > 0
- [ ] htotal/vtotal > 0
- [ ] 刷新率在合理范围内

## sequence 级别
- [ ] 每条命令至少有 3 字节头
- [ ] payload_length 正确
- [ ] 整个序列无 trailing bytes
- [ ] 可完整反解析

## other_info 级别
- [ ] 长度 >= 6
- [ ] format_type 合法
- [ ] lanes 合法

## DSC 级别
- [ ] 长度 >= 16（若启用）
- [ ] slice_width / slice_height 合理

---

# 13. 推荐导出附属文件

为了方便生产/调试，建议工具除导出 `.bin` 外，再同时导出：

1. `*.json` 参数镜像
2. `init_seq.txt`
3. `exit_seq.txt`
4. `report.txt` / `report.md`

其中 report 建议包含：
- timing 汇总
- 计算刷新率
- DSI 参数
- DSC 参数
- 各 section offset / length
- bin 文件 md5 / sha256

这样更适合产测和版本追踪。

---

# 14. 调试联动建议

当前内核侧已经可以在：

```c
panel_simple_parse_cmd_seq()
```

中打印命令流，并使用日志前缀：

```text
[mipi-timing]
```

因此推荐生成工具导出的文本命令格式，尽量与 dmesg 打印保持一致，便于人工对比。

建议调试流程：

1. 生成 bin
2. 保存参数副本与命令文本
3. 将 bin 放入 `/lib/firmware/vis-timing.bin`
4. 启动系统
5. 查看：

```bash
dmesg | grep "\[mipi-timing\]"
```

6. 比对工具导出的 init/exit 文本与 dmesg 输出是否一致

---

# 15. 当前样例参考值

当前工程已解析的样例 `vis-timing.bin` 关键值如下，可作为测试样板：

## timing
- resolution = `900x960`
- pixelclock = `28270000 Hz`
- hfp/hbp/hsync = `24 / 24 / 4`
- vfp/vbp/vsync = `8 / 6 / 2`
- refresh ≈ `30.43 Hz`

## DSI
- format = `RGB888`
- lanes = `2`
- dsi_flags = `0x0A00`

## init_seq
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

## exit_seq
```text
05 78 01 28
05 00 01 10
```

## DSC
- dsc_enable = `0`
- version = `1.1`
- slice = `720 x 65`

---

# 16. 已知风险与注意事项

## 16.1 `struct videomode` ABI 依赖
当前协议最大的工程风险之一是：

- `timing_entry` 不是显式协议字段
- 而是直接使用 `struct videomode` 的内存布局

这会导致：
- 不同平台/编译器/工具实现之间存在兼容风险

**建议长期方案**：
未来将 timing 区改成显式小端字段协议，而不是直接镜像内核结构体。

---

## 16.2 命令语义与 timing 可能不一致
init_seq 中部分命令（如 `2A/2B` 地址窗口）不一定直接等于 timing 分辨率。生成工具不应简单把两者绑定死。

建议：
- timing 和 init_seq 独立维护
- 允许人工确认
- 导出时提示潜在不一致风险

---

## 16.3 `eixt_seq_entry` 命名错误历史包袱
代码字段名拼写为：

```c
eixt_seq_entry
```

工具开发时要注意兼容这一命名，不要在依赖结构名反射时写错。

---

# 17. 建议的工具功能清单

一个较完整的 bin 生成工具建议具备：

- [ ] 图形界面或表格界面编辑 timing
- [ ] init/exit 命令编辑器
- [ ] hex 文本导入 / 导出
- [ ] 自动计算 offset / length
- [ ] 自动生成 firmware_size
- [ ] 本地反解析校验
- [ ] 生成调试报告
- [ ] 导出 bin + json + txt
- [ ] 项目模板管理（按面板型号保存）

---

# 18. 总结

`vis-timing.bin` 生成工具的核心目标不是“写出一个二进制文件”，而是要输出一份：

- 能被内核稳定解析
- 能覆盖 DTS 显示参数
- 能和 dmesg 调试日志一一对应
- 能方便版本追踪和产线回溯

的 **可维护固件文件**。

因此生成工具必须具备：
- 强校验
- 反解析验证
- 文本副本导出
- 结构化输入能力

这样才能降低屏幕 bring-up 和量产调试成本。
