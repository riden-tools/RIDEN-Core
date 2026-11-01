# Android 集成

AAR 提供共享的 C11 引擎、JNI 字节传输适配、CH340 USB 适配器和前台 Service。前台 Service 遵守 Android 的进程生命周期和通知要求；它不是无需授权、永久运行的系统级 daemon。

宿主工程依赖 Android API 26+，并声明 Maven 仓库 `https://jitpack.io` 及 `implementation("com.github.mik3y:usb-serial-for-android:3.9.0")`。AAR 不打包该依赖的 Java 字节码，使用 Maven/Gradle 工程依赖时由依赖解析处理。

## 直接嵌入

先通过 `UsbManager.requestPermission()` 获取指定设备的用户授权。在宿主工作线程上：

```java
UsbTransport transport = new UsbTransport(usbManager, device);
RidenCore core = new RidenCore(transport, RidenCore.HOT, false, false);
long[] snapshot = core.snapshot();  // 只读缓存，可能尚无第一帧
int[] values = core.read(10, 4, 200); // 可能等待硬件响应
core.close();                      // 先结束 worker，再关闭 USB
```

如确实要写设备，构造 `new RidenCore(transport, mode, true, true)`；参数依次为允许写和允许 `0x10` 连续写。生产 UI 应以已知型号 profile 提供的物理量/raw scale 与上下限作转换和校验。

不要同时让旧上位机或其他 Core 实例打开同一设备。核心的串行传输回调由原生工作线程调用；JNI 会附加到 JVM，Android 宿主负责 USB 授权与前台生命周期。

## 前台服务与 AIDL

宿主在获准启动前台服务的时机（例如用户操作后）调用：

```java
Intent intent = new Intent(context, RidenCoreService.class);
intent.putExtra(RidenCoreService.EXTRA_DEVICE, device);
intent.putExtra("mode", RidenCore.HOT);
context.startForegroundService(intent);
context.bindService(intent, connection, Context.BIND_AUTO_CREATE);
// 在 ServiceConnection.onServiceConnected 中：
IRidenCore api = IRidenCore.Stub.asInterface(binder);
long[] snapshot = api.snapshot();
```

AIDL 的 `read`/`write` 是阻塞调用，应在工作线程执行。初始化异步进行，未就绪返回 IllegalStateException；宿主应显示连接中状态并重试查询。服务 `exported=false`，提供给同一个宿主应用及其进程使用，未开放任意第三方应用跨包控制。

USB 拔出后状态会显示失败并退避；此 USB 适配器不会自动重新申请权限或重新枚举设备。宿主应在 detach 时停止服务，在新设备获权后重新启动。用户/系统停止前台服务后不能保证继续采集。

## 快照数组 ABI v1

253 项 `long[]`：

| 索引 | 内容 |
|---|---|
| 0 | ABI version=1 |
| 1 | 样本序号 |
| 2 / 3 | 完整收包/请求发送的单调纳秒 |
| 4 / 5 | 硬件事务/失败数 |
| 6 / 7 / 8 | 队列拒绝/复用/缓存命中 |
| 9 / 10 | 待执行数/最近事务错误 |
| 11–131 | HR0–120 原始值；无效为 -1 |
| 132–252 | HR0–120 各自最后更新时间 |

## SPP 和 BLE 扩展

实现 `RidenCore.Transport` 即可替换 USB。SPP 将 RFCOMM 字节流接入；BLE 将异步通知写入有界接收缓冲区，`read` 等待缓冲数据，`write` 按实际 MTU 和 GATT 完成回调分包并控制背压。需要先确认目标硬件是否透传本项目的 Modbus RTU 帧及服务/特征 UUID。

当前没有蓝牙 SDK 调用和 UUID 假设。Android 宿主后续负责蓝牙运行时权限、扫描、配对和连接生命周期；这些不应放入设备寄存器调度器。
