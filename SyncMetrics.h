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
    using Clock = std::chrono::steady_clock;

    void recordPingSent(int clientId, int sequenceNumber, Clock::time_point sentAt);
    void recordPongReceived(int clientId, int sequenceNumber, Clock::time_point receivedAt);
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

        long long rttSampleCount = 0;
        long long latestRttMs = -1;
        long long minRttMs = -1;
        long long maxRttMs = -1;
        long long totalRttMs = 0;
        std::unordered_map<int, Clock::time_point> pendingPings;
    };

    std::mutex mutex_;
    std::mutex logMutex_; // 保护指标日志输出
    std::unordered_map<int, ClientStats> statsByClient_;
};
