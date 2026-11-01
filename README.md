# RIDEN-Core

RIDEN 可调电源（RD60xx / RK 系列）的 Modbus RTU 通信内核，用 C11 编写。它为 Linux、Windows 和 Android 上位机提供统一的设备连接、寄存器读写、采样缓存和调度，可以作为本机后台服务（`riden-cored`）运行，也可以作为共享库或 Android AAR 嵌入。

上位机示例：[RIDEN-Desktop](https://github.com/riden-tools/RIDEN-Desktop)（Electron）。

## 功能

- 每台设备一个 I/O worker 独占链路，读、写、采集共享同一队列和缓存，相同范围的读取会被复用。
- 采集由事务完成驱动（默认无固定轮询间隔），并发写入时仍优先保证采样。
- 按型号校验写入范围；未识别的型号默认只读，可用 `--model-profile` 显式登记缩放和上限。
- 本机 NDJSON 接口（`127.0.0.1:19606`），支持快照、订阅、显式读取和写入。
- 平台：Linux（POSIX 串口）、Windows（Win32 COM，可注册为系统服务）、Android（JNI + CH340 USB + 前台服务 / AIDL）。

## 支持的型号

| HR0 型号代码 | 型号 | 电压精度 | 电流精度 | 最大电压 / 电流 |
|---|---|---|---|---|
| 6006, 60060–60064, 60067 | RD6006 | 0.01 V | 0.001 A | 61 V / 6.1 A |
| 60065 | RD6006P | 0.001 V | 0.0001 A | 61 V / 6.1 A |
| 60066 | RK6006 | 0.01 V | 0.001 A | 61 V / 6.1 A |
| 6012, 60120–60124 | RD6012 | 0.01 V | 0.01 A | 61 V / 12.1 A |
| 6018, 60180–60189 | RD6018 | 0.01 V | 0.01 A | 61 V / 18.1 A |
| 6024, 60241–60249 | RD6024 | 0.01 V | 0.01 A | 61 V / 24.1 A |
| 6030, 60300–60309 | RD6030 | 0.01 V | 0.01 A | 61 V / 30.1 A |
| 8009 | RK8009 | 0.01 V | 0.001 A | 81 V / 9.1 A |

真机只在 RD6006（型号代码 60067）上验证过，其余型号的范围来自协议资料，详见[验证记录](docs/VALIDATION.md)。

## 构建

依赖：CMake ≥ 3.22、C11 编译器；运行测试需要 Python 3。

### Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix dist/linux
```

`ctest` 包含单元测试和基于伪终端的 daemon 集成测试（模拟设备，不需要真机）。加 `-DRIDEN_SANITIZE=ON` 可启用 AddressSanitizer / UBSan。

### Windows（在 Linux 上用 MinGW-w64 交叉编译）

```bash
cmake -S . -B build-windows -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-windows
cmake --install build-windows --prefix dist/windows
```

产物在 `dist/windows/bin`（`riden-cored.exe`、`libriden_core.dll`、`riden-bench.exe`），依赖系统自带的 UCRT。在 Windows 上也可以直接用 MSVC 或 MinGW 原生构建。

### Android

```bash
cd android
./gradlew assembleRelease
```

需要 Android SDK 35 和 NDK 27.0.12077973，产物为 `android/build/outputs/aar/RIDEN-Core-Android-release.aar`，含 arm64-v8a、armeabi-v7a、x86_64、x86。集成方法见 [Android 集成](docs/ANDROID.md)。

## 使用 `riden-cored`

```bash
riden-cored --device /dev/ttyUSB0            # 只读，监听 127.0.0.1:19606
riden-cored --device COM3 --allow-writes     # Windows，允许写入
```

| 选项 | 默认 | 说明 |
|---|---|---|
| `--device PATH` | 必填 | 串口路径 |
| `--port N` | 19606 | 本机监听端口 |
| `--baud N` | 115200 | 波特率 |
| `--address N` | 1 | Modbus 从机地址（1–247） |
| `--mode hot\|status\|full` | hot | 采集范围：HR10–13 / HR4–19 / HR0–120 |
| `--period-ms N` | 0 | 采集间隔，0 表示上一笔完成后立即继续 |
| `--allow-writes` | 关 | 允许写入已知可写寄存器 |
| `--allow-multiwrite` | 关 | 允许 0x10 连续写入 |
| `--model-profile ID,VSCALE,ISCALE,VMAX,IMAX,OVP,OCP` | 无 | 登记未内置的型号，可重复使用，最多 8 个 |

订阅示例（只用 Python 标准库）：

```bash
python3 examples/subscribe.py
```

请求格式、错误码和写入规则见[本机接口](docs/API.md)。

### 作为服务运行

- Linux（systemd 用户服务）：把 `riden-cored` 放到 `~/.local/bin`，复制 [packaging/riden-core.service](packaging/riden-core.service) 到 `~/.config/systemd/user/`，在 `~/.config/riden-core/daemon.env` 中写 `RIDEN_DEVICE=/dev/serial/by-id/...`，然后 `systemctl --user enable --now riden-core`。
- Windows：在管理员 PowerShell 中运行 `packaging/install-service.ps1 -Device COM3 -Executable dist\windows\bin\riden-cored.exe`（脚本与 `riden-cored.exe` 放在同一目录时可省略 `-Executable`），再 `Start-Service RIDEN-Core`。服务以只读方式运行。

Linux 下读写串口需要加入 `dialout`（Debian/Ubuntu）或 `uucp`（Arch）组。

## 安全

本机接口没有认证。启用 `--allow-writes` 后，本机任何能连接该端口的进程都能控制电源输出。服务只绑定回环地址，不要把端口转发到网络上。

## 文档

- [本机接口与寄存器读写](docs/API.md)
- [通信架构与调度](docs/ARCHITECTURE.md)
- [Android 集成](docs/ANDROID.md)
- [验证范围与结果](docs/VALIDATION.md)
- [真机读写调度测试](docs/measurements/linux-c-rw-benchmark.md)

## 目录结构

| 路径 | 说明 |
|---|---|
| `include/riden_core.h` | 公共 C ABI |
| `src/` | 核心、Modbus RTU、串口、IPC、daemon、JNI |
| `tests/` | 单元测试、PTY 集成测试、真机测试脚本 |
| `android/` | Android 库工程（Gradle） |
| `packaging/` | systemd 单元、Windows 服务安装脚本 |
| `cmake/` | 交叉编译工具链 |
| `examples/` | 客户端示例 |

## 许可证

[GPL-3.0](LICENSE)。本项目与 RIDEN 官方无关。
