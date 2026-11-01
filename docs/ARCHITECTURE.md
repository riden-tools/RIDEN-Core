# 通信与调度

## 结构

每台设备由一个 C worker 独占通信链路。daemon、C ABI 和 Android AIDL 共享同一缓存与任务队列；传输层提供连续字节读写。串口事务按顺序发送、收完整响应并验证地址、功能码和 CRC 后才结束。

```mermaid
flowchart LR
  Clients[daemon / C ABI / AIDL] --> Core[单设备 C 核心]
  Core --> Queue[有界读写队列与缓存]
  Queue --> Worker[单一 I/O worker]
  Worker --> RTU[Modbus RTU]
  RTU --> Transport[USB / 串口 / 后续蓝牙适配]
```

## 调度

- 默认热采集 HR10–13；`period_ms=0` 表示一笔事务完成后尽快继续，不设固定轮询延时。
- 一条链路同一时刻只运行一笔 RTU 事务。写入不能中断当前事务；热采集到期时优先恢复采样，等待至少 500 ms 的用户读也参与调度。
- 覆盖相同范围的缓存、进行中或排队读取可被复用。队列有界，不合并不相交读取。
- 通信错误会使缓存失效并触发排空、退避和型号重识别；写入不自动重试。
- 样本时间是主机完整收包时间，不代表设备 ADC 的转换时刻。订阅为最新值模式，慢客户端可能跳过样本。

当前 60067 真机在并发写时采样间隔 P95 约 287 ms；这不是硬实时上限。完整测量与边界见[验证记录](VALIDATION.md)和[读写测速](measurements/linux-c-rw-benchmark.md)。

## 传输

Linux 使用 POSIX 串口，Windows 使用 Win32 COM，Android 通过 JNI 接入 USB。SPP/BLE 适配尚未实现；在确认设备透传 Modbus RTU 后，适配层需提供分包、重组和背压处理。
