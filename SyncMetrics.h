#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "Protocol.h"

// 单次进度上报样本。
// clientPositionMs 来自某个 client 的真实播放器进度；
// roomPositionMs 来自 server 维护的房间权威进度。
struct SyncMetricSample
{
    int clientId = 0;
    long long clientPositionMs = 0;
    long long roomPositionMs = 0;
    PlaybackState clientState = PlaybackState::Stopped;
    PlaybackState roomState = PlaybackState::Stopped;
};

// SyncMetricsCollector 只做日志分析，不做自动纠偏。
//
// 当前同时观察两类偏差：
//   1. client 与 Room 理论进度的偏差，用于发现播放器整体落后于房间时钟；
//   2. client 与 client 的相对偏差，这是判断两位观众是否真正同步的核心指标。
//
// 把“测量”和“控制”分开，可以避免尚未验证指标是否可靠时就自动 seek，
// 从而把测量误差放大成用户可见的播放跳动。
class SyncMetricsCollector
{
public:
    using Clock = std::chrono::steady_clock;

    void recordPingSent(int clientId, int sequenceNumber, Clock::time_point sentAt);
    void recordPongReceived(int clientId, int sequenceNumber, Clock::time_point receivedAt);

    // 每次 PLAY / PAUSE / SEEK 都开启新的测量周期。
    // RTT 描述网络链路，可以跨周期保留；播放偏差属于某次控制后的结果，必须重新统计。
    void beginControlEpoch(const SyncMessage& controlMessage);

    void recordProgressReport(const SyncMetricSample& sample);
    void removeClient(int clientId);

private:
    struct ClientStats
    {
        long long sampleCount = 0;
        long long totalAbsDiffMs = 0;
        long long minDiffMs = 0;
        long long maxDiffMs = 0;
        long long maxAbsDiffMs = 0;
        bool hasDiff = false;
        bool hasLastReportTime = false;
        Clock::time_point lastReportTime{};

        // 保存该 client 最近一次进度上报。
        // 当另一个 client 上报时，会把两份上报投影到同一个 server 时刻再比较，
        // 避免因为两份 REPORT 到达时间不同而制造虚假的进度差。
        bool hasLatestProgressReport = false;
        long long latestPositionMs = 0;
        PlaybackState latestPlaybackState = PlaybackState::Stopped;
        Clock::time_point latestReportReceivedAt{};

        long long rttSampleCount = 0;
        long long latestRttMs = -1;
        long long minRttMs = -1;
        long long maxRttMs = -1;
        long long totalRttMs = 0;
        std::unordered_map<int, Clock::time_point> pendingPings;
    };

    struct PairWindowStats
    {
        int clientAId = 0;
        int clientBId = 0;
        std::deque<long long> recentDiffMs;
        int consecutiveSevereSamples = 0;
    };

    std::mutex mutex_;
    std::mutex logMutex_; // 保护指标日志输出
    std::unordered_map<int, ClientStats> statsByClient_;
    std::unordered_map<std::uint64_t, PairWindowStats> pairStatsByKey_;

    long long currentControlEpoch_ = 0;
    bool hasControlEpochStart_ = false;
    Clock::time_point controlEpochStartedAt_{};
};
