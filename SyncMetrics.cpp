#include "SyncMetrics.h"

#include <algorithm>
#include <iostream>

namespace
{
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

void SyncMetricsCollector::recordProgressReport(const SyncMetricSample& sample)
{
    Clock::time_point now = Clock::now();
    long long diffMs = sample.clientPositionMs - sample.roomPositionMs;
    long long absDiffMs = absLongLong(diffMs);
    long long reportIntervalMs = -1;
    long long sampleCount = 0;
    long long avgAbsDiffMs = 0;
    long long maxAbsDiffMs = 0;
    long long minDiffMs = 0;
    long long maxDiffMs = 0;

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
    }

    // 这一行是后续同步算法的数据基础：
    // diff_ms > 0 表示 client 比房间权威进度快；
    // diff_ms < 0 表示 client 比房间权威进度慢。
    // 当前阶段只打印，不做 seek 或倍速校正。
    std::cout << "[metric] type=progress_report"
        << " client=" << sample.clientId
        << " sample=" << sampleCount
        << " client_state=" << stateToString(sample.clientState)
        << " room_state=" << stateToString(sample.roomState)
        << " client_pos_ms=" << sample.clientPositionMs
        << " room_pos_ms=" << sample.roomPositionMs
        << " diff_ms=" << diffMs
        << " abs_diff_ms=" << absDiffMs
        << " avg_abs_diff_ms=" << avgAbsDiffMs
        << " max_abs_diff_ms=" << maxAbsDiffMs
        << " min_diff_ms=" << minDiffMs
        << " max_diff_ms=" << maxDiffMs
        << " report_interval_ms=" << reportIntervalMs
        << " quality=" << syncQualityFromDiff(absDiffMs)
        << "\n";
}

void SyncMetricsCollector::removeClient(int clientId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    statsByClient_.erase(clientId);
}
