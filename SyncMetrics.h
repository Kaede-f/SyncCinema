#pragma once

#include <chrono>
#include <mutex>
#include <unordered_map>

#include "Protocol.h"

// 单次进度上报样本。
// clientPositionMs 来自某个 client 的真实播放器进度；
// roomPositionMs 来自 server 维护的房间权威进度。
// 两者相减就是当前最重要的同步观测指标。
struct SyncMetricSample
{
    int clientId = 0;
    long long clientPositionMs = 0;
    long long roomPositionMs = 0;
    PlaybackState clientState = PlaybackState::Stopped;
    PlaybackState roomState = PlaybackState::Stopped;
};

// SyncMetricsCollector 只做日志分析，不做自动纠偏。
// 这样可以先用事实数据判断同步问题，再决定后续算法策略。
class SyncMetricsCollector
{
public:
    void recordProgressReport(const SyncMetricSample& sample);
    void removeClient(int clientId);

private:
    using Clock = std::chrono::steady_clock;

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
    };

    std::mutex mutex_;
    std::unordered_map<int, ClientStats> statsByClient_;
};
