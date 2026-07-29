#include "SyncCorrectionPolicy.h"

namespace
{
    long long absoluteValue(long long value)
    {
        return value < 0 ? -value : value;
    }
}

SyncCorrectionDecision evaluateSyncCorrection(
    const SyncCorrectionInput& input,
    const SyncCorrectionPolicyConfig& config)
{
    SyncCorrectionDecision decision;

    // 按“数据是否可信 -> 偏差是否足够大 -> 是否持续稳定 -> 是否允许执行”
    // 的顺序逐层放行。每个提前返回的 reason 都能在日志中解释为什么没有校正。
    if (input.settling)
    {
        decision.reason = SyncCorrectionReason::Settling;
        return decision;
    }

    if (!input.windowReady)
    {
        decision.reason = SyncCorrectionReason::WindowNotReady;
        return decision;
    }

    if (input.playbackState == PlaybackState::Stopped)
    {
        decision.reason = SyncCorrectionReason::PlaybackInactive;
        return decision;
    }

    // Playing 样本需要用 RTT/2 投影到 server 比较时刻。
    // Paused 时位置不再前进，网络传播时间不会改变报告的位置，因此不强制要求 RTT。
    if (input.playbackState == PlaybackState::Playing &&
        (!input.hasRttA || !input.hasRttB))
    {
        decision.reason = SyncCorrectionReason::MissingNetworkEstimate;
        return decision;
    }

    if (input.medianAbsDiffMs <= config.toleranceMs)
    {
        decision.reason = SyncCorrectionReason::WithinTolerance;
        return decision;
    }

    if (input.medianAbsDiffMs < config.seekEnterThresholdMs)
    {
        decision.reason = SyncCorrectionReason::BelowSeekThreshold;
        return decision;
    }

    if (input.medianDiffMs == 0 ||
        input.directionAgreementPercent < config.minimumDirectionAgreementPercent)
    {
        decision.reason = SyncCorrectionReason::UnstableDirection;
        return decision;
    }

    if (input.consecutiveSevereSamples <
        config.minimumConsecutiveSevereSamples)
    {
        decision.reason = SyncCorrectionReason::InsufficientPersistence;
        return decision;
    }

    if (input.cooldownActive)
    {
        decision.reason = SyncCorrectionReason::Cooldown;
        return decision;
    }

    decision.action = SyncCorrectionAction::WouldSeekForward;
    decision.reason = SyncCorrectionReason::PersistentSkew;
    decision.suggestedForwardMs = absoluteValue(input.medianDiffMs);

    if (input.medianDiffMs > 0)
    {
        // A 的进度更大，B 落后。
        decision.targetClientId = input.clientBId;
        decision.referenceClientId = input.clientAId;
    }
    else
    {
        // B 的进度更大，A 落后。
        decision.targetClientId = input.clientAId;
        decision.referenceClientId = input.clientBId;
    }

    return decision;
}

const char* syncCorrectionActionToString(SyncCorrectionAction action)
{
    switch (action)
    {
    case SyncCorrectionAction::WouldSeekForward:
        return "would_seek_forward";
    case SyncCorrectionAction::Hold:
    default:
        return "hold";
    }
}

const char* syncCorrectionReasonToString(SyncCorrectionReason reason)
{
    switch (reason)
    {
    case SyncCorrectionReason::Settling:
        return "settling";
    case SyncCorrectionReason::WindowNotReady:
        return "window_not_ready";
    case SyncCorrectionReason::PlaybackInactive:
        return "playback_inactive";
    case SyncCorrectionReason::MissingNetworkEstimate:
        return "missing_network_estimate";
    case SyncCorrectionReason::WithinTolerance:
        return "within_tolerance";
    case SyncCorrectionReason::BelowSeekThreshold:
        return "below_seek_threshold";
    case SyncCorrectionReason::UnstableDirection:
        return "unstable_direction";
    case SyncCorrectionReason::InsufficientPersistence:
        return "insufficient_persistence";
    case SyncCorrectionReason::Cooldown:
        return "cooldown";
    case SyncCorrectionReason::PersistentSkew:
        return "persistent_skew";
    default:
        return "unknown";
    }
}
