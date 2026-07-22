#include "SyncMetrics.h"

#include <algorithm>
#include <iostream>

namespace
{
    constexpr std::size_t kMaxPendingPingsPerClient = 64;

    long long absLongLong(long long value)
    {
        return value < 0 ? -value : value;
    }

    const char* syncQualityFromDiff(long long absDiffMs)
    {
        if (absDiffMs <= 250)
        {
            return "good";
        }

        if (absDiffMs <= 1000)
        {
            return "watch";
        }

        return "drift";
    }
}

void SyncMetricsCollector::recordPingSent(
    int clientId,
    int sequenceNumber,
    Clock::time_point sentAt)
{
    std::lock_guard<std::mutex> lock(mutex_);

    ClientStats& stats = statsByClient_[clientId];
    if (stats.pendingPings.size() >= kMaxPendingPingsPerClient)
    {
        // 防止 client 长时间不回 PONG 时 pending map 无限增长。
        stats.pendingPings.erase(stats.pendingPings.begin());
    }

    stats.pendingPings[sequenceNumber] = sentAt;
}

void SyncMetricsCollector::recordPongReceived(
    int clientId,
    int sequenceNumber,
    Clock::time_point receivedAt)
{
    long long rttUs = -1;
    long long rttMs = -1;
    long long minRttMs = -1;
    long long avgRttMs = -1;
    long long maxRttMs = -1;
    long long sampleCount = 0;
    bool matchedPong = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        ClientStats& stats = statsByClient_[clientId];
        auto pendingIt = stats.pendingPings.find(sequenceNumber);
        if (pendingIt != stats.pendingPings.end())
        {
            matchedPong = true;
            rttUs = std::chrono::duration_cast<std::chrono::microseconds>(
                receivedAt - pendingIt->second
            ).count();
            rttMs = rttUs / 1000;
            stats.pendingPings.erase(pendingIt);

            ++stats.rttSampleCount;
            stats.latestRttMs = rttMs;
            stats.totalRttMs += rttMs;
            stats.minRttMs = stats.rttSampleCount == 1 ? rttMs : std::min(stats.minRttMs, rttMs);
            stats.maxRttMs = stats.rttSampleCount == 1 ? rttMs : std::max(stats.maxRttMs, rttMs);

            sampleCount = stats.rttSampleCount;
            minRttMs = stats.minRttMs;
            maxRttMs = stats.maxRttMs;
            avgRttMs = stats.totalRttMs / stats.rttSampleCount;
        }
    }

    if (!matchedPong)
    {
        std::lock_guard<std::mutex> logLock(logMutex_);

        std::cout << "[metric] type=rtt_sample"
            << " client=" << clientId
            << " seq=" << sequenceNumber
            << " status=unmatched_pong\n";
        return;
    }

    // RTT 使用 server 自己的发送时间和接收时间计算，不依赖 client/server 时钟同步。
    // 后续估算单向延迟时，先用 min_rtt/2 作为保守估计。
    {
        std::lock_guard<std::mutex> logLock(logMutex_);

        std::cout << "[metric] type=rtt_sample"
            << " client=" << clientId
            << " seq=" << sequenceNumber
            << " rtt_us=" << rttUs
            << " rtt_ms=" << rttMs
            << " min_rtt_ms=" << minRttMs
            << " avg_rtt_ms=" << avgRttMs
            << " max_rtt_ms=" << maxRttMs
            << " samples=" << sampleCount
            << "\n";
    }
}

void SyncMetricsCollector::recordProgressReport(const SyncMetricSample& sample)
{
    Clock::time_point now = Clock::now();
    long long diffMs = sample.clientPositionMs - sample.roomPositionMs;
    long long absDiffMs = absLongLong(diffMs);
    long long estimatedOneWayDelayMs = 0;
    long long compensatedClientPositionMs = sample.clientPositionMs;
    long long compensatedDiffMs = 0;
    long long compensatedAbsDiffMs = 0;
    long long reportIntervalMs = -1;
    long long sampleCount = 0;
    long long avgAbsDiffMs = 0;
    long long maxAbsDiffMs = 0;
    long long minDiffMs = 0;
    long long maxDiffMs = 0;
    long long latestRttMs = -1;
    long long minRttMs = -1;
    long long avgRttMs = -1;
    long long rttSampleCount = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        ClientStats& stats = statsByClient_[sample.clientId];
        if (stats.hasLastReportTime)
        {
            reportIntervalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - stats.lastReportTime
            ).count();
        }

        stats.lastReportTime = now;
        stats.hasLastReportTime = true;

        ++stats.sampleCount;
        stats.totalAbsDiffMs += absDiffMs;
        stats.maxAbsDiffMs = stats.sampleCount == 1
            ? absDiffMs
            : std::max(stats.maxAbsDiffMs, absDiffMs);

        if (!stats.hasDiff)
        {
            stats.minDiffMs = diffMs;
            stats.maxDiffMs = diffMs;
            stats.hasDiff = true;
        }
        else
        {
            stats.minDiffMs = std::min(stats.minDiffMs, diffMs);
            stats.maxDiffMs = std::max(stats.maxDiffMs, diffMs);
        }

        sampleCount = stats.sampleCount;
        avgAbsDiffMs = stats.totalAbsDiffMs / stats.sampleCount;
        maxAbsDiffMs = stats.maxAbsDiffMs;
        minDiffMs = stats.minDiffMs;
        maxDiffMs = stats.maxDiffMs;

        latestRttMs = stats.latestRttMs;
        minRttMs = stats.minRttMs;
        rttSampleCount = stats.rttSampleCount;
        avgRttMs = stats.rttSampleCount > 0
            ? stats.totalRttMs / stats.rttSampleCount
            : -1;

        if (stats.minRttMs >= 0)
        {
            estimatedOneWayDelayMs = stats.minRttMs / 2;
        }
    }

    if (sample.clientState == PlaybackState::Playing)
    {
        compensatedClientPositionMs += estimatedOneWayDelayMs;
    }

    compensatedDiffMs = compensatedClientPositionMs - sample.roomPositionMs;
    compensatedAbsDiffMs = absLongLong(compensatedDiffMs);

    // 这一行是后续同步算法的数据基础：
    // raw_diff_ms 是未补偿网络上报延迟的偏差；
    // compensated_diff_ms 会在 client 正在播放时加上估算单向延迟。
    // 当前阶段只打印，不做 seek 或倍速校正。
    {
        std::lock_guard<std::mutex> logLock(logMutex_);

        std::cout << "[metric] type=progress_report"
            << " client=" << sample.clientId
            << " sample=" << sampleCount
            << " client_state=" << stateToString(sample.clientState)
            << " room_state=" << stateToString(sample.roomState)
            << " client_pos_ms=" << sample.clientPositionMs
            << " room_pos_ms=" << sample.roomPositionMs
            << " raw_diff_ms=" << diffMs
            << " raw_abs_diff_ms=" << absDiffMs
            << " one_way_ms=" << estimatedOneWayDelayMs
            << " compensated_client_pos_ms=" << compensatedClientPositionMs
            << " compensated_diff_ms=" << compensatedDiffMs
            << " compensated_abs_diff_ms=" << compensatedAbsDiffMs
            << " avg_abs_diff_ms=" << avgAbsDiffMs
            << " max_abs_diff_ms=" << maxAbsDiffMs
            << " min_diff_ms=" << minDiffMs
            << " max_diff_ms=" << maxDiffMs
            << " latest_rtt_ms=" << latestRttMs
            << " min_rtt_ms=" << minRttMs
            << " avg_rtt_ms=" << avgRttMs
            << " rtt_samples=" << rttSampleCount
            << " report_interval_ms=" << reportIntervalMs
            << " quality=" << syncQualityFromDiff(compensatedAbsDiffMs)
            << "\n";
    }
}

void SyncMetricsCollector::removeClient(int clientId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    statsByClient_.erase(clientId);
}
