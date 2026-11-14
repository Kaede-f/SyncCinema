# SyncCinema

[![Linux CI](https://github.com/Kaede-f/SyncCinema/actions/workflows/linux-ci.yml/badge.svg?branch=main)](https://github.com/Kaede-f/SyncCinema/actions/workflows/linux-ci.yml)

SyncCinema 是一个 C++20 同步观影项目。多个 Windows Qt 客户端在各自的设备上用 libVLC 播放同一个本地媒体或 HTTP 媒体，Linux 服务端协调播放、暂停和跳转。迟到的客户端通过房间快照恢复当前状态。

## 架构与功能

- **Linux 服务端**以单个 epoll LT Reactor 驱动非阻塞 TCP Socket。每个连接使用有界发送队列处理短写和背压，慢客户端积压超限时会被断开。Windows 服务端使用 WSAPoll，共用协议和房间逻辑。
- **同步协议**由服务端维护单房间权威状态，以 `controlEpoch` 标识每次控制。客户端通过心跳与进度上报提供 RTT 和播放偏差样本。服务端在满足条件时发送校正指令，并跟踪控制回执和校正结果。
- **Windows 客户端**由 Qt Widgets 提供界面，`SyncClientSession` 负责 TCP 会话，libVLC 负责媒体播放。
- **可观测性**包括 Prometheus 指标、Grafana 看板、JSONL 事件和 TCP 故障注入测试。[指标与故障实验](docs/Observability.md)

## 构建与运行

### Linux 服务端

需要 C++20 编译器、CMake 和 Python 3。以 Ubuntu 为例：

```bash
sudo apt install build-essential cmake python3
cmake -S . -B out/build/linux-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build out/build/linux-release --parallel --target SyncCinemaServer SyncCinemaCoreTests
ctest --test-dir out/build/linux-release --output-on-failure
python3 -m unittest discover -s tools/observability/tests -v
./out/build/linux-release/SyncCinemaServer
```

播放协议使用 TCP `9000`。本机 Prometheus 端点为 `http://127.0.0.1:9100/metrics`，可通过 `SYNCCINEMA_METRICS_PORT` 设置端口。设置 `SYNCCINEMA_TELEMETRY_PATH` 可保存 JSONL。TCP 集成测试也会使用端口 `9000`，请在同一次运行中先测试再启动服务端。

### Windows Qt 客户端

需要支持 C++20 的 MSVC、CMake、Qt 5 或 Qt 6 Widgets，以及匹配架构的 libVLC SDK 与运行时。在 Visual Studio x64 开发者 PowerShell 中配置，按本机安装位置替换以下示例路径。

```powershell
$qt = 'C:/Qt/6.x/msvc2022_64'
$vlc = 'C:/SDKs/vlc-win64'

cmake -S . -B out/build/windows-qt -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_QT_CLIENT=ON -DUSE_LIBVLC=ON `
  "-DCMAKE_PREFIX_PATH=$qt" `
  "-DVLC_INCLUDE_DIR=$vlc/sdk/include" `
  "-DVLC_LIBRARY=$vlc/sdk/lib/libvlc.lib" `
  "-DVLC_RUNTIME_DIR=$vlc"
cmake --build out/build/windows-qt --config Release --target SyncCinemaQt
.\out\build\windows-qt\Release\SyncCinemaQt.exe
```

使用其他 Visual Studio 版本时选用对应的 CMake 生成器。客户端中填写相同媒体来源和服务端的 IP 或域名即可连接。[界面与连接指南](docs/QtClientGuide.md)

## 性能

在同一台机器的 WSL2 Ubuntu 22.04 回环网络上，以 GCC 11.4 构建 Release。1,000 个模拟 TCP 客户端按每批 32 个连接加入，随后发送 160 条控制指令，每版均匹配 160,000 条 ACK。

| 1,000 个模拟连接 | 每连接线程 `88698b1` | 初版 Reactor `2e22f49` |
| --- | ---: | ---: |
| 服务端线程 | 1,003 | 2 |
| RSS | 19.75 MiB | 5.88 MiB |
| 控制回显 P99 | 111.57 ms | 59.66 ms |
| 控制回显吞吐 | 37,248 条/秒 | 78,188 条/秒 |

后续在五轮同条件 Reactor A/B 中，预扩容 Linux fd 表将批量 JOIN P95 的中位数从 **23.71 ms** 降到 **1.73 ms**。控制回显吞吐的中位数为 84,435 → 84,080 条/秒，P99 为 63.31 → 58.30 ms。

这些结果衡量同机模拟 TCP 接入与控制回显。压测未覆盖 1,000 人真实视频播放、解码或广域网同步。[复现命令与完整对照](docs/LinuxServerArchitecture.md#构建与复现) · [压测脚本](tools/benchmark_server.py)
