# SyncCinema 可观测性与指标规范

## 1. 目标

这份文档定义 SyncCinema 的统一观测口径。它解决三个问题：

1. 用户是否能够顺利进入并完成一次同步观影；
2. 多端播放是否真正同步，异常后能否自动恢复；
3. server、网络和媒体链路是否健康，容量是否接近上限。

指标用于发现问题、验证改进和建立服务目标，不用于代替具体日志。当前实现先产出
版本化 JSONL 生命周期事件；会话汇总、Prometheus 和 Grafana 在后续阶段接入。

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

第一阶段已经实现：

| 事件 | 发生时机 | 核心字段 |
|---|---|---|
| `server_started` | server 完成监听初始化 | listen_port、jsonl_file_enabled |
| `session_started` | 空房间接受首个媒体会话 | session_id、media_id |
| `join_result` | JOIN 处理完成 | success、reason、duration_ms |
| `client_disconnected` | 已加入客户端连接结束 | reason、socket_error、connection_duration_ms |
| `session_ended` | 最后一个客户端离开 | duration_ms |

后续事件：`media_first_frame`、`control_applied`、`sync_sample_summary`、
`correction_completed` 和 `client_crash_report`。

## 6. 告警草案

告警必须指向可执行动作，并避免对单个偶发样本报警：

- 10 分钟窗口 `join_success_ratio < 0.99` 且尝试数 >= 20；
- 10 分钟窗口 `pair_skew_ms P95 > 250` 且有效样本数 >= 100；
- 5 分钟窗口 correction ACK 失败率 > 1%；
- server 进程不可用或 9000 端口健康检查连续失败 3 次；
- 文件描述符利用率 > 80% 持续 5 分钟；
- telemetry 磁盘剩余空间 < 15%。

## 7. 实施路线

1. **事件基础**：版本化 JSONL、session_id、生命周期事件和指标字典。
2. **核心体验测量**：首帧、控制 ACK、会话汇总、同步良好率和收敛时间。
3. **在线聚合**：Prometheus counter/gauge/histogram 与 `/metrics` endpoint。
4. **展示告警**：Grafana 三张看板和 Alertmanager 规则。
5. **故障实验**：延迟、抖动、丢包、进程暂停、断网、server 重启的自动报告。

每一阶段都必须先通过确定性测试和实验数据验证口径，再把指标用于自动控制或告警。
