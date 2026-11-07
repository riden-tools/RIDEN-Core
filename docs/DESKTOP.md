# 桌面端编程输出集成

[RIDEN-Desktop](https://github.com/riden-tools/RIDEN-Desktop) 0.2.1 提供基于 Blockly 的 Scratch 风格画布，支持多设备、实时条件、循环、列表、计时器及自定义积木。此功能使用现有 Core API，不增加 Core 协议命令，不更改 I/O 调度器或 Core 版本号。

## 数据与控制

- 每台电源一个 daemon、一个串口 worker。Desktop 使用 `status` 模式订阅 HR4–19，普通条件从内存中的最新样本读取；样本超过 1.5 s 未刷新时，Desktop 终止程序并尝试关闭绑定设备输出。
- 设备条件和日志不会逐次触发串口读取。需要确认输出状态、电池电压或数据组载入时才提交显式 `read`；`max_age_ms:0` 排除提交前已有缓存，但可共享进行中的读取，见 [API](API.md)。
- 电压、电流分别写 HR8、HR9；输出写 HR18。程序结束、停止或失败后，Desktop 尝试关闭所有绑定输出，并回读 HR18 确认；断线时不能保证关断。
- 切换 M1–M9 写 HR19，保存四项参数写 HR[80 + 组编号 × 4] 起的四个寄存器。即时 OVP / OCP 应用写 HR82/83，同时影响 M0 存储；不要为刷新保护值重新载入 HR19，因为它也会重载设定电压和电流。

## 电池电压

HR33 是电池电压读数，HR10 是输出端测量电压，两者不能无条件互换。用户在 RD6006 上反馈：连接 3.09 V 的 1S 电池时，界面 HR10 显示 3.09 V，而读取 HR33 得到零。

Desktop 0.2.1 的识别流程优先读 HR32/33；当 HR33 为零且输出关闭时，可用新鲜读取的 HR10 端电压。读取 HR10–18，使用同次响应中的 HR18 检查输出关闭；按型号电压缩放换算。连续三次采样和用户确认后的复核固定同一读数来源，并向用户显示来源。不会通过开启输出来识别电池。两处读数都为零时拒绝识别。

3.09 V 在示例每串 3.0–4.25 V 的候选窗口内，对应 1S；这是算法预期，不能替代用户核对电池标签。示例要求每次填写限流，确认串数后才设置目标电压并开启输出。充电程序的终止条件及范围见 [Desktop 编程说明](https://github.com/riden-tools/RIDEN-Desktop/blob/main/docs/PROGRAMMING.md)。

## 分发与验证边界

Desktop 安装包分别内置 `bin/riden-cored` + `lib/libriden_core.so`（Linux），或 `bin/riden-cored.exe` + `bin/libriden_core.dll`（Windows）；用户无需单独安装 Core。源码通过固定提交的 `core/` 子模块构建。

本次同步仅补充 Core 文档，不修改 Core 实现，未重新运行测试或真机操作。Desktop 的 HR10 回退读取、弹窗层级和 Linux 图标修复交由用户人工验收；不要将既往 PTY 模拟测试、Windows 交叉构建或 Wine 启动结果视为此次电池充电修复的真机验证。
