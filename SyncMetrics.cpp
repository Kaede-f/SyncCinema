#include "SyncMetrics.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace
{
    constexpr std::size_t kMaxPendingPingsPerClient = 64;
    constexpr auto kMaxPairReportAge = std::chrono::milliseconds(2500);
    constexpr auto kControlSettleDuration = std::chrono::milliseconds(2000);
    constexpr std::size_t kPairWindowSize = 12;
    constexpr std::size_t kMinReadyPairWindowSamples = 6;
    constexpr SyncCorrectionPolicyConfig kCorrectionPolicyConfig{};
    constexpr auto kCorrectionAdviceCooldown = std::chrono::milliseconds(5000);
    constexpr auto kCorrectionAdviceLogInterval = std::chrono::milliseconds(5000);
    constexpr long long kSeverePairSkewMs =
        kCorrectionPolicyConfig.seekEnterThresholdMs;

    using MetricsClock = SyncMetricsCollector::Clock;

    struct PairProgressMetric
    {
        int triggerClientId = 0;
        int clientAId = 0;
        int clientBId = 0;
        long long controlEpoch = 0;
        PlaybackState playbackState = PlaybackState::Stopped;
        long long projectedClientAPositionMs = 0;
        long long projectedClientBPositionMs = 0;
        long long pairDiffMs = 0;
        long long pairAbsDiffMs = 0;
        long long reportAgeAMs = 0;
        long long reportAgeBMs = 0;
        long long estimatedOneWayDelayAMs = 0;
        long long estimatedOneWayDelayBMs = 0;
        bool settling = false;
        bool windowReady = false;
        std::size_t windowSamples = 0;
        long long medianDiffMs = 0;
        long long medianAbsDiffMs = 0;
        long long p95AbsDiffMs = 0;
        int consecutiveSevereSamples = 0;
        int directionAgreementPercent = 0;
        long long qualityBasisAbsDiffMs = 0;
        bool hasRttA = false;
        bool hasRttB = false;
        long long cooldownRemainingMs = 0;
        SyncCorrectionDecision correctionDecision;
        bool emitCorrectionAdvice = false;
    };

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

    long long elapsedMilliseconds(
        MetricsClock::time_point earlier,
        MetricsClock::time_point later)
    {
        long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            later - earlier
        ).count();
        return std::max(0LL, elapsed);
    }

    long long projectPositionToServerTime(
        long long reportedPositionMs,
        PlaybackState playbackState,
        MetricsClock::time_point reportReceivedAt,
        MetricsClock::time_point targetTime,
        long long estimatedOneWayDelayMs)
    {
        if (playbackState != PlaybackState::Playing)
        {
            return reportedPositionMs;
        }

        // REPORT 中的位置是在 client 发送前采样的。
        // server 收到它时，播放器理论上已经继续播放了约 one_way_ms；
        // 对较早收到的另一份上报，还要加上它从到达到当前比较时刻的经过时间。
        return reportedPositionMs +
            std::max(0LL, estimatedOneWayDelayMs) +
            elapsedMilliseconds(reportReceivedAt, targetTime);
    }

    std::uint64_t makePairKey(int clientAId, int clientBId)
    {
        std::uint64_t high = static_cast<std::uint32_t>(clientAId);
        std::uint64_t low = static_cast<std::uint32_t>(clientBId);
        return (high << 32) | low;
    }

    long long medianOf(std::vector<long long> values)
    {
        if (values.empty())
        {
            return 0;
        }

        std::sort(values.begin(), values.end());
        std::size_t middle = values.size() / 2;
        if (values.size() % 2 == 1)
        {
            return values[middle];
        }

        long long lower = values[middle - 1];
        long long upper = values[middle];
        return lower + (upper - lower) / 2;
    }

    long long percentile95OfAbsoluteDiffs(const std::deque<long long>& diffValues)
    {
        if (diffValues.empty())
        {
            return 0;
        }

        std::vector<long long> absoluteValues;
        absoluteValues.reserve(diffValues.size());
        for (long long diff : diffValues)
        {
            absoluteValues.push_back(absLongLong(diff));
        }

        std::sort(absoluteValues.begin(), absoluteValues.end());

        // nearest-rank P95：向上取第 ceil(N * 0.95) 个样本。
        std::size_t rank = (absoluteValues.size() * 95 + 99) / 100;
        return absoluteValues[rank - 1];
    }

    int calculateDirectionAgreementPercent(
        const std::deque<long long>& diffValues,
        long long medianDiffMs)
    {
        if (diffValues.empty() || medianDiffMs == 0)
        {
            return 0;
        }

        std::size_t matchingDirectionCount = 0;
        for (long long diffMs : diffValues)
        {
            if ((medianDiffMs > 0 && diffMs > 0) ||
                (medianDiffMs < 0 && diffMs < 0))
            {
                ++matchingDirectionCount;
            }
        }

        return static_cast<int>(
            matchingDirectionCount * 100 / diffValues.size()
        );
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

void SyncMetricsCollector::beginControlEpoch(
    long long controlEpoch,
    const SyncMessage& controlMessage)
{
    if (!isPlaybackControlMessage(controlMessage.type) || controlEpoch <= 0)
    {
        return;
    }

    Clock::time_point now = Clock::now();
    long long newEpoch = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        currentControlEpoch_ = controlEpoch;
        newEpoch = currentControlEpoch_;
        hasControlEpochStart_ = true;
        controlEpochStartedAt_ = now;
        pairStatsByKey_.clear();

        for (auto& [clientId, stats] : statsByClient_)
        {
            (void)clientId;

            // RTT 反映网络链路，不因播放命令改变，所以保留 RTT 统计。
            // 下面只清理与“本次播放控制结果”相关的进度和偏差数据。
            stats.sampleCount = 0;
            stats.totalAbsDiffMs = 0;
            stats.minDiffMs = 0;
            stats.maxDiffMs = 0;
            stats.maxAbsDiffMs = 0;
            stats.hasDiff = false;
            stats.hasLastReportTime = false;
            stats.lastReportTime = {};
            stats.hasLatestProgressReport = false;
            stats.latestPositionMs = 0;
            stats.latestPlaybackState = PlaybackState::Stopped;
            stats.latestReportReceivedAt = {};
        }
    }

    {
        std::lock_guard<std::mutex> logLock(logMutex_);
        std::cout << "[metric] type=control_epoch"
            << " epoch=" << newEpoch
            << " command=" << messageTypeToString(controlMessage.type)
            << " settle_ms=" << kControlSettleDuration.count()
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
    long long controlEpoch = 0;
    bool settling = false;
    std::vector<PairProgressMetric> pairMetrics;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        controlEpoch = currentControlEpoch_;
        settling = hasControlEpochStart_ &&
            (now - controlEpochStartedAt_) < kControlSettleDuration;

        ClientStats& stats = statsByClient_[sample.clientId];
        if (stats.hasLastReportTime)
        {
            reportIntervalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - stats.lastReportTime
            ).count();
        }

        stats.lastReportTime = now;
        stats.hasLastReportTime = true;

        if (!settling)
        {
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

        stats.hasLatestProgressReport = true;
        stats.latestPositionMs = sample.clientPositionMs;
        stats.latestPlaybackState = sample.clientState;
        stats.latestReportReceivedAt = now;

        const long long currentProjectedPositionMs = projectPositionToServerTime(
            sample.clientPositionMs,
            sample.clientState,
            now,
            now,
            estimatedOneWayDelayMs
        );

        for (const auto& [otherClientId, otherStats] : statsByClient_)
        {
            if (otherClientId == sample.clientId || !otherStats.hasLatestProgressReport)
            {
                continue;
            }

            long long otherReportAgeMs = elapsedMilliseconds(
                otherStats.latestReportReceivedAt,
                now
            );

            // REPORT 默认每秒发送一次。超过 2.5 秒仍未更新通常表示网络阻塞、
            // client 卡顿或正在退出；陈旧样本不应参与“当前同步程度”的判断。
            if (otherReportAgeMs > kMaxPairReportAge.count())
            {
                pairStatsByKey_.erase(makePairKey(
                    std::min(sample.clientId, otherClientId),
                    std::max(sample.clientId, otherClientId)
                ));
                continue;
            }

            // Playing 与 Paused 的位置推进规则不同，直接比较会失去物理意义。
            // 状态不一致本身会在 progress_report 的 client_state/room_state 中体现。
            if (otherStats.latestPlaybackState != sample.clientState)
            {
                // 状态不一致说明这对 client 的连续证据已经断开。
                // 清空旧窗口，防止状态重新一致后立即沿用过期趋势给出校正建议。
                pairStatsByKey_.erase(makePairKey(
                    std::min(sample.clientId, otherClientId),
                    std::max(sample.clientId, otherClientId)
                ));
                continue;
            }

            long long otherOneWayDelayMs = otherStats.minRttMs >= 0
                ? otherStats.minRttMs / 2
                : 0;
            long long otherProjectedPositionMs = projectPositionToServerTime(
                otherStats.latestPositionMs,
                otherStats.latestPlaybackState,
                otherStats.latestReportReceivedAt,
                now,
                otherOneWayDelayMs
            );

            PairProgressMetric pairMetric;
            pairMetric.triggerClientId = sample.clientId;
            pairMetric.controlEpoch = controlEpoch;
            pairMetric.playbackState = sample.clientState;
            pairMetric.settling = settling;

            // client_a/client_b 始终按 id 排序，使 pair_diff_ms 的正负含义稳定：
            // pair_diff_ms > 0 表示 client_a 比 client_b 更靠前。
            if (sample.clientId < otherClientId)
            {
                pairMetric.clientAId = sample.clientId;
                pairMetric.clientBId = otherClientId;
                pairMetric.projectedClientAPositionMs = currentProjectedPositionMs;
                pairMetric.projectedClientBPositionMs = otherProjectedPositionMs;
                pairMetric.reportAgeAMs = 0;
                pairMetric.reportAgeBMs = otherReportAgeMs;
                pairMetric.estimatedOneWayDelayAMs = estimatedOneWayDelayMs;
                pairMetric.estimatedOneWayDelayBMs = otherOneWayDelayMs;
                pairMetric.hasRttA = stats.minRttMs >= 0;
                pairMetric.hasRttB = otherStats.minRttMs >= 0;
            }
            else
            {
                pairMetric.clientAId = otherClientId;
                pairMetric.clientBId = sample.clientId;
                pairMetric.projectedClientAPositionMs = otherProjectedPositionMs;
                pairMetric.projectedClientBPositionMs = currentProjectedPositionMs;
                pairMetric.reportAgeAMs = otherReportAgeMs;
                pairMetric.reportAgeBMs = 0;
                pairMetric.estimatedOneWayDelayAMs = otherOneWayDelayMs;
                pairMetric.estimatedOneWayDelayBMs = estimatedOneWayDelayMs;
                pairMetric.hasRttA = otherStats.minRttMs >= 0;
                pairMetric.hasRttB = stats.minRttMs >= 0;
            }

            pairMetric.pairDiffMs =
                pairMetric.projectedClientAPositionMs -
                pairMetric.projectedClientBPositionMs;
            pairMetric.pairAbsDiffMs = absLongLong(pairMetric.pairDiffMs);

            std::uint64_t pairKey = makePairKey(
                pairMetric.clientAId,
                pairMetric.clientBId
            );
            PairWindowStats& pairStats = pairStatsByKey_[pairKey];
            pairStats.clientAId = pairMetric.clientAId;
            pairStats.clientBId = pairMetric.clientBId;

            if (!settling)
            {
                pairStats.recentDiffMs.push_back(pairMetric.pairDiffMs);
                if (pairStats.recentDiffMs.size() > kPairWindowSize)
                {
                    pairStats.recentDiffMs.pop_front();
                }

                // 与策略层的 seekEnterThresholdMs 保持同一边界语义：
                // 达到阈值（>=），就算作一次严重偏差样本。
                if (pairMetric.pairAbsDiffMs >= kSeverePairSkewMs)
                {
                    ++pairStats.consecutiveSevereSamples;
                }
                else
                {
                    pairStats.consecutiveSevereSamples = 0;
                }
            }

            std::vector<long long> signedDiffs(
                pairStats.recentDiffMs.begin(),
                pairStats.recentDiffMs.end()
            );
            std::vector<long long> absoluteDiffs;
            absoluteDiffs.reserve(signedDiffs.size());
            for (long long value : signedDiffs)
            {
                absoluteDiffs.push_back(absLongLong(value));
            }

            pairMetric.windowSamples = pairStats.recentDiffMs.size();
            pairMetric.windowReady =
                pairMetric.windowSamples >= kMinReadyPairWindowSamples;
            pairMetric.medianDiffMs = medianOf(signedDiffs);
            pairMetric.medianAbsDiffMs = medianOf(absoluteDiffs);
            pairMetric.p95AbsDiffMs =
                percentile95OfAbsoluteDiffs(pairStats.recentDiffMs);
            pairMetric.consecutiveSevereSamples =
                pairStats.consecutiveSevereSamples;
            pairMetric.directionAgreementPercent =
                calculateDirectionAgreementPercent(
                    pairStats.recentDiffMs,
                    pairMetric.medianDiffMs
                );
            pairMetric.qualityBasisAbsDiffMs = pairMetric.windowReady
                ? pairMetric.medianAbsDiffMs
                : pairMetric.pairAbsDiffMs;

            SyncCorrectionInput correctionInput;
            correctionInput.clientAId = pairMetric.clientAId;
            correctionInput.clientBId = pairMetric.clientBId;
            correctionInput.controlEpoch = pairMetric.controlEpoch;
            correctionInput.playbackState = pairMetric.playbackState;
            correctionInput.settling = pairMetric.settling;
            correctionInput.windowReady = pairMetric.windowReady;
            correctionInput.windowSamples = pairMetric.windowSamples;
            correctionInput.medianDiffMs = pairMetric.medianDiffMs;
            correctionInput.medianAbsDiffMs = pairMetric.medianAbsDiffMs;
            correctionInput.p95AbsDiffMs = pairMetric.p95AbsDiffMs;
            correctionInput.consecutiveSevereSamples =
                pairMetric.consecutiveSevereSamples;
            correctionInput.directionAgreementPercent =
                pairMetric.directionAgreementPercent;
            correctionInput.hasRttA = pairMetric.hasRttA;
            correctionInput.hasRttB = pairMetric.hasRttB;

            if (pairStats.hasLastWouldCorrectTime)
            {
                long long elapsedSinceAdviceMs = elapsedMilliseconds(
                    pairStats.lastWouldCorrectTime,
                    now
                );
                if (elapsedSinceAdviceMs < kCorrectionAdviceCooldown.count())
                {
                    correctionInput.cooldownActive = true;
                    correctionInput.cooldownRemainingMs =
                        kCorrectionAdviceCooldown.count() - elapsedSinceAdviceMs;
                }
            }

            pairMetric.cooldownRemainingMs =
                correctionInput.cooldownRemainingMs;
            pairMetric.correctionDecision = evaluateSyncCorrection(
                correctionInput,
                kCorrectionPolicyConfig
            );

            if (pairMetric.correctionDecision.action ==
                SyncCorrectionAction::WouldSeekForward)
            {
                // 这里只记录“如果闭环已经启用，本次会执行校正”。
                // 记录时间用于模拟未来控制器的冷却，但绝不调用 seek。
                pairStats.hasLastWouldCorrectTime = true;
                pairStats.lastWouldCorrectTime = now;
            }

            bool decisionChanged =
                !pairStats.hasLastLoggedDecision ||
                pairStats.lastLoggedAction !=
                    pairMetric.correctionDecision.action ||
                pairStats.lastLoggedReason !=
                    pairMetric.correctionDecision.reason;
            bool periodicSummaryDue =
                !pairStats.hasLastLoggedDecision ||
                elapsedMilliseconds(pairStats.lastDecisionLoggedAt, now) >=
                    kCorrectionAdviceLogInterval.count();

            pairMetric.emitCorrectionAdvice =
                decisionChanged || periodicSummaryDue;

            if (pairMetric.emitCorrectionAdvice)
            {
                pairStats.hasLastLoggedDecision = true;
                pairStats.lastLoggedAction =
                    pairMetric.correctionDecision.action;
                pairStats.lastLoggedReason =
                    pairMetric.correctionDecision.reason;
                pairStats.lastDecisionLoggedAt = now;
            }

            pairMetrics.push_back(pairMetric);
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
            << " epoch=" << controlEpoch
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
            << " settling=" << (settling ? 1 : 0)
            << " quality=" << (
                settling ? "settling" : syncQualityFromDiff(compensatedAbsDiffMs)
            )
            << "\n";

        for (const PairProgressMetric& pairMetric : pairMetrics)
        {
            std::cout << "[metric] type=pair_progress"
                << " trigger_client=" << pairMetric.triggerClientId
                << " client_a=" << pairMetric.clientAId
                << " client_b=" << pairMetric.clientBId
                << " epoch=" << pairMetric.controlEpoch
                << " state=" << stateToString(pairMetric.playbackState)
                << " projected_a_ms=" << pairMetric.projectedClientAPositionMs
                << " projected_b_ms=" << pairMetric.projectedClientBPositionMs
                << " pair_diff_ms=" << pairMetric.pairDiffMs
                << " pair_abs_diff_ms=" << pairMetric.pairAbsDiffMs
                << " report_age_a_ms=" << pairMetric.reportAgeAMs
                << " report_age_b_ms=" << pairMetric.reportAgeBMs
                << " one_way_a_ms=" << pairMetric.estimatedOneWayDelayAMs
                << " one_way_b_ms=" << pairMetric.estimatedOneWayDelayBMs
                << " settling=" << (pairMetric.settling ? 1 : 0)
                << " window_samples=" << pairMetric.windowSamples
                << " window_ready=" << (pairMetric.windowReady ? 1 : 0)
                << " median_diff_ms=" << pairMetric.medianDiffMs
                << " median_abs_diff_ms=" << pairMetric.medianAbsDiffMs
                << " p95_abs_diff_ms=" << pairMetric.p95AbsDiffMs
                << " consecutive_severe=" << pairMetric.consecutiveSevereSamples
                << " direction_agreement_pct="
                << pairMetric.directionAgreementPercent
                << " quality=" << (
                    pairMetric.settling
                        ? "settling"
                        : syncQualityFromDiff(pairMetric.qualityBasisAbsDiffMs)
                )
                << "\n";

            if (pairMetric.emitCorrectionAdvice)
            {
                const SyncCorrectionDecision& decision =
                    pairMetric.correctionDecision;

                std::cout << "[metric] type=correction_advice"
                    << " mode=read_only"
                    << " client_a=" << pairMetric.clientAId
                    << " client_b=" << pairMetric.clientBId
                    << " epoch=" << pairMetric.controlEpoch
                    << " state=" << stateToString(pairMetric.playbackState)
                    << " action="
                    << syncCorrectionActionToString(decision.action)
                    << " reason="
                    << syncCorrectionReasonToString(decision.reason)
                    << " target_client=" << decision.targetClientId
                    << " reference_client=" << decision.referenceClientId
                    << " suggested_forward_ms=" << decision.suggestedForwardMs
                    << " median_diff_ms=" << pairMetric.medianDiffMs
                    << " median_abs_diff_ms=" << pairMetric.medianAbsDiffMs
                    << " p95_abs_diff_ms=" << pairMetric.p95AbsDiffMs
                    << " direction_agreement_pct="
                    << pairMetric.directionAgreementPercent
                    << " consecutive_severe="
                    << pairMetric.consecutiveSevereSamples
                    << " window_samples=" << pairMetric.windowSamples
                    << " has_rtt_a=" << (pairMetric.hasRttA ? 1 : 0)
                    << " has_rtt_b=" << (pairMetric.hasRttB ? 1 : 0)
                    << " cooldown_remaining_ms="
                    << pairMetric.cooldownRemainingMs
                    << " tolerance_ms="
                    << kCorrectionPolicyConfig.toleranceMs
                    << " seek_enter_ms="
                    << kCorrectionPolicyConfig.seekEnterThresholdMs
                    << "\n";
            }
        }
    }
}

void SyncMetricsCollector::removeClient(int clientId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    statsByClient_.erase(clientId);

    for (auto it = pairStatsByKey_.begin(); it != pairStatsByKey_.end();)
    {
        if (it->second.clientAId == clientId || it->second.clientBId == clientId)
        {
            it = pairStatsByKey_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
