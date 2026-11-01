# 验证记录

## 已验证

- Linux：GCC/Clang 构建、CTest 与 sanitizer 测试通过。
- Windows：MinGW x64 交叉构建通过；Wine 可启动 daemon。Windows 原生设备尚未测试。
- Android：四种 ABI 的 Release AAR 构建通过；尚未在 Android 真机安装验收。
- 真机：Linux、CH340、RD6006 衍生型号 60067、115200。验证了 121 寄存器读取、连续采集、多订阅者、缓存复用，以及 HR8/9/72 和 HR8–9 同值写入。

真机热采样间隔中位数约 144 ms。4 客户端并发写时，新调度的采样间隔 P95 约 287 ms，40 次写入无通信失败或序号缺口。写入测试只写回原始值，验证了协议确认与读回一致性，不代表改值后的设备功能已验证。详见[读写调度测试](measurements/linux-c-rw-benchmark.md)。

真机测试与 PTY 集成测试分开进行：`ctest` 中的 `daemon-pty` 用伪终端模拟设备，覆盖全部内置型号的缩放与范围、`--model-profile`、HR72 限制、0x06/0x10 写入、CRC 恢复和退出流程，不需要真机。上述结果不构成长期可靠性或硬实时保证。

## 尚未验证

- 其他型号的语义寄存器范围、缩放和实际写入行为。
- Windows 原生串口及 Service 生命周期；Android USB 授权、拔插和前台服务生命周期。
- 断线、超时和坏帧下的恢复延迟上限及持续队列压力。
- SPP/BLE 协议透传、UUID、分包重组、重连和吞吐；蓝牙传输尚未实现。
- 设备内部 ADC 刷新率；主机采样间隔不等于 ADC 转换间隔。
