#pragma once

#include <cstddef>

#include "Protocol.h"

// 只读校正策略的输入。
//
// 这些字段全部来自 SyncMetricsCollector 已经完成时间归一化后的稳健窗口，
// 策略层不访问 socket、Room 或播放器，因此可以独立测试。
struct SyncCorrectionInput
{
    int clientAId = 0;
    int clientBId = 0;
    long long controlEpoch = 0;
    PlaybackState playbackState = PlaybackState::Stopped;

    bool settling = false;
    bool windowReady = false;
    std::size_t windowSamples = 0;

    long long medianDiffMs = 0;
    long long medianAbsDiffMs = 0;
    long long p95AbsDiffMs = 0;
    int consecutiveSevereSamples = 0;
    int directionAgreementPercent = 0;

    bool hasRttA = false;
    bool hasRttB = false;
    bool cooldownActive = false;
    long long cooldownRemainingMs = 0;
};

// 阈值集中在配置对象中，避免散落在日志代码和未来控制代码里。
// 当前值是 MVP 的保守起点，后续应根据真实设备测试数据调整。
struct SyncCorrectionPolicyConfig
{
    long long toleranceMs = 250;
    long long seekEnterThresholdMs = 750;
    int minimumConsecutiveSevereSamples = 3;
    int minimumDirectionAgreementPercent = 75;
};

enum class SyncCorrectionAction
{
    Hold,
    WouldSeekForward
};

enum class SyncCorrectionReason
{
    Settling,
    WindowNotReady,
    PlaybackInactive,
    MissingNetworkEstimate,
    WithinTolerance,
    BelowSeekThreshold,
    UnstableDirection,
    InsufficientPersistence,
    Cooldown,
    PersistentSkew
};

struct SyncCorrectionDecision
{
    SyncCorrectionAction action = SyncCorrectionAction::Hold;
    SyncCorrectionReason reason = SyncCorrectionReason::WindowNotReady;

    // 只建议让落后的 client 向前追赶，不建议把领先端向后拉。
    // 这样未来接入真实控制时，可以减少用户可见的重复画面。
    int targetClientId = 0;
    int referenceClientId = 0;
    long long suggestedForwardMs = 0;
};

SyncCorrectionDecision evaluateSyncCorrection(
    const SyncCorrectionInput& input,
    const SyncCorrectionPolicyConfig& config = SyncCorrectionPolicyConfig{}
);

const char* syncCorrectionActionToString(SyncCorrectionAction action);
const char* syncCorrectionReasonToString(SyncCorrectionReason reason);
