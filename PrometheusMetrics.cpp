#include "PrometheusMetrics.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

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
    constexpr long long kGoodSyncThresholdMs = 250;

    std::size_t firstFrameStatusIndex(FirstFrameRecordStatus status)
    {
        switch (status)
        {
        case FirstFrameRecordStatus::Recorded:
            return 0;
        case FirstFrameRecordStatus::Duplicate:
            return 1;
        case FirstFrameRecordStatus::UnknownClient:
            return 2;
        case FirstFrameRecordStatus::InvalidMessage:
        default:
            return 3;
        }
    }

    const char* firstFrameStatusLabel(std::size_t index)
    {
        constexpr std::array<const char*, 4> labels{
            "recorded",
            "duplicate",
            "unknown_client",
            "invalid_message"
        };
        return labels[index];
    }

    std::size_t commandIndex(MessageType command)
    {
        switch (command)
        {
        case MessageType::Play:
            return 0;
        case MessageType::Pause:
            return 1;
        case MessageType::Seek:
        default:
            return 2;
        }
    }

    const char* commandLabel(std::size_t index)
    {
        constexpr std::array<const char*, 3> labels{
            "PLAY",
            "PAUSE",
            "SEEK"
        };
        return labels[index];
    }

    void writeTypeAndHelp(
        std::ostringstream& output,
        const char* name,
        const char* type,
        const char* help)
    {
        output << "# HELP " << name << ' ' << help << '\n';
        output << "# TYPE " << name << ' ' << type << '\n';
    }

    template<typename HistogramType>
    void writeHistogram(
        std::ostringstream& output,
        const char* name,
        const char* help,
        const HistogramType& histogram)
    {
        writeTypeAndHelp(output, name, "histogram", help);
        unsigned long long cumulative = 0;
        for (std::size_t index = 0;
            index < kHistogramUpperBoundsMs.size();
            ++index)
        {
            cumulative += histogram.bucketCounts[index];
            output << name << "_bucket{le=\""
                << kHistogramUpperBoundsMs[index]
                << "\"} " << cumulative << '\n';
        }
        cumulative += histogram.bucketCounts.back();
        output << name << "_bucket{le=\"+Inf\"} "
            << cumulative << '\n';
        output << name << "_sum "
            << static_cast<double>(histogram.sum) << '\n';
        output << name << "_count " << histogram.count << '\n';
    }
}

void PrometheusMetrics::Histogram::observe(long long value)
{
    const long long normalized = (std::max)(0LL, value);
    ++count;
    sum += static_cast<long double>(normalized);
    for (std::size_t index = 0;
        index < kHistogramUpperBoundsMs.size();
        ++index)
    {
        if (normalized <= kHistogramUpperBoundsMs[index])
        {
            ++bucketCounts[index];
            return;
        }
    }
    ++bucketCounts.back();
}

void PrometheusMetrics::setServerUp(bool up)
{
    std::lock_guard<std::mutex> lock(mutex_);
    serverUp_ = up;
}

void PrometheusMetrics::recordSessionStarted()
{
    std::lock_guard<std::mutex> lock(mutex_);
    activeSessions_ = 1;
    ++sessionsStarted_;
}

void PrometheusMetrics::recordSessionEnded()
{
    std::lock_guard<std::mutex> lock(mutex_);
    activeSessions_ = 0;
    ++sessionsEnded_;
}

void PrometheusMetrics::setConnectedClients(std::size_t count)
{
    std::lock_guard<std::mutex> lock(mutex_);
    connectedClients_ = static_cast<unsigned long long>(count);
}

void PrometheusMetrics::recordJoinResult(
    JoinMetricReason reason,
    long long durationMilliseconds)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t index = static_cast<std::size_t>(reason);
    if (index < joinsByReason_.size())
    {
        ++joinsByReason_[index];
    }
    else
    {
        ++joinsByReason_[static_cast<std::size_t>(JoinMetricReason::Other)];
    }
    joinDuration_.observe(durationMilliseconds);
}

void PrometheusMetrics::recordFirstFrame(const FirstFrameRecord& record)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++firstFrameByStatus_[firstFrameStatusIndex(record.status)];
    if (record.status == FirstFrameRecordStatus::Recorded)
    {
        firstFrameDuration_.observe(record.durationMilliseconds);
    }
}

void PrometheusMetrics::recordControlDispatched(
    MessageType command,
    std::size_t targetClients,
    bool broadcastSucceeded)
{
    if (!isPlaybackControlMessage(command))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ++controlsByCommand_[commandIndex(command)];
    controlExpectedAcks_ += static_cast<unsigned long long>(targetClients);
    if (!broadcastSucceeded)
    {
        ++controlBroadcastFailures_;
    }
}

void PrometheusMetrics::recordControlResult(const ControlAckRecord& record)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (record.matchStatus != ControlAckMatchStatus::Matched)
    {
        ++controlUnmatchedAcks_;
        return;
    }

    controlAckDuration_.observe(record.acknowledgementLatencyMilliseconds);
    clientApplyDuration_.observe(record.clientApplyMilliseconds);
    if (record.resultStatus == ControlResultStatus::Applied)
    {
        ++controlAppliedAcks_;
    }
    else
    {
        ++controlRejectedAcks_;
    }
}

void PrometheusMetrics::recordPairSample(const ExperiencePairSample& sample)
{
    if (!sample.valid)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    pairSkew_.observe(sample.absoluteDifferenceMilliseconds);
    if (sample.absoluteDifferenceMilliseconds <= kGoodSyncThresholdMs)
    {
        ++syncGoodSamples_;
    }
    else
    {
        ++syncBadSamples_;
    }
}

void PrometheusMetrics::recordCorrectionDispatched(bool sent)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (sent)
    {
        ++correctionsDispatched_;
    }
    else
    {
        ++correctionDispatchFailures_;
    }
}

void PrometheusMetrics::recordCorrectionResult(bool matched, bool applied)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!matched)
    {
        ++correctionUnmatchedResults_;
    }
    else if (applied)
    {
        ++correctionsApplied_;
    }
    else
    {
        ++correctionsRejected_;
    }
}

void PrometheusMetrics::recordCorrectionConverged(
    const CorrectionConvergenceRecord& record)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++correctionsConverged_;
    correctionConvergenceDuration_.observe(record.convergenceMilliseconds);
}

void PrometheusMetrics::recordSessionSummary(
    const ExperienceSessionSummary& summary)
{
    std::lock_guard<std::mutex> lock(mutex_);
    firstFrameMissing_ += static_cast<unsigned long long>(
        (std::max)(0LL, summary.firstFrameMissing)
    );
    controlMissingAcks_ += static_cast<unsigned long long>(
        (std::max)(0LL, summary.controlMissingAcks)
    );
    correctionMissingResults_ += static_cast<unsigned long long>(
        (std::max)(0LL, summary.correctionsMissingResults)
    );
    correctionsNotConverged_ += static_cast<unsigned long long>(
        (std::max)(0LL, summary.correctionsNotConverged)
    );
}

std::string PrometheusMetrics::render() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream output;
    output << std::setprecision(17);

    writeTypeAndHelp(
        output,
        "synccinema_server_up",
        "gauge",
        "Whether the SyncCinema server main listener is running."
    );
    output << "synccinema_server_up " << (serverUp_ ? 1 : 0) << '\n';

    writeTypeAndHelp(
        output,
        "synccinema_active_sessions",
        "gauge",
        "Current active media sessions."
    );
    output << "synccinema_active_sessions " << activeSessions_ << '\n';
    writeTypeAndHelp(
        output,
        "synccinema_connected_clients",
        "gauge",
        "Current clients that completed the JOIN handshake."
    );
    output << "synccinema_connected_clients " << connectedClients_ << '\n';

    writeTypeAndHelp(
        output,
        "synccinema_sessions_total",
        "counter",
        "Media session lifecycle transitions."
    );
    output << "synccinema_sessions_total{state=\"started\"} "
        << sessionsStarted_ << '\n';
    output << "synccinema_sessions_total{state=\"ended\"} "
        << sessionsEnded_ << '\n';

    writeTypeAndHelp(
        output,
        "synccinema_join_results_total",
        "counter",
        "JOIN outcomes grouped by a bounded reason code."
    );
    for (std::size_t index = 0; index < joinsByReason_.size(); ++index)
    {
        const JoinMetricReason reason = static_cast<JoinMetricReason>(index);
        output << "synccinema_join_results_total{outcome=\""
            << (reason == JoinMetricReason::Ok ? "accepted" : "rejected")
            << "\",reason=\"" << joinMetricReasonToString(reason)
            << "\"} " << joinsByReason_[index] << '\n';
    }
    writeHistogram(
        output,
        "synccinema_join_duration_ms",
        "JOIN handshake duration in milliseconds.",
        joinDuration_
    );

    writeTypeAndHelp(
        output,
        "synccinema_first_frame_results_total",
        "counter",
        "First-frame observation outcomes."
    );
    for (std::size_t index = 0; index < firstFrameByStatus_.size(); ++index)
    {
        output << "synccinema_first_frame_results_total{status=\""
            << firstFrameStatusLabel(index) << "\"} "
            << firstFrameByStatus_[index] << '\n';
    }
    output << "# HELP synccinema_first_frame_missing_total Expected first frames not reported before session end.\n";
    output << "# TYPE synccinema_first_frame_missing_total counter\n";
    output << "synccinema_first_frame_missing_total "
        << firstFrameMissing_ << '\n';
    writeHistogram(
        output,
        "synccinema_time_to_first_frame_ms",
        "Time from the relevant user action to libVLC Playing event.",
        firstFrameDuration_
    );

    writeTypeAndHelp(
        output,
        "synccinema_controls_dispatched_total",
        "counter",
        "Authoritative playback controls dispatched by command."
    );
    for (std::size_t index = 0; index < controlsByCommand_.size(); ++index)
    {
        output << "synccinema_controls_dispatched_total{command=\""
            << commandLabel(index) << "\"} "
            << controlsByCommand_[index] << '\n';
    }
    output << "# HELP synccinema_control_expected_acks_total Target acknowledgements expected for authoritative controls.\n";
    output << "# TYPE synccinema_control_expected_acks_total counter\n";
    output << "synccinema_control_expected_acks_total "
        << controlExpectedAcks_ << '\n';
    output << "# HELP synccinema_control_broadcast_failures_total Control broadcasts with at least one socket send failure.\n";
    output << "# TYPE synccinema_control_broadcast_failures_total counter\n";
    output << "synccinema_control_broadcast_failures_total "
        << controlBroadcastFailures_ << '\n';
    writeTypeAndHelp(
        output,
        "synccinema_control_results_total",
        "counter",
        "Control acknowledgement outcomes."
    );
    output << "synccinema_control_results_total{status=\"applied\"} "
        << controlAppliedAcks_ << '\n';
    output << "synccinema_control_results_total{status=\"rejected\"} "
        << controlRejectedAcks_ << '\n';
    output << "synccinema_control_results_total{status=\"unmatched\"} "
        << controlUnmatchedAcks_ << '\n';
    output << "synccinema_control_results_total{status=\"missing\"} "
        << controlMissingAcks_ << '\n';
    writeHistogram(
        output,
        "synccinema_control_ack_latency_ms",
        "Server dispatch to matching client acknowledgement latency.",
        controlAckDuration_
    );
    writeHistogram(
        output,
        "synccinema_client_control_apply_ms",
        "Client player API control application duration.",
        clientApplyDuration_
    );

    writeTypeAndHelp(
        output,
        "synccinema_sync_pair_samples_total",
        "counter",
        "Valid projected client-pair samples by quality."
    );
    output << "synccinema_sync_pair_samples_total{quality=\"good\"} "
        << syncGoodSamples_ << '\n';
    output << "synccinema_sync_pair_samples_total{quality=\"bad\"} "
        << syncBadSamples_ << '\n';
    writeHistogram(
        output,
        "synccinema_pair_skew_ms",
        "Absolute projected playback skew between a client pair.",
        pairSkew_
    );

    writeTypeAndHelp(
        output,
        "synccinema_corrections_total",
        "counter",
        "Guarded hard correction lifecycle outcomes."
    );
    output << "synccinema_corrections_total{status=\"dispatched\"} "
        << correctionsDispatched_ << '\n';
    output << "synccinema_corrections_total{status=\"dispatch_failed\"} "
        << correctionDispatchFailures_ << '\n';
    output << "synccinema_corrections_total{status=\"applied\"} "
        << correctionsApplied_ << '\n';
    output << "synccinema_corrections_total{status=\"rejected\"} "
        << correctionsRejected_ << '\n';
    output << "synccinema_corrections_total{status=\"unmatched\"} "
        << correctionUnmatchedResults_ << '\n';
    output << "synccinema_corrections_total{status=\"missing_result\"} "
        << correctionMissingResults_ << '\n';
    output << "synccinema_corrections_total{status=\"converged\"} "
        << correctionsConverged_ << '\n';
    output << "synccinema_corrections_total{status=\"not_converged\"} "
        << correctionsNotConverged_ << '\n';
    writeHistogram(
        output,
        "synccinema_correction_convergence_ms",
        "Correction dispatch to two consecutive good pair samples.",
        correctionConvergenceDuration_
    );

    return output.str();
}

const char* joinMetricReasonToString(JoinMetricReason reason)
{
    switch (reason)
    {
    case JoinMetricReason::Ok:
        return "OK";
    case JoinMetricReason::JoinRequired:
        return "JOIN_REQUIRED";
    case JoinMetricReason::MediaMismatch:
        return "MEDIA_MISMATCH";
    case JoinMetricReason::SnapshotSendFailed:
        return "SNAPSHOT_SEND_FAILED";
    case JoinMetricReason::Other:
    case JoinMetricReason::Count:
    default:
        return "OTHER";
    }
}
