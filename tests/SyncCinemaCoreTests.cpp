#include <chrono>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "MockPlayer.h"
#include "Protocol.h"
#include "Room.h"
#include "SyncCorrectionPolicy.h"
#include "SyncCorrectionCoordinator.h"
#include "SyncCorrectionExecutor.h"

namespace
{
    int failureCount = 0;

    void expect(bool condition, const std::string& description)
    {
        if (condition)
        {
            std::cout << "[PASS] " << description << "\n";
            return;
        }

        ++failureCount;
        std::cout << "[FAIL] " << description << "\n";
    }

    void testSnapshotProtocolRoundTrip()
    {
        const std::string mediaIdentity = makeMediaIdentity(
            "http://example.test/videos/movie.mp4"
        );
        SyncMessage outgoing;
        outgoing.type = MessageType::Snapshot;
        outgoing.controlEpoch = 7;
        outgoing.playbackState = PlaybackState::Playing;
        outgoing.positionMilliseconds = 123456;
        outgoing.positionSeconds = 123;
        outgoing.mediaIdentity = mediaIdentity;

        std::string wireText = messageToString(outgoing);
        expect(
            wireText == "SNAPSHOT 7 Playing 123456 " + mediaIdentity + "\n",
            "SNAPSHOT serializes to one newline-delimited protocol line"
        );

        SyncMessage parsed = stringToMessage(wireText);
        expect(parsed.type == MessageType::Snapshot, "SNAPSHOT type parses");
        expect(parsed.controlEpoch == 7, "SNAPSHOT epoch parses");
        expect(
            parsed.playbackState == PlaybackState::Playing,
            "SNAPSHOT playback state parses"
        );
        expect(
            parsed.positionMilliseconds == 123456,
            "SNAPSHOT millisecond position parses"
        );
        expect(parsed.positionSeconds == 123, "SNAPSHOT derives second position");
        expect(
            parsed.mediaIdentity == mediaIdentity,
            "SNAPSHOT media identity parses"
        );

        SyncState state;
        applyMessageToState(parsed, state);
        expect(state.state == PlaybackState::Playing, "SNAPSHOT applies state");
        expect(
            state.positionMilliseconds == 123456,
            "SNAPSHOT applies millisecond position"
        );
    }

    void testSnapshotProtocolRejectsMalformedInput()
    {
        expect(
            stringToMessage("SNAPSHOT -1 Playing 1000").type == MessageType::Unknown,
            "SNAPSHOT rejects a negative epoch"
        );
        expect(
            stringToMessage("SNAPSHOT 1 Buffering 1000").type == MessageType::Unknown,
            "SNAPSHOT rejects an unknown playback state"
        );
        expect(
            stringToMessage("SNAPSHOT 1 Paused -1").type == MessageType::Unknown,
            "SNAPSHOT rejects a negative position"
        );
        expect(
            stringToMessage("SNAPSHOT 1 Paused 1000 extra").type == MessageType::Unknown,
            "SNAPSHOT rejects trailing fields"
        );
        expect(
            stringToMessage("SNAPSHOT 1 Paused").type == MessageType::Unknown,
            "SNAPSHOT rejects missing fields"
        );

        const long long tooLargePositionMs =
            (static_cast<long long>((std::numeric_limits<int>::max)()) + 1) * 1000;
        expect(
            stringToMessage(
                "SNAPSHOT 1 Paused " + std::to_string(tooLargePositionMs)
                + " 0123456789abcdef"
            ).type == MessageType::Unknown,
            "SNAPSHOT rejects a position that cannot fit SyncState seconds"
        );
    }

    void testMediaJoinProtocol()
    {
        const std::string windowsPath = "D:\\videos\\movie.mp4";
        const std::string slashPath = "D:/videos/movie.mp4";
        const std::string mediaIdentity = makeMediaIdentity(windowsPath);

        expect(
            mediaIdentity == makeMediaIdentity(slashPath),
            "media identity normalizes path separators"
        );
        expect(
            mediaIdentity != makeMediaIdentity("D:/videos/other.mp4"),
            "different media sources produce different identities"
        );

        SyncMessage join;
        join.type = MessageType::Join;
        join.mediaIdentity = mediaIdentity;
        std::string joinWire = messageToString(join);
        expect(
            joinWire == "JOIN " + mediaIdentity + "\n",
            "JOIN serializes to one protocol line"
        );

        SyncMessage parsedJoin = stringToMessage(joinWire);
        expect(parsedJoin.type == MessageType::Join, "JOIN type parses");
        expect(
            parsedJoin.mediaIdentity == mediaIdentity,
            "JOIN media identity parses"
        );
        expect(
            stringToMessage("JOIN not-a-valid-id").type == MessageType::Unknown,
            "JOIN rejects malformed media identities"
        );

        SyncMessage rejection;
        rejection.type = MessageType::JoinRejected;
        rejection.rejectionReason = "MEDIA_MISMATCH";
        SyncMessage parsedRejection = stringToMessage(
            messageToString(rejection)
        );
        expect(
            parsedRejection.type == MessageType::JoinRejected,
            "REJECT type parses"
        );
        expect(
            parsedRejection.rejectionReason == "MEDIA_MISMATCH",
            "REJECT reason parses"
        );
    }

    void testRoomMediaSessionLifecycle()
    {
        Room room;
        const SocketHandle firstSocket = static_cast<SocketHandle>(101);
        const SocketHandle secondSocket = static_cast<SocketHandle>(102);
        const std::string firstMedia = makeMediaIdentity("movie-a.mp4");
        const std::string secondMedia = makeMediaIdentity("movie-b.mp4");

        RoomJoinResult firstJoin = room.joinClient(firstSocket, firstMedia);
        expect(firstJoin.accepted, "first client establishes the room media");
        expect(
            room.getSnapshot().mediaIdentity == firstMedia,
            "Room snapshot carries the active media identity"
        );

        SyncMessage seek;
        seek.type = MessageType::Seek;
        seek.positionSeconds = 95;
        seek.positionMilliseconds = 95000;
        // 这里使用的是测试用伪 socket，权威 CONTROL 回显会发送失败；
        // Room 的状态更新和网络写入是两个独立结果，生命周期测试只验证前者。
        room.broadcastControlMessage(seek);
        RoomSnapshot afterSeek = room.getSnapshot();
        expect(
            afterSeek.controlEpoch == 1 &&
                afterSeek.state.positionMilliseconds == 95000,
            "active media session applies playback controls"
        );

        RoomJoinResult mismatchedJoin = room.joinClient(
            secondSocket,
            secondMedia
        );
        expect(
            !mismatchedJoin.accepted,
            "Room rejects a different media while viewers remain"
        );
        expect(
            room.getClientCount() == 1,
            "rejected media does not enter the broadcast list"
        );

        RoomJoinResult matchingJoin = room.joinClient(
            secondSocket,
            firstMedia
        );
        expect(
            matchingJoin.accepted,
            "Room accepts another client with the same media"
        );

        room.removeClient(secondSocket);
        room.removeClient(firstSocket);
        RoomSnapshot emptyRoom = room.getSnapshot();
        expect(
            emptyRoom.mediaIdentity.empty(),
            "last client leaving ends the media session"
        );
        expect(
            emptyRoom.state.state == PlaybackState::Stopped &&
                emptyRoom.state.positionMilliseconds == 0 &&
                emptyRoom.controlEpoch == 0,
            "ended media session resets authoritative playback state"
        );

        RoomJoinResult nextJoin = room.joinClient(firstSocket, secondMedia);
        RoomSnapshot nextSnapshot = room.getSnapshot();
        expect(nextJoin.accepted, "next media can establish a fresh session");
        expect(
            nextSnapshot.mediaIdentity == secondMedia &&
                nextSnapshot.state.state == PlaybackState::Stopped &&
                nextSnapshot.state.positionMilliseconds == 0,
            "new media starts from Stopped at position zero"
        );
        room.removeClient(firstSocket);
    }

    void testPlaybackControlClassification()
    {
        expect(isPlaybackControlMessage(MessageType::Play), "PLAY is a control");
        expect(isPlaybackControlMessage(MessageType::Pause), "PAUSE is a control");
        expect(isPlaybackControlMessage(MessageType::Seek), "SEEK is a control");
        expect(
            !isPlaybackControlMessage(MessageType::Snapshot),
            "SNAPSHOT is not a client control"
        );
        expect(
            !isPlaybackControlMessage(MessageType::Report),
            "REPORT is not a client control"
        );
    }

    void testProgressReportCarriesControlEpoch()
    {
        SyncMessage report;
        report.type = MessageType::Report;
        report.controlEpoch = 8;
        report.positionMilliseconds = 65432;
        report.positionSeconds = 65;
        report.playbackState = PlaybackState::Playing;

        std::string wireText = messageToString(report);
        expect(
            wireText == "REPORT 8 65432 Playing\n",
            "REPORT serializes its authoritative control epoch"
        );

        SyncMessage parsed = stringToMessage(wireText);
        expect(
            parsed.type == MessageType::Report &&
                parsed.controlEpoch == 8 &&
                parsed.positionMilliseconds == 65432 &&
                parsed.playbackState == PlaybackState::Playing,
            "REPORT epoch, position and state round-trip"
        );
        expect(
            stringToMessage("REPORT 65432 Playing").type ==
                MessageType::Unknown,
            "legacy REPORT without an epoch is rejected"
        );
        expect(
            stringToMessage("REPORT -1 65432 Playing").type ==
                MessageType::Unknown,
            "REPORT rejects a negative control epoch"
        );
    }

    void testAuthoritativeControlAndCorrectionProtocol()
    {
        SyncMessage authoritativeSeek;
        authoritativeSeek.type = MessageType::Seek;
        authoritativeSeek.controlEpoch = 9;
        authoritativeSeek.positionMilliseconds = 123456;
        authoritativeSeek.positionSeconds = 123;

        std::string controlWire = messageToString(authoritativeSeek);
        expect(
            controlWire == "CONTROL 9 SEEK 123456\n",
            "authoritative SEEK carries epoch and millisecond position"
        );
        SyncMessage parsedControl = stringToMessage(controlWire);
        expect(
            parsedControl.type == MessageType::Seek &&
                parsedControl.controlEpoch == 9 &&
                parsedControl.positionMilliseconds == 123456,
            "authoritative CONTROL round-trips"
        );

        SyncMessage correction;
        correction.type = MessageType::Correction;
        correction.commandId = 42;
        correction.controlEpoch = 9;
        correction.playbackState = PlaybackState::Playing;
        correction.correctionForwardMilliseconds = 875;
        SyncMessage parsedCorrection = stringToMessage(
            messageToString(correction)
        );
        expect(
            parsedCorrection.type == MessageType::Correction &&
                parsedCorrection.commandId == 42 &&
                parsedCorrection.controlEpoch == 9 &&
                parsedCorrection.playbackState == PlaybackState::Playing &&
                parsedCorrection.correctionForwardMilliseconds == 875,
            "CORRECT round-trips with command, epoch, state and offset"
        );

        SyncMessage result;
        result.type = MessageType::CorrectionResult;
        result.commandId = 42;
        result.controlEpoch = 9;
        result.correctionResultStatus = CorrectionResultStatus::Applied;
        result.positionMilliseconds = 124331;
        result.correctionReason = "OK";
        SyncMessage parsedResult = stringToMessage(messageToString(result));
        expect(
            parsedResult.type == MessageType::CorrectionResult &&
                parsedResult.commandId == 42 &&
                parsedResult.correctionResultStatus ==
                    CorrectionResultStatus::Applied &&
                parsedResult.positionMilliseconds == 124331 &&
                parsedResult.correctionReason == "OK",
            "CORRECT_RESULT round-trips an applied acknowledgement"
        );

        expect(
            stringToMessage("CONTROL 0 PLAY").type == MessageType::Unknown,
            "CONTROL rejects a non-positive epoch"
        );
        expect(
            stringToMessage("CORRECT 1 2 Stopped 800").type ==
                MessageType::Unknown,
            "CORRECT rejects inactive playback"
        );
        expect(
            stringToMessage("CORRECT_RESULT 1 2 APPLIED 1000 FAILED").type ==
                MessageType::Unknown,
            "applied CORRECT_RESULT requires the OK reason"
        );
    }

    void testPlayerEventAbstraction()
    {
        ConsoleMockPlayer player;
        std::vector<PlayerEventType> receivedEvents;
        player.setEventCallback(
            [&receivedEvents](const PlayerEvent& event)
            {
                receivedEvents.push_back(event.type);
            }
        );

        expect(player.openMedia("movie.mp4"), "MockPlayer opens test media");
        expect(player.play(), "MockPlayer starts playback");
        expect(player.pause(), "MockPlayer pauses playback");
        expect(
            receivedEvents == std::vector<PlayerEventType>{
                PlayerEventType::Opening,
                PlayerEventType::Playing,
                PlayerEventType::Paused
            },
            "player events stay independent from the libVLC implementation"
        );
    }

    void testRoomSnapshotAndEpoch()
    {
        Room room;

        RoomSnapshot initial = room.getSnapshot();
        expect(initial.controlEpoch == 0, "new Room starts at epoch 0");
        expect(
            initial.state.state == PlaybackState::Stopped,
            "new Room starts stopped"
        );
        expect(
            initial.state.positionMilliseconds == 0,
            "new Room starts at position 0"
        );

        SyncMessage seek;
        seek.type = MessageType::Seek;
        seek.positionSeconds = 12;
        seek.positionMilliseconds = 12000;
        expect(
            room.broadcastControlMessage(seek),
            "Room accepts SEEK without connected clients"
        );

        RoomSnapshot afterSeek = room.getSnapshot();
        expect(afterSeek.controlEpoch == 1, "SEEK increments Room epoch");
        expect(
            afterSeek.state.positionMilliseconds == 12000,
            "SEEK updates Room position"
        );

        SyncMessage play;
        play.type = MessageType::Play;
        expect(
            room.broadcastControlMessage(play),
            "Room accepts PLAY without connected clients"
        );

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        RoomSnapshot whilePlaying = room.getSnapshot();
        expect(whilePlaying.controlEpoch == 2, "PLAY increments Room epoch");
        expect(
            whilePlaying.state.state == PlaybackState::Playing,
            "PLAY updates Room state"
        );
        expect(
            whilePlaying.state.positionMilliseconds >= 12000,
            "playing Room snapshot advances with steady_clock"
        );

        SyncMessage pause;
        pause.type = MessageType::Pause;
        expect(
            room.broadcastControlMessage(pause),
            "Room accepts PAUSE without connected clients"
        );

        RoomSnapshot afterPause = room.getSnapshot();
        expect(afterPause.controlEpoch == 3, "PAUSE increments Room epoch");
        expect(
            afterPause.state.state == PlaybackState::Paused,
            "PAUSE updates Room state"
        );

        SyncMessage forgedSnapshot;
        forgedSnapshot.type = MessageType::Snapshot;
        forgedSnapshot.controlEpoch = 999;
        forgedSnapshot.playbackState = PlaybackState::Playing;
        forgedSnapshot.positionMilliseconds = 999000;

        expect(
            !room.broadcastControlMessage(forgedSnapshot),
            "Room refuses a forged SNAPSHOT as a control"
        );
        expect(
            room.getSnapshot().controlEpoch == 3,
            "refused messages do not advance Room epoch"
        );
    }

    SyncCorrectionInput makePersistentSkewInput()
    {
        SyncCorrectionInput input;
        input.clientAId = 1;
        input.clientBId = 2;
        input.controlEpoch = 4;
        input.playbackState = PlaybackState::Playing;
        input.windowReady = true;
        input.windowSamples = 12;
        input.medianDiffMs = 900;
        input.medianAbsDiffMs = 900;
        input.p95AbsDiffMs = 1050;
        input.consecutiveSevereSamples = 4;
        input.directionAgreementPercent = 83;
        input.hasRttA = true;
        input.hasRttB = true;
        return input;
    }

    void testCorrectionPolicySafetyGates()
    {
        SyncCorrectionInput input = makePersistentSkewInput();
        input.settling = true;
        expect(
            evaluateSyncCorrection(input).reason ==
                SyncCorrectionReason::Settling,
            "correction advice waits for the control settling period"
        );

        input = makePersistentSkewInput();
        input.windowReady = false;
        expect(
            evaluateSyncCorrection(input).reason ==
                SyncCorrectionReason::WindowNotReady,
            "correction advice requires a ready robust window"
        );

        input = makePersistentSkewInput();
        input.playbackState = PlaybackState::Stopped;
        expect(
            evaluateSyncCorrection(input).reason ==
                SyncCorrectionReason::PlaybackInactive,
            "correction advice does not seek stopped playback"
        );

        input = makePersistentSkewInput();
        input.hasRttB = false;
        expect(
            evaluateSyncCorrection(input).reason ==
                SyncCorrectionReason::MissingNetworkEstimate,
            "playing correction advice requires RTT for both clients"
        );

        input = makePersistentSkewInput();
        input.medianDiffMs = 180;
        input.medianAbsDiffMs = 180;
        expect(
            evaluateSyncCorrection(input).reason ==
                SyncCorrectionReason::WithinTolerance,
            "small skew remains inside the no-correction tolerance"
        );

        input = makePersistentSkewInput();
        input.medianDiffMs = 500;
        input.medianAbsDiffMs = 500;
        expect(
            evaluateSyncCorrection(input).reason ==
                SyncCorrectionReason::BelowSeekThreshold,
            "moderate skew is observed without suggesting a hard seek"
        );

        input = makePersistentSkewInput();
        input.medianDiffMs = 750;
        input.medianAbsDiffMs = 750;
        expect(
            evaluateSyncCorrection(input).action ==
                SyncCorrectionAction::SeekForward,
            "the hard-seek entry threshold uses an inclusive boundary"
        );

        input = makePersistentSkewInput();
        input.directionAgreementPercent = 66;
        expect(
            evaluateSyncCorrection(input).reason ==
                SyncCorrectionReason::UnstableDirection,
            "correction advice rejects a direction that keeps changing"
        );

        input = makePersistentSkewInput();
        input.consecutiveSevereSamples = 2;
        expect(
            evaluateSyncCorrection(input).reason ==
                SyncCorrectionReason::InsufficientPersistence,
            "correction advice requires consecutive severe samples"
        );

        input = makePersistentSkewInput();
        input.cooldownActive = true;
        input.cooldownRemainingMs = 2400;
        expect(
            evaluateSyncCorrection(input).reason ==
                SyncCorrectionReason::Cooldown,
            "correction advice respects the simulated cooldown"
        );

        input = makePersistentSkewInput();
        input.playbackState = PlaybackState::Paused;
        input.hasRttA = false;
        input.hasRttB = false;
        expect(
            evaluateSyncCorrection(input).action ==
                SyncCorrectionAction::SeekForward,
            "paused positions can be compared without an RTT projection"
        );
    }

    void testCorrectionPolicySelectsLaggingClient()
    {
        SyncCorrectionInput input = makePersistentSkewInput();
        SyncCorrectionDecision decision = evaluateSyncCorrection(input);
        expect(
            decision.action == SyncCorrectionAction::SeekForward,
            "persistent stable skew produces a forward seek decision"
        );
        expect(
            decision.reason == SyncCorrectionReason::PersistentSkew,
            "persistent skew explains the seek suggestion"
        );
        expect(
            decision.targetClientId == 2 && decision.referenceClientId == 1,
            "positive pair diff selects client B as the lagging target"
        );
        expect(
            decision.suggestedForwardMs == 900,
            "positive pair diff preserves the suggested forward offset"
        );

        input.medianDiffMs = -1250;
        input.medianAbsDiffMs = 1250;
        decision = evaluateSyncCorrection(input);
        expect(
            decision.targetClientId == 1 && decision.referenceClientId == 2,
            "negative pair diff selects client A as the lagging target"
        );
        expect(
            decision.suggestedForwardMs == 1250,
            "negative pair diff becomes a positive forward offset"
        );
    }

    SyncCorrectionProposal makeCorrectionProposal()
    {
        SyncCorrectionProposal proposal;
        proposal.targetClientId = 2;
        proposal.referenceClientId = 1;
        proposal.controlEpoch = 7;
        proposal.playbackState = PlaybackState::Playing;
        proposal.forwardMilliseconds = 900;
        proposal.medianAbsDiffMs = 900;
        proposal.p95AbsDiffMs = 1050;
        return proposal;
    }

    SyncMessage makeAppliedCorrectionResult(const SyncMessage& command)
    {
        SyncMessage result;
        result.type = MessageType::CorrectionResult;
        result.commandId = command.commandId;
        result.controlEpoch = command.controlEpoch;
        result.correctionResultStatus = CorrectionResultStatus::Applied;
        result.positionMilliseconds = 120900;
        result.correctionReason = "OK";
        return result;
    }

    void testCorrectionCoordinatorLifecycle()
    {
        SyncCorrectionCoordinator coordinator;
        SyncCorrectionCoordinator::Clock::time_point startedAt{};
        std::optional<SyncMessage> command = coordinator.createCommand(
            makeCorrectionProposal(),
            startedAt
        );
        expect(command.has_value(), "coordinator creates a valid command");
        expect(
            command->commandId == 1 && command->type == MessageType::Correction,
            "coordinator assigns a monotonic command id"
        );
        expect(
            coordinator.pendingCommandCount() == 1,
            "created correction remains pending until acknowledgement"
        );
        expect(
            !coordinator.createCommand(
                makeCorrectionProposal(),
                startedAt + std::chrono::milliseconds(1)
            ).has_value(),
            "one client cannot have two pending correction commands"
        );

        SyncMessage result = makeAppliedCorrectionResult(*command);
        CorrectionResultRecord wrongClient = coordinator.recordResult(
            3,
            result,
            startedAt + std::chrono::milliseconds(10)
        );
        expect(
            wrongClient.matchStatus == CorrectionResultMatchStatus::WrongClient &&
                coordinator.pendingCommandCount() == 1,
            "wrong client cannot consume another client's command"
        );

        result.controlEpoch = 8;
        CorrectionResultRecord wrongEpoch = coordinator.recordResult(
            2,
            result,
            startedAt + std::chrono::milliseconds(20)
        );
        expect(
            wrongEpoch.matchStatus == CorrectionResultMatchStatus::EpochMismatch &&
                coordinator.pendingCommandCount() == 1,
            "wrong epoch cannot consume the pending command"
        );

        result.controlEpoch = command->controlEpoch;
        CorrectionResultRecord matched = coordinator.recordResult(
            2,
            result,
            startedAt + std::chrono::milliseconds(37)
        );
        expect(
            matched.matchStatus == CorrectionResultMatchStatus::Matched &&
                matched.acknowledgementLatencyMs == 37 &&
                coordinator.pendingCommandCount() == 0,
            "matching result completes the command and records latency"
        );
        expect(
            coordinator.recordResult(2, result).matchStatus ==
                CorrectionResultMatchStatus::UnknownCommand,
            "duplicate acknowledgement is reported as unknown"
        );

        std::optional<SyncMessage> failedDispatch = coordinator.createCommand(
            makeCorrectionProposal()
        );
        expect(
            failedDispatch.has_value() &&
                coordinator.markDispatchFailed(failedDispatch->commandId) &&
                coordinator.pendingCommandCount() == 0,
            "failed socket dispatch removes the pending command"
        );

        std::optional<SyncMessage> removedClientCommand =
            coordinator.createCommand(makeCorrectionProposal());
        coordinator.removeClient(1);
        expect(
            removedClientCommand.has_value() &&
                coordinator.pendingCommandCount() == 0,
            "disconnecting either endpoint clears related commands"
        );
    }

    void testCorrectionCoordinatorInvalidatesOldEpochs()
    {
        SyncCorrectionCoordinator coordinator;
        SyncCorrectionProposal epochSeven = makeCorrectionProposal();
        SyncCorrectionProposal epochEight = epochSeven;
        epochEight.controlEpoch = 8;
        epochEight.targetClientId = 3;

        expect(
            coordinator.createCommand(epochSeven).has_value() &&
                coordinator.createCommand(epochEight).has_value(),
            "coordinator accepts commands from test epochs"
        );
        coordinator.retainControlEpoch(8);
        expect(
            coordinator.pendingCommandCount() == 1,
            "new authoritative control invalidates older correction commands"
        );

        SyncCorrectionCoordinator timeoutCoordinator;
        SyncCorrectionCoordinator::Clock::time_point startedAt{};
        expect(
            timeoutCoordinator.createCommand(epochSeven, startedAt).has_value(),
            "timeout test creates an initial command"
        );
        expect(
            timeoutCoordinator.createCommand(
                epochSeven,
                startedAt + std::chrono::seconds(11)
            ).has_value(),
            "expired unacknowledged command no longer blocks retry"
        );
    }

    void testCorrectionExecutorSafetyAndApplication()
    {
        ConsoleMockPlayer player;
        expect(player.openMedia("movie.mp4"), "executor test opens media");

        SyncState localState;
        localState.state = PlaybackState::Playing;

        SyncMessage command;
        command.type = MessageType::Correction;
        command.commandId = 1;
        command.controlEpoch = 4;
        command.playbackState = PlaybackState::Playing;
        command.correctionForwardMilliseconds = 900;

        SyncCorrectionExecution applied = executeSyncCorrection(
            command,
            4,
            localState,
            player
        );
        expect(
            applied.applied &&
                applied.resultMessage.correctionResultStatus ==
                    CorrectionResultStatus::Applied &&
                applied.resultMessage.positionMilliseconds == 900,
            "matching correction seeks the lagging player forward"
        );

        command.commandId = 2;
        command.controlEpoch = 3;
        SyncCorrectionExecution stale = executeSyncCorrection(
            command,
            4,
            localState,
            player
        );
        expect(
            !stale.applied &&
                stale.resultMessage.correctionReason == "EPOCH_MISMATCH" &&
                player.getPositionMilliseconds() == 900,
            "stale correction cannot move the player"
        );

        ConsoleMockPlayer unopenedPlayer;
        command.commandId = 3;
        command.controlEpoch = 4;
        SyncCorrectionExecution notSeekable = executeSyncCorrection(
            command,
            4,
            localState,
            unopenedPlayer
        );
        expect(
            !notSeekable.applied &&
                notSeekable.resultMessage.correctionReason == "NOT_SEEKABLE",
            "correction reports a stable reason when media is not seekable"
        );
    }
}

int main()
{
    testSnapshotProtocolRoundTrip();
    testSnapshotProtocolRejectsMalformedInput();
    testMediaJoinProtocol();
    testPlaybackControlClassification();
    testProgressReportCarriesControlEpoch();
    testAuthoritativeControlAndCorrectionProtocol();
    testPlayerEventAbstraction();
    testRoomSnapshotAndEpoch();
    testRoomMediaSessionLifecycle();
    testCorrectionPolicySafetyGates();
    testCorrectionPolicySelectsLaggingClient();
    testCorrectionCoordinatorLifecycle();
    testCorrectionCoordinatorInvalidatesOldEpochs();
    testCorrectionExecutorSafetyAndApplication();

    if (failureCount != 0)
    {
        std::cout << failureCount << " test(s) failed\n";
        return 1;
    }

    std::cout << "All SyncCinema core tests passed\n";
    return 0;
}
