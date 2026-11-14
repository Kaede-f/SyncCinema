#include "SyncServer.h"

#include <iostream>
#include <string>
#include <string_view>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <limits>
#include <system_error>
#include <utility>

#include "ExperienceMetrics.h"
#include "MetricsHttpServer.h"
#include "Protocol.h"
#include "MetricLogger.h"
#include "NetSocket.h"
#include "PrometheusMetrics.h"
#include "Room.h"
#include "SyncCorrectionCoordinator.h"
#include "SyncMetrics.h"
#include "Telemetry.h"

namespace
{
    constexpr unsigned short kServerPort = 9000;
    constexpr unsigned short kDefaultMetricsPort = 9100;

    using Clock = ServerClock;

    unsigned short metricsPortFromEnvironment(std::string& warning)
    {
#ifdef _WIN32
        char* environmentValue = nullptr;
        std::size_t environmentLength = 0;
        if (_dupenv_s(
                &environmentValue,
                &environmentLength,
                "SYNCCINEMA_METRICS_PORT") != 0)
        {
            environmentValue = nullptr;
        }
        const std::string value = environmentValue == nullptr
            ? std::string{}
            : std::string(environmentValue);
        std::free(environmentValue);
#else
        const char* environmentValue = std::getenv(
            "SYNCCINEMA_METRICS_PORT"
        );
        const std::string value = environmentValue == nullptr
            ? std::string{}
            : std::string(environmentValue);
#endif
        if (value.empty())
        {
            return kDefaultMetricsPort;
        }

        errno = 0;
        char* end = nullptr;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (errno != 0 || end == value.c_str() || *end != '\0' ||
            parsed < 1 || parsed > 65535)
        {
            warning = "invalid SYNCCINEMA_METRICS_PORT; using 9100";
            return kDefaultMetricsPort;
        }
        return static_cast<unsigned short>(parsed);
    }

    long long elapsedMilliseconds(
        Clock::time_point startedAt,
        Clock::time_point endedAt = Clock::now())
    {
        const long long elapsed = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                endedAt - startedAt
            ).count()
        );
        return elapsed < 0 ? 0 : elapsed;
    }

    void emitSessionEnded(
        TelemetryLogger& telemetry,
        const RoomRemovalResult& removal)
    {
        if (!removal.sessionEnded)
        {
            return;
        }

        TelemetryEvent event;
        event.name = "session_ended";
        event.sessionId = removal.sessionId;
        event.addInteger(
            "duration_ms",
            removal.sessionDurationMilliseconds
        );
        telemetry.emit(event);
    }

    void emitSessionSummary(
        TelemetryLogger& telemetry,
        const ExperienceSessionSummary& summary)
    {
        TelemetryEvent event;
        event.name = "session_summary";
        event.sessionId = summary.sessionId;
        event
            .addInteger("joined_clients", summary.joinedClients)
            .addInteger("first_frame_expected", summary.firstFrameExpected)
            .addInteger("first_frame_reports", summary.firstFrameReports)
            .addInteger("first_frame_missing", summary.firstFrameMissing)
            .addInteger(
                "first_frame_p95_bucket_ms",
                summary.firstFrameP95BucketMilliseconds
            )
            .addInteger("control_expected_acks", summary.controlExpectedAcks)
            .addInteger("control_matched_acks", summary.controlMatchedAcks)
            .addInteger("control_applied_acks", summary.controlAppliedAcks)
            .addInteger("control_rejected_acks", summary.controlRejectedAcks)
            .addInteger("control_missing_acks", summary.controlMissingAcks)
            .addInteger(
                "control_ack_p95_bucket_ms",
                summary.controlAckP95BucketMilliseconds
            )
            .addInteger(
                "client_apply_p95_bucket_ms",
                summary.clientApplyP95BucketMilliseconds
            )
            .addInteger("valid_pair_samples", summary.validPairSamples)
            .addInteger("good_pair_samples", summary.goodPairSamples)
            .addDouble("sync_good_ratio", summary.syncGoodRatio)
            .addInteger(
                "pair_skew_p95_bucket_ms",
                summary.pairSkewP95BucketMilliseconds
            )
            .addInteger(
                "max_pair_skew_ms",
                summary.maximumPairSkewMilliseconds
            )
            .addInteger(
                "corrections_dispatched",
                summary.correctionsDispatched
            )
            .addInteger("corrections_applied", summary.correctionsApplied)
            .addInteger("corrections_rejected", summary.correctionsRejected)
            .addInteger(
                "corrections_missing_results",
                summary.correctionsMissingResults
            )
            .addInteger("corrections_converged", summary.correctionsConverged)
            .addInteger(
                "corrections_not_converged",
                summary.correctionsNotConverged
            )
            .addInteger(
                "correction_convergence_p95_bucket_ms",
                summary.correctionConvergenceP95BucketMilliseconds
            );
        telemetry.emit(event);
    }

    SyncMessage makeSnapshotMessage(const RoomSnapshot& snapshot)
    {
        SyncMessage message;
        message.type = MessageType::Snapshot;
        message.controlEpoch = snapshot.controlEpoch;
        message.playbackState = snapshot.state.state;
        message.positionSeconds = snapshot.state.positionSeconds;
        message.positionMilliseconds = snapshot.state.positionMilliseconds;
        message.mediaIdentity = snapshot.mediaIdentity;
        return message;
    }

    struct ControlBroadcast
    {
        TelemetryEvent event;
        std::size_t remaining;
        bool succeeded = true;
        bool ackTrackingStarted;
    };

    class SyncServer final : public ServerEvents
    {
    public:
        SyncServer() : telemetry(&std::cout, TelemetryLogger::pathFromEnvironment()) {}

        void started() override;
        void receivedLine(ServerTransport&, ServerPeer&, const std::string&) override;
        void joinTimedOut(ServerTransport&, ServerPeer&) override;
        void disconnected(ServerPeer&, const char*, int) override;
        void heartbeat(ServerTransport&) override;
        void stopped() override;
        void transportStats(const ServerTransportStats& stats) override;

    private:
        void join(ServerTransport&, ServerPeer&, const SyncMessage&);
        void reject(ServerTransport&, ServerPeer&, const char*);
        void recordJoin(ServerPeer&, JoinMetricReason, const char*,
            const RoomJoinResult* = nullptr, const RoomSnapshot* = nullptr,
            const std::string& requestedMedia = {});
        void processLine(ServerTransport&, const ServerPeer&, const SyncMessage&);

        Room room;
        SyncMetricsCollector metrics;
        ExperienceMetricsCollector experienceMetrics;
        PrometheusMetrics prometheus;
        MetricsHttpServer metricsHttpServer{prometheus};
        SyncCorrectionCoordinator correctionCoordinator;
        TelemetryLogger telemetry;
        std::size_t connectedClients = 0;
        int nextPingSequence = 1;
    };

    void SyncServer::processLine(
        ServerTransport& transport,
        const ServerPeer& peer,
        const SyncMessage& message)
    {
        const int clientId = peer.clientId;
        const std::string& sessionId = peer.sessionId;
        if (message.type == MessageType::Pong)
        {
            metrics.recordPongReceived(clientId, message.sequenceNumber, Clock::now());
            return;
        }

        if (message.type == MessageType::Ping)
        {
            // 当前协议中 PING 只由 server 主动发出，client 只需要回 PONG。
            // 如果这里收到 PING，说明对端发来了当前 server 不支持的方向，直接忽略。
            return;
        }

        if (message.type == MessageType::FirstFrame)
        {
            FirstFrameRecord record = experienceMetrics.recordFirstFrame(
                clientId,
                message
            );
            prometheus.recordFirstFrame(record);

            TelemetryEvent event;
            event.name = "media_first_frame";
            event.sessionId = sessionId;
            event.clientId = clientId;
            event.controlEpoch = message.controlEpoch;
            event
                .addString(
                    "status",
                    firstFrameRecordStatusToString(record.status)
                )
                .addString(
                    "trigger",
                    firstFrameTriggerToString(record.trigger)
                )
                .addInteger("duration_ms", record.durationMilliseconds);
            telemetry.emit(event);
            return;
        }

        if (message.type == MessageType::ControlResult)
        {
            ControlAckRecord record = experienceMetrics.recordControlResult(
                clientId,
                message,
                Clock::now()
            );
            prometheus.recordControlResult(record);

            TelemetryEvent event;
            event.name = "control_applied";
            event.sessionId = sessionId;
            event.clientId = clientId;
            event.controlEpoch = message.controlEpoch;
            event
                .addString(
                    "match",
                    controlAckMatchStatusToString(record.matchStatus)
                )
                .addString(
                    "command",
                    messageTypeToString(record.controlType)
                )
                .addString(
                    "status",
                    controlResultStatusToString(record.resultStatus)
                )
                .addInteger(
                    "ack_latency_ms",
                    record.acknowledgementLatencyMilliseconds
                )
                .addInteger(
                    "client_apply_ms",
                    record.clientApplyMilliseconds
                )
                .addInteger(
                    "actual_position_ms",
                    record.actualPositionMilliseconds
                )
                .addString("reason", record.reason);
            telemetry.emit(event);
            return;
        }

        if (message.type == MessageType::Snapshot ||
            message.type == MessageType::Correction)
        {
            // SNAPSHOT 是 server -> client 的单向消息。
            // client 伪造快照不能改变房间权威状态。
            std::cout << "server ignored server-only message\n";
            return;
        }

        if (message.type == MessageType::Join ||
            message.type == MessageType::JoinRejected)
        {
            // JOIN 只允许作为连接后的第一条握手消息；已加入房间后不能
            // 在同一 socket 上切换媒体身份。
            std::cout << "server ignored out-of-phase handshake message\n";
            return;
        }

        if (message.type == MessageType::Report)
        {
            RoomSnapshot roomSnapshot = room.getSnapshot();
            if (message.controlEpoch != roomSnapshot.controlEpoch)
            {
                std::lock_guard<std::mutex> logLock(metricLogMutex());
                std::cout << "[metric] type=progress_report"
                    << " client=" << clientId
                    << " report_epoch=" << message.controlEpoch
                    << " room_epoch=" << roomSnapshot.controlEpoch
                    << " status=stale_epoch"
                    << "\n";
                return;
            }

            SyncMetricSample sample;
            sample.clientId = clientId;
            sample.clientPositionMs = message.positionMilliseconds;
            sample.roomPositionMs = roomSnapshot.state.positionMilliseconds;
            sample.clientState = message.playbackState;
            sample.roomState = roomSnapshot.state.state;

            SyncMetricsUpdate metricsUpdate =
                metrics.recordProgressReport(sample);

            for (const SyncPairObservation& observation :
                metricsUpdate.pairObservations)
            {
                ExperiencePairSample experienceSample;
                experienceSample.clientAId = observation.clientAId;
                experienceSample.clientBId = observation.clientBId;
                experienceSample.controlEpoch = observation.controlEpoch;
                experienceSample.playbackState = observation.playbackState;
                experienceSample.absoluteDifferenceMilliseconds =
                    observation.absoluteDifferenceMilliseconds;
                experienceSample.valid = observation.validForExperience;
                prometheus.recordPairSample(experienceSample);

                std::vector<CorrectionConvergenceRecord> converged =
                    experienceMetrics.recordPairSample(
                        experienceSample,
                        Clock::now()
                    );
                for (const CorrectionConvergenceRecord& record : converged)
                {
                    prometheus.recordCorrectionConverged(record);
                    TelemetryEvent event;
                    event.name = "correction_converged";
                    event.sessionId = sessionId;
                    event.controlEpoch = record.controlEpoch;
                    event
                        .addInteger("command_id", record.commandId)
                        .addInteger("target_client", record.targetClientId)
                        .addInteger(
                            "reference_client",
                            record.referenceClientId
                        )
                        .addInteger(
                            "convergence_ms",
                            record.convergenceMilliseconds
                        )
                        .addInteger(
                            "final_abs_diff_ms",
                            record.finalAbsoluteDifferenceMilliseconds
                        );
                    telemetry.emit(event);
                }
            }

            std::optional<SyncCorrectionProposal>& proposal =
                metricsUpdate.correctionProposal;
            if (!proposal.has_value())
            {
                return;
            }

            // Metrics 观察窗口和 Room 使用同一个 epoch 才允许执行。
            // 用户刚刚 PAUSE/SEEK 时，旧窗口产生的提案会在这里被安全丢弃。
            if (proposal->controlEpoch != roomSnapshot.controlEpoch ||
                proposal->playbackState != roomSnapshot.state.state ||
                roomSnapshot.mediaIdentity.empty())
            {
                std::lock_guard<std::mutex> logLock(metricLogMutex());
                std::cout << "[metric] type=correction_command"
                    << " status=stale_proposal"
                    << " target_client=" << proposal->targetClientId
                    << " proposal_epoch=" << proposal->controlEpoch
                    << " room_epoch=" << roomSnapshot.controlEpoch
                    << "\n";
                return;
            }

            const auto target = room.findConnection(proposal->targetClientId);
            if (!target)
            {
                return;
            }
            const auto command = correctionCoordinator.createCommand(
                *proposal, Clock::now(), true
            );
            if (!command)
            {
                return;
            }

            const long long commandId = command->commandId;
            transport.write(*target, {
                std::make_shared<const std::string>(messageToString(*command)),
                [this, commandId, proposal = *proposal](bool sent)
                {
                    const auto now = Clock::now();
                    if (sent && correctionCoordinator.markDispatched(commandId, now))
                    {
                        metrics.recordCorrectionDispatched(proposal);
                        experienceMetrics.recordCorrectionDispatched(
                            commandId,
                            proposal.controlEpoch,
                            proposal.targetClientId,
                            proposal.referenceClientId,
                            now
                        );
                    }
                    if (!sent)
                    {
                        correctionCoordinator.markDispatchFailed(commandId);
                    }
                    prometheus.recordCorrectionDispatched(sent);
                    std::lock_guard<std::mutex> logLock(metricLogMutex());
                    std::cout << "[metric] type=correction_command"
                        << " command_id=" << commandId
                        << " epoch=" << proposal.controlEpoch
                        << " target_client=" << proposal.targetClientId
                        << " reference_client=" << proposal.referenceClientId
                        << " state=" << stateToString(proposal.playbackState)
                        << " forward_ms=" << proposal.forwardMilliseconds
                        << " median_abs_diff_ms=" << proposal.medianAbsDiffMs
                        << " p95_abs_diff_ms=" << proposal.p95AbsDiffMs
                        << " sent=" << (sent ? 1 : 0) << "\n";
                },
                [this, commandId] { return correctionCoordinator.isPending(commandId); }
            });
            return;
        }

        if (message.type == MessageType::CorrectionResult)
        {
            CorrectionResultRecord result = correctionCoordinator.recordResult(
                clientId,
                message
            );
            const bool matched = result.matchStatus ==
                CorrectionResultMatchStatus::Matched;
            const bool applied = result.resultStatus ==
                CorrectionResultStatus::Applied;
            experienceMetrics.recordCorrectionResult(
                result.commandId,
                matched,
                applied
            );
            prometheus.recordCorrectionResult(matched, applied);

            TelemetryEvent event;
            event.name = "correction_completed";
            event.sessionId = sessionId;
            event.clientId = clientId;
            event.controlEpoch = result.controlEpoch;
            event
                .addString(
                    "match",
                    correctionResultMatchStatusToString(result.matchStatus)
                )
                .addInteger("command_id", result.commandId)
                .addString(
                    "status",
                    correctionResultStatusToString(result.resultStatus)
                )
                .addInteger(
                    "ack_latency_ms",
                    result.acknowledgementLatencyMs
                )
                .addInteger(
                    "actual_position_ms",
                    result.actualPositionMilliseconds
                )
                .addInteger("forward_ms", result.forwardMilliseconds)
                .addString("reason", result.reason);
            telemetry.emit(event);
            {
                std::lock_guard<std::mutex> logLock(metricLogMutex());
                std::cout << "[metric] type=correction_result"
                    << " match="
                    << correctionResultMatchStatusToString(result.matchStatus)
                    << " client=" << clientId
                    << " command_id=" << result.commandId
                    << " epoch=" << result.controlEpoch
                    << " status="
                    << correctionResultStatusToString(result.resultStatus)
                    << " actual_pos_ms=" << result.actualPositionMilliseconds
                    << " forward_ms=" << result.forwardMilliseconds
                    << " ack_latency_ms=" << result.acknowledgementLatencyMs
                    << " reason=" << result.reason
                    << "\n";
            }
            return;
        }

        if (!isPlaybackControlMessage(message.type) ||
            message.controlEpoch != 0)
        {
            std::cout << "server ignored invalid client control\n";
            return;
        }

        const auto authoritative = room.applyControlMessage(message);
        const long long epoch = authoritative->controlEpoch;
        const auto targets = room.getClientSnapshot();
        const auto startedAt = Clock::now();
        metrics.beginControlEpoch(epoch, message);
        correctionCoordinator.retainControlEpoch(epoch);

        std::vector<int> targetClientIds;
        targetClientIds.reserve(targets.size());
        for (const ClientConnection& target : targets)
        {
            targetClientIds.push_back(target.id);
        }
        const bool ackTrackingStarted = experienceMetrics.recordControlDispatched(
            epoch, message.type, targetClientIds, startedAt
        );
        // Count the authoritative command and its intended ACKs at commit.
        // Count broadcast failures separately when all writes have resolved.
        prometheus.recordControlDispatched(message.type, targets.size());

        auto broadcast = std::make_shared<ControlBroadcast>();
        broadcast->event.name = "control_dispatched";
        broadcast->event.sessionId = sessionId;
        broadcast->event.clientId = clientId;
        broadcast->event.controlEpoch = epoch;
        broadcast->event
            .addString("command", messageTypeToString(message.type))
            .addInteger("target_clients", static_cast<long long>(targets.size()));
        broadcast->remaining = targets.size();
        broadcast->ackTrackingStarted = ackTrackingStarted;

        const auto frame = std::make_shared<const std::string>(
            messageToString(*authoritative)
        );
        for (const ClientConnection& target : targets)
        {
            transport.write(target.connectionId, {
                frame,
                [this, broadcast](bool sent)
                {
                    broadcast->succeeded &= sent;
                    if (--broadcast->remaining != 0)
                    {
                        return;
                    }
                    if (!broadcast->succeeded)
                    {
                        prometheus.recordControlBroadcastFailure();
                    }
                    broadcast->event
                        .addBoolean("broadcast_success", broadcast->succeeded)
                        .addBoolean("ack_tracking_started", broadcast->ackTrackingStarted);
                    telemetry.emit(broadcast->event);
                },
                {}
            });
        }
    }

    void SyncServer::recordJoin(
        ServerPeer& peer,
        JoinMetricReason result,
        const char* reason,
        const RoomJoinResult* joinResult,
        const RoomSnapshot* snapshot,
        const std::string& requestedMedia)
    {
        peer.joinResultRecorded = true;
        const long long duration = elapsedMilliseconds(peer.acceptedAt);
        const bool success = result == JoinMetricReason::Ok;
        TelemetryEvent event;
        event.name = "join_result";
        event.sessionId = joinResult ? joinResult->sessionId : peer.sessionId;
        event.clientId = peer.clientId;
        event.addBoolean("success", success)
            .addString("reason", reason)
            .addInteger("duration_ms", duration);
        if (snapshot && success)
        {
            event.controlEpoch = snapshot->controlEpoch;
            event.addString("media_id", snapshot->mediaIdentity)
                .addInteger("online_clients", static_cast<long long>(connectedClients));
        }
        if (result == JoinMetricReason::MediaMismatch)
        {
            event.addString("requested_media_id", requestedMedia)
                .addString("active_media_id", joinResult->activeMediaIdentity);
        }
        telemetry.emit(event);
        prometheus.recordJoinResult(result, duration);
    }

    void SyncServer::reject(
        ServerTransport& transport, ServerPeer& peer, const char* reason)
    {
        SyncMessage message;
        message.type = MessageType::JoinRejected;
        message.rejectionReason = reason;
        transport.write(peer.connectionId, {
            std::make_shared<const std::string>(messageToString(message)), {}, {}
        });
        transport.closeAfterWrites(peer.connectionId);
    }

    void SyncServer::joinTimedOut(ServerTransport& transport, ServerPeer& peer)
    {
        recordJoin(peer, JoinMetricReason::JoinRequired, "JOIN_REQUIRED");
        reject(transport, peer, "JOIN_REQUIRED");
    }

    void SyncServer::join(
        ServerTransport& transport, ServerPeer& peer, const SyncMessage& message)
    {
        const auto result = room.joinClient(peer.connectionId, message.mediaIdentity);
        if (!result.accepted)
        {
            recordJoin(peer, JoinMetricReason::MediaMismatch, "MEDIA_MISMATCH",
                &result, nullptr, message.mediaIdentity);
            reject(transport, peer, "MEDIA_MISMATCH");
            return;
        }

        peer.clientId = result.clientId;
        peer.sessionId = result.sessionId;
        if (result.sessionStarted)
        {
            experienceMetrics.beginSession(result.sessionId);
            prometheus.recordSessionStarted();
            TelemetryEvent event;
            event.name = "session_started";
            event.sessionId = result.sessionId;
            event.addString("media_id", result.activeMediaIdentity);
            telemetry.emit(event);
        }

        // Membership and queueing the snapshot are one serialized operation.
        const RoomSnapshot snapshot = room.getSnapshot();
        transport.write(peer.connectionId, {
            std::make_shared<const std::string>(messageToString(makeSnapshotMessage(snapshot))),
            [this, &peer, snapshot](bool sent)
            {
                if (sent)
                {
                    peer.snapshotWritten = true;
                    experienceMetrics.recordClientJoined(
                        peer.clientId, snapshot.state.state == PlaybackState::Playing
                    );
                    prometheus.setConnectedClients(++connectedClients);
                    recordJoin(peer, JoinMetricReason::Ok, "OK", nullptr, &snapshot);
                }
                else
                {
                    recordJoin(peer, JoinMetricReason::SnapshotSendFailed, "SNAPSHOT_SEND_FAILED");
                }
            },
            {}
        });
    }

    void SyncServer::receivedLine(
        ServerTransport& transport, ServerPeer& peer, const std::string& line)
    {
        if (line.empty())
        {
            return;
        }
        const SyncMessage message = stringToMessage(line);
        if (peer.clientId != 0)
        {
            processLine(transport, peer, message);
        }
        else if (message.type == MessageType::Join)
        {
            join(transport, peer, message);
        }
        else
        {
            joinTimedOut(transport, peer);
        }
    }

    void SyncServer::disconnected(ServerPeer& peer, const char* reason, int error)
    {
        if (peer.clientId == 0)
        {
            if (!peer.joinResultRecorded && std::string_view(reason) != "server_shutdown")
            {
                recordJoin(peer, JoinMetricReason::JoinRequired, "JOIN_REQUIRED");
            }
            return;
        }

        const auto removal = room.removeClient(peer.connectionId);
        std::optional<ExperienceSessionSummary> sessionSummary;
        if (removal.sessionEnded)
        {
            sessionSummary = experienceMetrics.endSession(Clock::now());
            metrics.resetSession();
        }
        else
        {
            experienceMetrics.removeClient(peer.clientId);
            metrics.removeClient(peer.clientId);
        }
        correctionCoordinator.removeClient(peer.clientId);
        if (peer.snapshotWritten)
        {
            prometheus.setConnectedClients(--connectedClients);
        }
        if (sessionSummary)
        {
            prometheus.recordSessionSummary(*sessionSummary);
            prometheus.recordSessionEnded();
        }
        TelemetryEvent event;
        event.name = "client_disconnected";
        event.sessionId = peer.sessionId;
        event.clientId = peer.clientId;
        event.addString("reason", reason)
            .addInteger("socket_error", error)
            .addInteger("connection_duration_ms", elapsedMilliseconds(peer.acceptedAt))
            .addInteger("remaining_clients", static_cast<long long>(removal.remainingClients));
        telemetry.emit(event);
        if (sessionSummary)
        {
            emitSessionSummary(telemetry, *sessionSummary);
        }
        emitSessionEnded(telemetry, removal);
    }

    void SyncServer::heartbeat(ServerTransport& transport)
    {
        const auto clients = room.getClientSnapshot();
        if (clients.empty())
        {
            return;
        }
        const int sequence = nextPingSequence;
        nextPingSequence = sequence == (std::numeric_limits<int>::max)() ? 1 : sequence + 1;
        SyncMessage ping;
        ping.type = MessageType::Ping;
        ping.sequenceNumber = sequence;
        const auto frame = std::make_shared<const std::string>(messageToString(ping));
        for (const auto& client : clients)
        {
            transport.write(client.connectionId, {
                frame,
                [this, id = client.id, sequence](bool sent)
                {
                    if (sent)
                    {
                        metrics.recordPingSent(id, sequence, Clock::now());
                    }
                },
                {}
            });
        }
    }

    void SyncServer::started()
    {
        std::string metricsPortWarning;
        const unsigned short requestedMetricsPort =
            metricsPortFromEnvironment(metricsPortWarning);
        if (!metricsPortWarning.empty())
        {
            std::cerr << metricsPortWarning << "\n";
        }
        std::string metricsError;
        const bool metricsEndpointEnabled = metricsHttpServer.start(
            requestedMetricsPort,
            metricsError
        );
        prometheus.setServerUp(true);

        std::cout << "SyncServer listening on port " << kServerPort << "...\n";
        std::cout << "Server role: accept clients and broadcast playback commands.\n";
        std::cout << "Press Ctrl+C to stop the server.\n";
        if (metricsEndpointEnabled)
        {
            std::cout << "Metrics endpoint: http://127.0.0.1:"
                << metricsHttpServer.port() << "/metrics\n";
        }
        else
        {
            std::cerr << "metrics endpoint disabled: " << metricsError << "\n";
        }
        if (!telemetry.initializationError().empty())
        {
            std::cerr << "telemetry file disabled: "
                << telemetry.initializationError() << "\n";
        }

        TelemetryEvent started;
        started.name = "server_started";
        started
            .addInteger("listen_port", kServerPort)
            .addBoolean("jsonl_file_enabled", telemetry.fileEnabled())
            .addBoolean("metrics_endpoint_enabled", metricsEndpointEnabled)
            .addInteger(
                "metrics_port",
                metricsEndpointEnabled ? metricsHttpServer.port() : 0
            );
        telemetry.emit(started);
    }

    void SyncServer::transportStats(const ServerTransportStats& stats)
    {
        prometheus.setTransportStats(stats);
    }

    void SyncServer::stopped()
    {
        prometheus.setServerUp(false);
        metricsHttpServer.stop();
    }
}

void runSyncServer()
{
    if (!initializeSockets())
    {
        return;
    }
    SyncServer server;
    try
    {
        runServerTransport(kServerPort, server);
    }
    catch (const std::system_error& error)
    {
        std::cerr << "SyncServer: " << error.what() << "\n";
    }
    cleanupSockets();
}
