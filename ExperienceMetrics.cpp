#include "ExperienceMetrics.h"

#include <algorithm>

namespace
{
    constexpr std::array<long long, 13> kHistogramUpperBoundsMs{
        10,
        25,
        50,
        100,
        250,
        500,
        1000,
        2000,
        3000,
        5000,
        10000,
        30000,
        60000
    };
    constexpr auto kControlAckTimeout = std::chrono::seconds(10);
    constexpr auto kCorrectionConvergenceTimeout = std::chrono::seconds(15);
    constexpr long long kGoodPairSkewMs = 250;
    constexpr int kConsecutiveGoodSamplesForConvergence = 2;

    long long elapsedMilliseconds(
        ExperienceMetricsCollector::Clock::time_point startedAt,
        ExperienceMetricsCollector::Clock::time_point endedAt)
    {
        const long long value =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                endedAt - startedAt
            ).count();
        return (std::max)(0LL, value);
    }

    bool samePair(
        int leftA,
        int leftB,
        int rightA,
        int rightB)
    {
        return (leftA == rightA && leftB == rightB) ||
            (leftA == rightB && leftB == rightA);
    }
}

void ExperienceMetricsCollector::MillisecondHistogram::observe(long long value)
{
    const long long normalized = (std::max)(0LL, value);
    ++sampleCount;
    maximum = (std::max)(maximum, normalized);

    for (std::size_t index = 0; index < kHistogramUpperBoundsMs.size(); ++index)
    {
        if (normalized <= kHistogramUpperBoundsMs[index])
        {
            ++counts[index];
            return;
        }
    }

    // 独立的最后一桶承担 +Inf。maximum 保留真实最大值，避免长尾被截断。
    ++counts.back();
}

long long ExperienceMetricsCollector::MillisecondHistogram::
percentile95UpperBound() const
{
    if (sampleCount == 0)
    {
        return -1;
    }

    const long long rank = (sampleCount * 95 + 99) / 100;
    long long cumulative = 0;
    for (std::size_t index = 0; index < counts.size(); ++index)
    {
        cumulative += counts[index];
        if (cumulative >= rank)
        {
            return index < kHistogramUpperBoundsMs.size()
                ? kHistogramUpperBoundsMs[index]
                : maximum;
        }
    }
    return maximum;
}

void ExperienceMetricsCollector::MillisecondHistogram::reset()
{
    counts.fill(0);
    sampleCount = 0;
    maximum = -1;
}

void ExperienceMetricsCollector::beginSession(const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    clearLocked();
    sessionId_ = sessionId;
}

void ExperienceMetricsCollector::recordClientJoined(
    int clientId,
    bool firstFrameExpected)
{
    if (clientId <= 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    joinedClients_.insert(clientId);
    if (firstFrameExpected)
    {
        firstFrameExpectedClients_.insert(clientId);
    }
}

FirstFrameRecord ExperienceMetricsCollector::recordFirstFrame(
    int clientId,
    const SyncMessage& message)
{
    FirstFrameRecord record;
    record.clientId = clientId;
    record.controlEpoch = message.controlEpoch;
    record.trigger = message.firstFrameTrigger;
    record.durationMilliseconds = message.firstFrameMilliseconds;

    if (clientId <= 0 || message.type != MessageType::FirstFrame ||
        message.controlEpoch < 0 ||
        message.firstFrameTrigger == FirstFrameTrigger::None ||
        message.firstFrameMilliseconds < 0)
    {
        return record;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (joinedClients_.find(clientId) == joinedClients_.end())
    {
        record.status = FirstFrameRecordStatus::UnknownClient;
        return record;
    }

    if (!firstFrameClients_.insert(clientId).second)
    {
        record.status = FirstFrameRecordStatus::Duplicate;
        return record;
    }

    // 收到真实首帧报告时兜底纳入分母，可覆盖 PLAY 广播与客户端回报
    // 几乎同时发生的情况，也避免并发时序让成功率超过 100%。
    firstFrameExpectedClients_.insert(clientId);
    firstFrameHistogram_.observe(message.firstFrameMilliseconds);
    record.status = FirstFrameRecordStatus::Recorded;
    return record;
}

bool ExperienceMetricsCollector::recordControlDispatched(
    long long controlEpoch,
    MessageType controlType,
    const std::vector<int>& targetClientIds,
    Clock::time_point sentAt)
{
    if (controlEpoch <= 0 || !isPlaybackControlMessage(controlType))
    {
        return false;
    }

    PendingControl pending;
    pending.controlType = controlType;
    pending.sentAt = sentAt;
    for (int clientId : targetClientIds)
    {
        if (clientId > 0)
        {
            pending.expectedClients.insert(clientId);
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    pruneExpiredControlsLocked(sentAt);
    if (pendingControls_.find(controlEpoch) != pendingControls_.end())
    {
        return false;
    }

    if (controlType == MessageType::Play)
    {
        firstFrameExpectedClients_.insert(
            pending.expectedClients.begin(),
            pending.expectedClients.end()
        );
    }

    controlExpectedAcks_ += static_cast<long long>(
        pending.expectedClients.size()
    );
    if (!pending.expectedClients.empty())
    {
        pendingControls_.emplace(controlEpoch, std::move(pending));
    }
    return true;
}

ControlAckRecord ExperienceMetricsCollector::recordControlResult(
    int clientId,
    const SyncMessage& message,
    Clock::time_point receivedAt)
{
    ControlAckRecord record;
    record.clientId = clientId;
    record.controlEpoch = message.controlEpoch;
    record.controlType = message.relatedControlType;
    record.resultStatus = message.controlResultStatus;
    record.clientApplyMilliseconds = message.controlApplyMilliseconds;
    record.actualPositionMilliseconds = message.positionMilliseconds;
    record.reason = message.controlResultReason;

    if (clientId <= 0 || message.type != MessageType::ControlResult ||
        message.controlEpoch <= 0 ||
        !isPlaybackControlMessage(message.relatedControlType) ||
        message.controlResultStatus == ControlResultStatus::None ||
        message.controlApplyMilliseconds < 0 ||
        message.positionMilliseconds < 0)
    {
        return record;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    pruneExpiredControlsLocked(receivedAt);

    auto pendingIt = pendingControls_.find(message.controlEpoch);
    if (pendingIt == pendingControls_.end())
    {
        record.matchStatus = ControlAckMatchStatus::UnknownEpoch;
        return record;
    }

    PendingControl& pending = pendingIt->second;
    if (pending.expectedClients.find(clientId) ==
        pending.expectedClients.end())
    {
        record.matchStatus = ControlAckMatchStatus::UnexpectedClient;
        return record;
    }
    if (pending.controlType != message.relatedControlType)
    {
        record.matchStatus = ControlAckMatchStatus::CommandMismatch;
        return record;
    }
    if (!pending.respondedClients.insert(clientId).second)
    {
        record.matchStatus = ControlAckMatchStatus::Duplicate;
        return record;
    }

    record.matchStatus = ControlAckMatchStatus::Matched;
    record.acknowledgementLatencyMilliseconds = elapsedMilliseconds(
        pending.sentAt,
        receivedAt
    );
    ++controlMatchedAcks_;
    controlAckHistogram_.observe(record.acknowledgementLatencyMilliseconds);
    clientApplyHistogram_.observe(message.controlApplyMilliseconds);
    if (message.controlResultStatus == ControlResultStatus::Applied)
    {
        ++controlAppliedAcks_;
    }
    else
    {
        ++controlRejectedAcks_;
    }

    if (pending.respondedClients.size() == pending.expectedClients.size())
    {
        pendingControls_.erase(pendingIt);
    }
    return record;
}

std::vector<CorrectionConvergenceRecord>
ExperienceMetricsCollector::recordPairSample(
    const ExperiencePairSample& sample,
    Clock::time_point observedAt)
{
    std::vector<CorrectionConvergenceRecord> converged;
    if (!sample.valid || sample.clientAId <= 0 || sample.clientBId <= 0 ||
        sample.clientAId == sample.clientBId || sample.controlEpoch < 0 ||
        sample.absoluteDifferenceMilliseconds < 0)
    {
        return converged;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    pruneExpiredCorrectionsLocked(observedAt);

    ++validPairSamples_;
    pairSkewHistogram_.observe(sample.absoluteDifferenceMilliseconds);
    if (sample.absoluteDifferenceMilliseconds <= kGoodPairSkewMs)
    {
        ++goodPairSamples_;
    }

    for (auto it = pendingCorrections_.begin();
        it != pendingCorrections_.end();)
    {
        PendingCorrection& pending = it->second;
        if (!pending.applied || pending.controlEpoch != sample.controlEpoch ||
            !samePair(
                pending.targetClientId,
                pending.referenceClientId,
                sample.clientAId,
                sample.clientBId))
        {
            ++it;
            continue;
        }

        if (sample.absoluteDifferenceMilliseconds <= kGoodPairSkewMs)
        {
            ++pending.consecutiveGoodSamples;
        }
        else
        {
            pending.consecutiveGoodSamples = 0;
        }

        if (pending.consecutiveGoodSamples <
            kConsecutiveGoodSamplesForConvergence)
        {
            ++it;
            continue;
        }

        CorrectionConvergenceRecord record;
        record.commandId = pending.commandId;
        record.controlEpoch = pending.controlEpoch;
        record.targetClientId = pending.targetClientId;
        record.referenceClientId = pending.referenceClientId;
        record.convergenceMilliseconds = elapsedMilliseconds(
            pending.sentAt,
            observedAt
        );
        record.finalAbsoluteDifferenceMilliseconds =
            sample.absoluteDifferenceMilliseconds;
        correctionConvergenceHistogram_.observe(
            record.convergenceMilliseconds
        );
        ++correctionsConverged_;
        converged.push_back(record);
        it = pendingCorrections_.erase(it);
    }
    return converged;
}

void ExperienceMetricsCollector::recordCorrectionDispatched(
    long long commandId,
    long long controlEpoch,
    int targetClientId,
    int referenceClientId,
    Clock::time_point sentAt)
{
    if (commandId <= 0 || controlEpoch <= 0 || targetClientId <= 0 ||
        referenceClientId <= 0 || targetClientId == referenceClientId)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    pruneExpiredCorrectionsLocked(sentAt);
    PendingCorrection pending;
    pending.commandId = commandId;
    pending.controlEpoch = controlEpoch;
    pending.targetClientId = targetClientId;
    pending.referenceClientId = referenceClientId;
    pending.sentAt = sentAt;
    if (pendingCorrections_.emplace(commandId, pending).second)
    {
        ++correctionsDispatched_;
    }
}

void ExperienceMetricsCollector::recordCorrectionResult(
    long long commandId,
    bool matched,
    bool applied)
{
    if (!matched || commandId <= 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto pendingIt = pendingCorrections_.find(commandId);
    if (pendingIt == pendingCorrections_.end())
    {
        return;
    }

    if (applied)
    {
        if (!pendingIt->second.applied)
        {
            pendingIt->second.applied = true;
            ++correctionsApplied_;
        }
    }
    else
    {
        ++correctionsRejected_;
        pendingCorrections_.erase(pendingIt);
    }
}

void ExperienceMetricsCollector::removeClient(int clientId)
{
    if (clientId <= 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = pendingControls_.begin(); it != pendingControls_.end();)
    {
        it->second.expectedClients.erase(clientId);
        it->second.respondedClients.erase(clientId);
        if (it->second.expectedClients.empty() ||
            it->second.respondedClients.size() ==
                it->second.expectedClients.size())
        {
            it = pendingControls_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = pendingCorrections_.begin();
        it != pendingCorrections_.end();)
    {
        if (it->second.targetClientId == clientId ||
            it->second.referenceClientId == clientId)
        {
            it = pendingCorrections_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

ExperienceSessionSummary ExperienceMetricsCollector::endSession(
    Clock::time_point endedAt)
{
    std::lock_guard<std::mutex> lock(mutex_);
    pruneExpiredControlsLocked(endedAt);
    pruneExpiredCorrectionsLocked(endedAt);

    ExperienceSessionSummary summary;
    summary.sessionId = sessionId_;
    summary.joinedClients = static_cast<long long>(joinedClients_.size());
    summary.firstFrameExpected = static_cast<long long>(
        firstFrameExpectedClients_.size()
    );
    summary.firstFrameReports = static_cast<long long>(
        firstFrameClients_.size()
    );
    summary.firstFrameMissing = (std::max)(
        0LL,
        summary.firstFrameExpected - summary.firstFrameReports
    );
    summary.firstFrameP95BucketMilliseconds =
        firstFrameHistogram_.percentile95UpperBound();

    summary.controlExpectedAcks = controlExpectedAcks_;
    summary.controlMatchedAcks = controlMatchedAcks_;
    summary.controlAppliedAcks = controlAppliedAcks_;
    summary.controlRejectedAcks = controlRejectedAcks_;
    summary.controlMissingAcks = (std::max)(
        0LL,
        controlExpectedAcks_ - controlMatchedAcks_
    );
    summary.controlAckP95BucketMilliseconds =
        controlAckHistogram_.percentile95UpperBound();
    summary.clientApplyP95BucketMilliseconds =
        clientApplyHistogram_.percentile95UpperBound();

    summary.validPairSamples = validPairSamples_;
    summary.goodPairSamples = goodPairSamples_;
    summary.syncGoodRatio = validPairSamples_ == 0
        ? 0.0
        : static_cast<double>(goodPairSamples_) /
            static_cast<double>(validPairSamples_);
    summary.pairSkewP95BucketMilliseconds =
        pairSkewHistogram_.percentile95UpperBound();
    summary.maximumPairSkewMilliseconds = pairSkewHistogram_.maximum;

    summary.correctionsDispatched = correctionsDispatched_;
    summary.correctionsApplied = correctionsApplied_;
    summary.correctionsRejected = correctionsRejected_;
    summary.correctionsMissingResults = (std::max)(
        0LL,
        correctionsDispatched_ - correctionsApplied_ - correctionsRejected_
    );
    summary.correctionsConverged = correctionsConverged_;
    summary.correctionsNotConverged = (std::max)(
        0LL,
        correctionsApplied_ - correctionsConverged_
    );
    summary.correctionConvergenceP95BucketMilliseconds =
        correctionConvergenceHistogram_.percentile95UpperBound();

    clearLocked();
    return summary;
}

void ExperienceMetricsCollector::pruneExpiredControlsLocked(
    Clock::time_point now)
{
    for (auto it = pendingControls_.begin(); it != pendingControls_.end();)
    {
        if (now - it->second.sentAt >= kControlAckTimeout)
        {
            it = pendingControls_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void ExperienceMetricsCollector::pruneExpiredCorrectionsLocked(
    Clock::time_point now)
{
    for (auto it = pendingCorrections_.begin();
        it != pendingCorrections_.end();)
    {
        if (now - it->second.sentAt >= kCorrectionConvergenceTimeout)
        {
            it = pendingCorrections_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void ExperienceMetricsCollector::clearLocked()
{
    sessionId_.clear();
    joinedClients_.clear();
    firstFrameExpectedClients_.clear();
    firstFrameClients_.clear();
    pendingControls_.clear();
    pendingCorrections_.clear();
    firstFrameHistogram_.reset();
    controlAckHistogram_.reset();
    clientApplyHistogram_.reset();
    pairSkewHistogram_.reset();
    correctionConvergenceHistogram_.reset();
    controlExpectedAcks_ = 0;
    controlMatchedAcks_ = 0;
    controlAppliedAcks_ = 0;
    controlRejectedAcks_ = 0;
    validPairSamples_ = 0;
    goodPairSamples_ = 0;
    correctionsDispatched_ = 0;
    correctionsApplied_ = 0;
    correctionsRejected_ = 0;
    correctionsConverged_ = 0;
}

const char* firstFrameRecordStatusToString(FirstFrameRecordStatus status)
{
    switch (status)
    {
    case FirstFrameRecordStatus::Recorded:
        return "recorded";
    case FirstFrameRecordStatus::Duplicate:
        return "duplicate";
    case FirstFrameRecordStatus::UnknownClient:
        return "unknown_client";
    case FirstFrameRecordStatus::InvalidMessage:
    default:
        return "invalid_message";
    }
}

const char* controlAckMatchStatusToString(ControlAckMatchStatus status)
{
    switch (status)
    {
    case ControlAckMatchStatus::Matched:
        return "matched";
    case ControlAckMatchStatus::UnknownEpoch:
        return "unknown_epoch";
    case ControlAckMatchStatus::UnexpectedClient:
        return "unexpected_client";
    case ControlAckMatchStatus::CommandMismatch:
        return "command_mismatch";
    case ControlAckMatchStatus::Duplicate:
        return "duplicate";
    case ControlAckMatchStatus::InvalidMessage:
    default:
        return "invalid_message";
    }
}
