# riden-mcp

让 AI agent（Claude Code、Codex 等支持 MCP 的客户端）通过 [MCP](https://modelcontextprotocol.io) 读取和控制 RIDEN 电源。它连接本机已在运行的 `riden-cored`，把工具调用转换成 [本机接口](../docs/API.md) 请求，C 核心无需改动。

```
Agent ⇄ stdio ⇄ riden-mcp ⇄ TCP 127.0.0.1:19606 ⇄ riden-cored ⇄ 串口 ⇄ 电源
```

只支持 stdio 传输，不开放网络端口。

## 工具

所有数值都是物理单位（V、A、W），缩放和型号上限从 daemon 的能力表读取。

| 工具 | 作用 | 何时可用 |
|---|---|---|
| `get_status` | 型号、固件、输出状态、设定值、实测 V/I/P、CC/CV、保护状态、上限 | 总是 |
| `sample` | 采样 `duration_s` 秒（最长 60），返回每个通道的最小/最大/平均值和最多 `max_points` 个点 | 总是 |
| `set_voltage` | 设定电压，写入后回读状态 | `--enable-writes` |
| `set_current` | 设定限流，写入后回读状态 | `--enable-writes` |
| `set_output` | 打开或关闭输出；设定值超出上限时拒绝打开 | `--enable-writes` |
| `recall_preset` | 写 HR19 切换到 M1–M9，同时使用对应组的 OVP/OCP | `--enable-writes` |

写入工具带 `destructiveHint`，客户端一般会在每次调用前请你确认。没有提供写原始寄存器、OVP/OCP 或背光的工具。

## 安装

需要 Python 3.10+。在本目录下：

```bash
uv sync                 # 或：python3 -m venv .venv && .venv/bin/pip install -e .
```

## 使用

先启动 `riden-cored`（见[上级 README](../README.md)）。只读使用时 daemon 不需要 `--allow-writes`。

注册到 Claude Code（只读）：

```bash
claude mcp add riden -- uv run --directory /path/to/RIDEN-Core/mcp riden-mcp
```

允许 agent 控制输出时，daemon 和 riden-mcp 都要显式开启写入，并且必须给出上限：

```bash
riden-cored --device /dev/ttyUSB0 --allow-writes
claude mcp add riden -- uv run --directory /path/to/RIDEN-Core/mcp \
  riden-mcp --enable-writes --max-voltage 12 --max-current 1
```

注册到 Codex：

```bash
codex mcp add riden -- uv run --directory /path/to/RIDEN-Core/mcp riden-mcp
```

或者写进 `~/.codex/config.toml`（路径要写绝对路径，不会展开 `~`）：

```toml
[mcp_servers.riden]
command = "uv"
args = ["run", "--directory", "/path/to/RIDEN-Core/mcp", "riden-mcp",
        "--enable-writes", "--max-voltage", "12", "--max-current", "1"]
```

写入前是否需要确认由各客户端自己的审批设置决定，不一定会按 `destructiveHint` 逐次询问；电压电流上限由 riden-mcp 检查，与客户端无关。

| 选项 | 默认 | 说明 |
|---|---|---|
| `--port N` | 19606 | `riden-cored` 的端口 |
| `--timeout S` | 5 | 单次请求超时（秒） |
| `--enable-writes` | 关 | 注册写入工具 |
| `--max-voltage V` | 无 | agent 能设定的最高电压，开启写入时必填 |
| `--max-current A` | 无 | agent 能设定的最高电流，开启写入时必填 |

`--max-voltage` / `--max-current` 应按接在电源上的被测设备来定，而不是按电源本身的 61 V。超出上限或超出型号能力的请求会在发出前被拒绝。每次写入都会记录到 stderr。

## 与 RIDEN Desktop 同时使用

一个串口只能由一个 `riden-cored` 打开。Desktop 默认会为每台设备自己启动 daemon，要共享同一台设备，就先单独运行 `riden-cored`，再在 Desktop 的「添加设备」里选择连接已在运行的核心服务。daemon 最多接受 32 个并发连接。

## 限制

- 主机采样间隔约 144 ms，单次写入约 287 ms（RD6006 实测）。适合测试流程和监控，不适合毫秒级闭环控制。
- 设定值不等于实际输出。写入后返回的是回读的设定值，实际电压电流请用 `get_status` 或 `sample` 查看。
- 本机接口没有认证。开启写入后，本机任何进程都能通过 19606 端口控制电源。

## 测试

测试会启动一个 PTY 模拟设备和真实的 `riden-cored`，再通过 stdio 驱动 `riden-mcp`，不需要真机（仅 Linux/macOS）：

```bash
uv run python tests/test_server.py ../build/riden-cored
```
