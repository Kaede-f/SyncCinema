#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Protocol.h"

enum class FirstFrameRecordStatus
{
    Recorded,
    Duplicate,
    InvalidMessage,
    UnknownClient
};

struct FirstFrameRecord
{
    FirstFrameRecordStatus status = FirstFrameRecordStatus::InvalidMessage;
    int clientId = 0;
    long long controlEpoch = 0;
    FirstFrameTrigger trigger = FirstFrameTrigger::None;
    long long durationMilliseconds = 0;
};

enum class ControlAckMatchStatus
{
    Matched,
    UnknownEpoch,
    UnexpectedClient,
    CommandMismatch,
    Duplicate,
    InvalidMessage
};

struct ControlAckRecord
{
    ControlAckMatchStatus matchStatus = ControlAckMatchStatus::InvalidMessage;
    int clientId = 0;
    long long controlEpoch = 0;
    MessageType controlType = MessageType::Unknown;
    ControlResultStatus resultStatus = ControlResultStatus::None;
    long long acknowledgementLatencyMilliseconds = -1;
    long long clientApplyMilliseconds = -1;
    long long actualPositionMilliseconds = 0;
    std::string reason;
};

struct ExperiencePairSample
{
    int clientAId = 0;
    int clientBId = 0;
    long long controlEpoch = 0;
    PlaybackState playbackState = PlaybackState::Stopped;
    long long absoluteDifferenceMilliseconds = 0;
    bool valid = false;
};

struct CorrectionConvergenceRecord
{
    long long commandId = 0;
    long long controlEpoch = 0;
    int targetClientId = 0;
    int referenceClientId = 0;
    long long convergenceMilliseconds = 0;
    long long finalAbsoluteDifferenceMilliseconds = 0;
};

struct ExperienceSessionSummary
{
    std::string sessionId;
    long long joinedClients = 0;
    long long firstFrameExpected = 0;
    long long firstFrameReports = 0;
    long long firstFrameMissing = 0;
    long long firstFrameP95BucketMilliseconds = -1;

    long long controlExpectedAcks = 0;
    long long controlMatchedAcks = 0;
    long long controlAppliedAcks = 0;
    long long controlRejectedAcks = 0;
    long long controlMissingAcks = 0;
    long long controlAckP95BucketMilliseconds = -1;
    long long clientApplyP95BucketMilliseconds = -1;

    long long validPairSamples = 0;
    long long goodPairSamples = 0;
    double syncGoodRatio = 0.0;
    long long pairSkewP95BucketMilliseconds = -1;
    long long maximumPairSkewMilliseconds = -1;

    long long correctionsDispatched = 0;
    long long correctionsApplied = 0;
    long long correctionsRejected = 0;
    long long correctionsMissingResults = 0;
    long long correctionsConverged = 0;
    long long correctionsNotConverged = 0;
    long long correctionConvergenceP95BucketMilliseconds = -1;
};

// 汇总“用户是否顺利加入、控制是否及时应用、两端是否真正同步”三类体验事实。
// 该类不读写 socket，也不直接输出日志，server 负责把返回记录转换成遥测事件。
class ExperienceMetricsCollector
{
public:
    using Clock = std::chrono::steady_clock;

    void beginSession(const std::string& sessionId);
    void recordClientJoined(int clientId, bool firstFrameExpected);

    FirstFrameRecord recordFirstFrame(
        int clientId,
        const SyncMessage& message
    );

    bool recordControlDispatched(
        long long controlEpoch,
        MessageType controlType,
        const std::vector<int>& targetClientIds,
        Clock::time_point sentAt = Clock::now()
    );

    ControlAckRecord recordControlResult(
        int clientId,
        const SyncMessage& message,
        Clock::time_point receivedAt = Clock::now()
    );

    std::vector<CorrectionConvergenceRecord> recordPairSample(
        const ExperiencePairSample& sample,
        Clock::time_point observedAt = Clock::now()
    );

    void recordCorrectionDispatched(
        long long commandId,
        long long controlEpoch,
        int targetClientId,
        int referenceClientId,
        Clock::time_point sentAt = Clock::now()
    );

    void recordCorrectionResult(
        long long commandId,
        bool matched,
        bool applied
    );

    void removeClient(int clientId);
    ExperienceSessionSummary endSession(
        Clock::time_point endedAt = Clock::now()
    );

private:
    // 13 个有限上界 + 1 个 +Inf 桶，内存占用与会话时长无关。
    static constexpr std::size_t kHistogramBucketCount = 14;

    struct MillisecondHistogram
    {
        std::array<long long, kHistogramBucketCount> counts{};
        long long sampleCount = 0;
        long long maximum = -1;

        void observe(long long value);
        long long percentile95UpperBound() const;
        void reset();
    };

    struct PendingControl
    {
        MessageType controlType = MessageType::Unknown;
        Clock::time_point sentAt{};
        std::unordered_set<int> expectedClients;
        std::unordered_set<int> respondedClients;
    };

    struct PendingCorrection
    {
        long long commandId = 0;
        long long controlEpoch = 0;
        int targetClientId = 0;
        int referenceClientId = 0;
        Clock::time_point sentAt{};
        bool applied = false;
        int consecutiveGoodSamples = 0;
    };

    void pruneExpiredControlsLocked(Clock::time_point now);
    void pruneExpiredCorrectionsLocked(Clock::time_point now);
    void clearLocked();

    mutable std::mutex mutex_;
    std::string sessionId_;
    std::unordered_set<int> joinedClients_;
    std::unordered_set<int> firstFrameExpectedClients_;
    std::unordered_set<int> firstFrameClients_;
    std::unordered_map<long long, PendingControl> pendingControls_;
    std::unordered_map<long long, PendingCorrection> pendingCorrections_;

    MillisecondHistogram firstFrameHistogram_;
    MillisecondHistogram controlAckHistogram_;
    MillisecondHistogram clientApplyHistogram_;
    MillisecondHistogram pairSkewHistogram_;
    MillisecondHistogram correctionConvergenceHistogram_;

    long long controlExpectedAcks_ = 0;
    long long controlMatchedAcks_ = 0;
    long long controlAppliedAcks_ = 0;
    long long controlRejectedAcks_ = 0;
    long long validPairSamples_ = 0;
    long long goodPairSamples_ = 0;
    long long correctionsDispatched_ = 0;
    long long correctionsApplied_ = 0;
    long long correctionsRejected_ = 0;
    long long correctionsConverged_ = 0;
};

const char* firstFrameRecordStatusToString(FirstFrameRecordStatus status);
const char* controlAckMatchStatusToString(ControlAckMatchStatus status);
