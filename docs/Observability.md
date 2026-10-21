# SyncCinema 可观测性与指标规范

## 1. 目标

这份文档定义 SyncCinema 的统一观测口径。它解决三个问题：

1. 用户是否能够顺利进入并完成一次同步观影；
2. 多端播放是否真正同步，异常后能否自动恢复；
3. server、网络和媒体链路是否健康，容量是否接近上限。

指标用于发现问题、验证改进和建立服务目标，不用于代替具体日志。当前实现已经形成
JSONL 事件、会话聚合、Prometheus 指标、Grafana 看板、Alertmanager 规则和可复现
故障实验的完整链路。

## 2. 观测模型

### 2.1 三类信号

- **事件日志**：回答某次会话具体发生了什么，例如 JOIN 被拒绝或客户端异常断开。
- **聚合指标**：回答一段时间内发生了多少次、成功率多少、P95 多高。
- **链路关联**：通过 `session_id`、`client_id`、`control_epoch` 和 `command_id`
  把同一次操作在不同线程和组件中的记录串起来。

耗时只能使用 `steady_clock` 在同一进程内计算；UTC `timestamp` 只用于日志检索，
不能直接拿不同设备的墙上时钟相减计算网络延迟。

### 2.2 JSONL 事件结构

每一行都是一个完整 JSON 对象：

```json
{
  "schema_version": 1,
  "timestamp": "2026-10-21T12:00:00.123Z",
  "service": "sync_server",
  "event": "join_result",
  "session_id": "9a2d6c5f-32d5-4ac8-a522-f14418271fd8",
  "client_id": 2,
  "control_epoch": 7,
  "attributes": {
    "success": true,
    "reason": "OK",
    "duration_ms": 26
  }
}
```

字段约束：

- `schema_version`：事件结构发生不兼容变化时递增。
- `session_id`：每次空房间被首个客户端建立时生成新的 UUID。
- `client_id`：server 分配的会话内连接编号；不能作为长期用户身份。
- `media_id`：规范化媒体来源的稳定哈希，不记录原始本地路径或 URL。
- `reason`：使用稳定的大写错误码，不能把异常全文当作指标标签。
- 高频、无限取值字段只能留在日志中，不能直接成为 Prometheus label。

设置以下环境变量后，server 会把纯 JSONL 追加写入指定文件：

```bash
export SYNCCINEMA_TELEMETRY_PATH=/var/log/synccinema/telemetry.jsonl
```

未设置时仍输出带 `[telemetry]` 前缀的控制台记录。无法创建日志目录或打开文件
不会阻止 server 启动，但会明确报告 telemetry file disabled。

## 3. 核心用户体验指标

所有成功率必须同时展示分子、分母和统计窗口；延迟类指标至少展示 P50、P95、P99，
不能只展示平均值。

| 指标 | 类型 | 严格定义 | 主要维度 | 第一版目标 |
|---|---|---|---|---|
| `join_success_ratio` | Ratio | `success=true` 的 `join_result` 数 / 全部 `join_result` 数 | app_version、reason | >= 99% |
| `time_to_first_frame_ms` | Histogram | 用户发起连接到 libVLC 首帧到达的 steady_clock 耗时 | media_kind、app_version | P95 < 3000 ms |
| `initial_sync_duration_ms` | Histogram | 收到 SNAPSHOT 到播放器连续两次进入 250 ms 容差的耗时 | room_state | P95 < 2000 ms |
| `sync_good_ratio` | Ratio | 非 settling 的有效 pair 样本中，`abs(pair_diff_ms) <= 250` 的比例 | network_profile | >= 95% |
| `pair_skew_ms` | Histogram | 两个客户端进度投影到同一 server 时刻后的绝对差 | playback_state | P95 < 250 ms |
| `control_apply_latency_ms` | Histogram | server 接收控制到目标客户端返回 APPLIED ACK 的耗时 | command_type | P95 < 500 ms |
| `correction_success_ratio` | Ratio | 收到匹配 APPLIED 的校正数 / 已成功下发的校正数 | rejection_reason | >= 99% |
| `correction_convergence_ms` | Histogram | server 首次确认持续偏差到 pair 重新进入 250 ms 的耗时 | correction_type | P95 < 2000 ms |
| `hard_corrections_per_watch_hour` | Rate | 硬 seek 校正次数 / 有效 Playing 小时 | app_version | < 1 次/小时 |
| `crash_free_session_ratio` | Ratio | 正常结束且无进程崩溃的会话数 / 已开始会话数 | app_version、os | >= 99.5% |

### 3.1 有效同步样本

`sync_good_ratio` 和 `pair_skew_ms` 只统计满足以下条件的样本：

- 两端属于同一个 `session_id` 和 `control_epoch`；
- 两端播放状态一致；
- 两份 REPORT 的年龄均不超过 2500 ms；
- 两端已有可用 RTT 基线；
- 不处于 PLAY、PAUSE、SEEK 后的 settling 窗口；
- 不处于客户端刚加入但播放器尚未完成初始位置收敛的阶段。

这能避免把正常控制切换、陈旧数据或测量缺失误算成同步故障。

## 4. 纯技术指标

### 4.1 延迟 Latency

- TCP connect、JOIN 握手、SNAPSHOT 应用和首帧耗时；
- PLAY、PAUSE、SEEK 的端到端应用延迟；
- RTT baseline、rolling median、rolling P95 和抖动；
- 校正命令 ACK 延迟和最终收敛耗时；
- Nginx Range 请求响应时间。

### 4.2 流量 Traffic

- 活跃会话、在线客户端和峰值并发数；
- JOIN、CONTROL、REPORT、PING/PONG、CORRECT 消息速率；
- server 每秒收发字节数；
- Nginx 视频出口带宽、并发连接和 Range 请求数量。

### 4.3 错误 Errors

- JOIN_REQUIRED、MEDIA_MISMATCH、SNAPSHOT_SEND_FAILED 的计数和比例；
- 非法协议、stale epoch、unmatched PONG、ACK timeout；
- socket accept/recv/send 错误；
- 媒体打开失败、解码失败、缓冲超时；
- Nginx 4xx、5xx；
- server 重启和客户端崩溃。

### 4.4 饱和度 Saturation

- server CPU、RSS 内存、线程数和文件描述符数；
- 当前 socket 数、accept backlog 和连接上限利用率；
- 网络出口带宽利用率；
- telemetry 文件增长速度和磁盘空间；
- 客户端 CPU、内存和解码负载。

服务请求采用 RED（Rate、Errors、Duration）组织；主机资源采用 USE
（Utilization、Saturation、Errors）组织。

## 5. 生命周期事件

当前已经实现：

| 事件 | 发生时机 | 核心字段 |
|---|---|---|
| `server_started` | server 完成监听初始化 | listen_port、jsonl_file_enabled |
| `session_started` | 空房间接受首个媒体会话 | session_id、media_id |
| `join_result` | JOIN 处理完成 | success、reason、duration_ms |
| `client_disconnected` | 已加入客户端连接结束 | reason、socket_error、connection_duration_ms |
| `session_ended` | 最后一个客户端离开 | duration_ms |
| `media_first_frame` | 客户端报告 libVLC 首帧 | trigger、status、duration_ms |
| `control_dispatched` | server 广播权威控制命令 | command、target_clients、broadcast_success |
| `control_applied` | client 返回控制执行结果 | status、ack_latency_ms、client_apply_ms |
| `correction_completed` | client 返回校正执行结果 | command_id、status、ack_latency_ms |
| `correction_converged` | 校正后连续两次恢复到容差内 | convergence_ms、final_abs_diff_ms |
| `session_summary` | 最后一个客户端离开后生成会话汇总 | 首帧、控制、同步和校正聚合结果 |

当前没有实现 `client_crash_report`。server 能观察 socket 异常断开，但不能可靠区分
客户端崩溃、进程被杀和网络中断；这需要客户端崩溃上报或外部进程监督器。

## 6. 在线指标与告警

server 默认在 `127.0.0.1:9100` 提供两个只读端点：

- `/healthz`：进程级健康检查；
- `/metrics`：Prometheus 文本格式指标。

可通过 `SYNCCINEMA_METRICS_PORT` 修改端口。指标 label 只使用固定枚举值，媒体
URL、session_id、client_id 和错误全文不会成为 label，避免时间序列基数无限增长。

Prometheus 当前覆盖：在线客户端和活跃会话、JOIN 结果及耗时、首帧结果及耗时、
控制下发和 ACK、有效同步样本和 pair skew、校正生命周期与收敛耗时。

告警必须指向可执行动作，并避免对单个偶发样本报警：

- 10 分钟窗口 `join_success_ratio < 0.99` 且尝试数 >= 20；
- 10 分钟窗口 `pair_skew_ms P95 > 250` 且有效样本数 >= 100；
- 5 分钟窗口 correction ACK 失败率 > 1%；
- server 进程不可用或 9000 端口健康检查连续失败 3 次；
- 文件描述符利用率 > 80% 持续 5 分钟；
- telemetry 磁盘剩余空间 < 15%。

仓库中的 12 条规则位于 `deploy/observability/prometheus/alerts.yml`，均设置持续
时间或最小样本量。Alertmanager 第一版只保留本地告警状态，不内置私人邮箱、Webhook
或密钥；部署方应在服务器私有配置中添加真实通知接收器。

## 7. 实施路线

1. **事件基础（完成）**：版本化 JSONL、session_id、生命周期事件和指标字典。
2. **核心体验测量（完成）**：首帧、控制 ACK、会话汇总、同步良好率和收敛时间。
3. **在线聚合（完成）**：Prometheus counter/gauge/histogram 与 `/metrics` endpoint。
4. **展示告警（完成）**：Grafana 产品总览看板和 Alertmanager 规则。
5. **故障实验（完成第一版）**：可复现 TCP 延迟、抖动、阻塞和断连，以及自动验收报告。

每一阶段都必须先通过确定性测试和实验数据验证口径，再把指标用于自动控制或告警。

## 8. 监控栈部署

### 8.1 启动

Linux server 先以 telemetry 和 metrics 配置启动：

```bash
export SYNCCINEMA_TELEMETRY_PATH=/var/log/synccinema/telemetry.jsonl
export SYNCCINEMA_METRICS_PORT=9100
./build/SyncCinemaServer
```

复制环境变量示例并设置独立 Grafana 密码：

```bash
cd deploy/observability
cp .env.example .env
# 编辑 .env，不能把真实密码提交到 Git
docker compose up -d
docker compose ps
```

监控组件只监听服务器回环地址，不应直接暴露到公网。开发机通过 SSH 隧道访问：

```powershell
ssh -L 3000:127.0.0.1:3000 `
    -L 9090:127.0.0.1:9090 `
    -L 9093:127.0.0.1:9093 `
    root@47.101.211.107
```

随后访问：

- Grafana：`http://127.0.0.1:3000`；
- Prometheus：`http://127.0.0.1:9090`；
- Alertmanager：`http://127.0.0.1:9093`。

Grafana 启动后会自动加载 `SyncCinema Product Overview`，无需手工复制 PromQL。

### 8.2 配置校验

提交前至少执行：

```bash
promtool check config deploy/observability/prometheus/prometheus.yml
promtool check rules deploy/observability/prometheus/alerts.yml
amtool check-config deploy/observability/alertmanager/alertmanager.yml
docker compose -f deploy/observability/compose.yaml config
```

## 9. 故障实验与自动报告

### 9.1 可复现网络故障

由于 Qt client 当前固定连接 9000 端口，开发机先启动本地代理，并让 UI 连接
`127.0.0.1`。以下示例为每个方向增加 `80 +/- 20 ms` 延迟，在连接 60 秒后阻塞
转发 4 秒：

```powershell
python tools/observability/tcp_fault_proxy.py `
    --upstream-host 47.101.211.107 `
    --latency-ms 80 `
    --jitter-ms 20 `
    --stall-after-seconds 60 `
    --stall-duration-seconds 4
```

代理不删除 TCP 字节，因为随机删除字节会破坏应用层分帧，不能等价模拟 IP 丢包。
真实丢包实验应在隔离测试主机或网络命名空间中使用 `tc netem`，不能直接修改承载
SSH 的生产网卡。代理输出 JSONL，并使用固定随机种子保证参数相同时可复现。

建议固定实验步骤：

1. 启动两个 client，连接同一媒体；
2. 连续播放至少 15 分钟，执行不少于 20 次 PLAY/PAUSE/SEEK；
3. 故障窗口内继续操作，观察告警、校正和恢复；
4. 测试结束后生成报告，不用单次截图代替统计结论。

### 9.2 生成验收报告

保持 Prometheus SSH 隧道后运行：

```powershell
python tools/observability/prometheus_experiment_report.py `
    --prometheus-url http://127.0.0.1:9090 `
    --window 15m `
    --scenario latency-80ms-jitter-20ms-stall-4s `
    --output reports/latency-experiment.md
```

工具同时生成 Markdown 和 JSON。结果有四种状态：

- `PASS`：样本量足够且达到目标；
- `FAIL`：样本量足够但违反目标；
- `INSUFFICIENT_DATA`：有数据但样本量不足，不能下结论；
- `NO_DATA`：采集或查询链路存在问题，不能视为通过。

报告器测试：

```powershell
python -m unittest discover -s tools/observability/tests -v
```

## 10. 当前边界

本分支完成的是单房间、少量客户端场景下的产品可观测性闭环，不代表已经具备大规模
生产能力。以下项目仍需真实业务量或新架构支持：

- `hard_corrections_per_watch_hour` 缺少精确的有效 Playing 时长分母；
- `crash_free_session_ratio` 缺少客户端崩溃上报或监督进程；
- Nginx 请求延迟、4xx/5xx 和出口带宽尚未纳入统一看板；
- Alertmanager 尚未配置真实通知渠道；
- 当前 server 是单进程、内存态单房间，不支持水平扩展和持久化房间恢复。

这些边界应作为下一阶段容量与可靠性工作的输入，而不是通过补写日志“假装已经解决”。
