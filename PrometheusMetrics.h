#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <string>

#include "ExperienceMetrics.h"
#include "Protocol.h"

enum class JoinMetricReason
{
    Ok,
    JoinRequired,
    MediaMismatch,
    SnapshotSendFailed,
    Other,
    Count
};

// 进程内 Prometheus 注册表。所有标签均来自固定枚举，禁止把 session_id、
// client_id、媒体 URL 或错误全文作为 label，避免时间序列数量无限增长。
class PrometheusMetrics
{
public:
    void setServerUp(bool up);
    void recordSessionStarted();
    void recordSessionEnded();
    void setConnectedClients(std::size_t count);

    void recordJoinResult(
        JoinMetricReason reason,
        long long durationMilliseconds
    );
    void recordFirstFrame(const FirstFrameRecord& record);
    void recordControlDispatched(
        MessageType command,
        std::size_t targetClients,
        bool broadcastSucceeded
    );
    void recordControlResult(const ControlAckRecord& record);
    void recordPairSample(const ExperiencePairSample& sample);
    void recordCorrectionDispatched(bool sent);
    void recordCorrectionResult(bool matched, bool applied);
    void recordCorrectionConverged(
        const CorrectionConvergenceRecord& record
    );
    void recordSessionSummary(const ExperienceSessionSummary& summary);

    // 返回一次自洽的 Prometheus text exposition 快照。
    std::string render() const;

private:
    static constexpr std::size_t kHistogramBucketCount = 14;

    struct Histogram
    {
        std::array<unsigned long long, kHistogramBucketCount> bucketCounts{};
        unsigned long long count = 0;
        long double sum = 0.0L;

        void observe(long long value);
    };

    mutable std::mutex mutex_;
    bool serverUp_ = false;
    unsigned long long activeSessions_ = 0;
    unsigned long long connectedClients_ = 0;
    unsigned long long sessionsStarted_ = 0;
    unsigned long long sessionsEnded_ = 0;

    std::array<unsigned long long,
        static_cast<std::size_t>(JoinMetricReason::Count)> joinsByReason_{};
    Histogram joinDuration_;

    std::array<unsigned long long, 4> firstFrameByStatus_{};
    unsigned long long firstFrameMissing_ = 0;
    Histogram firstFrameDuration_;

    std::array<unsigned long long, 3> controlsByCommand_{};
    unsigned long long controlExpectedAcks_ = 0;
    unsigned long long controlBroadcastFailures_ = 0;
    unsigned long long controlAppliedAcks_ = 0;
    unsigned long long controlRejectedAcks_ = 0;
    unsigned long long controlUnmatchedAcks_ = 0;
    unsigned long long controlMissingAcks_ = 0;
    Histogram controlAckDuration_;
    Histogram clientApplyDuration_;

    unsigned long long syncGoodSamples_ = 0;
    unsigned long long syncBadSamples_ = 0;
    Histogram pairSkew_;

    unsigned long long correctionsDispatched_ = 0;
    unsigned long long correctionDispatchFailures_ = 0;
    unsigned long long correctionsApplied_ = 0;
    unsigned long long correctionsRejected_ = 0;
    unsigned long long correctionUnmatchedResults_ = 0;
    unsigned long long correctionMissingResults_ = 0;
    unsigned long long correctionsConverged_ = 0;
    unsigned long long correctionsNotConverged_ = 0;
    Histogram correctionConvergenceDuration_;
};

const char* joinMetricReasonToString(JoinMetricReason reason);
