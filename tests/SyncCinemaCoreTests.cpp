#include <chrono>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

#include "Protocol.h"
#include "Room.h"
#include "SyncCorrectionPolicy.h"

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
        SyncMessage outgoing;
        outgoing.type = MessageType::Snapshot;
        outgoing.controlEpoch = 7;
        outgoing.playbackState = PlaybackState::Playing;
        outgoing.positionMilliseconds = 123456;
        outgoing.positionSeconds = 123;

        std::string wireText = messageToString(outgoing);
        expect(
            wireText == "SNAPSHOT 7 Playing 123456\n",
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
            ).type == MessageType::Unknown,
            "SNAPSHOT rejects a position that cannot fit SyncState seconds"
        );
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
            room.broadcastControlMessage(kInvalidSocket, seek),
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
            room.broadcastControlMessage(kInvalidSocket, play),
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
            room.broadcastControlMessage(kInvalidSocket, pause),
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
            !room.broadcastControlMessage(kInvalidSocket, forgedSnapshot),
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
                SyncCorrectionAction::WouldSeekForward,
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
                SyncCorrectionAction::WouldSeekForward,
            "paused positions can be compared without an RTT projection"
        );
    }

    void testCorrectionPolicySelectsLaggingClient()
    {
        SyncCorrectionInput input = makePersistentSkewInput();
        SyncCorrectionDecision decision = evaluateSyncCorrection(input);
        expect(
            decision.action == SyncCorrectionAction::WouldSeekForward,
            "persistent stable skew produces a read-only seek suggestion"
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
}

int main()
{
    testSnapshotProtocolRoundTrip();
    testSnapshotProtocolRejectsMalformedInput();
    testPlaybackControlClassification();
    testRoomSnapshotAndEpoch();
    testCorrectionPolicySafetyGates();
    testCorrectionPolicySelectsLaggingClient();

    if (failureCount != 0)
    {
        std::cout << failureCount << " test(s) failed\n";
        return 1;
    }

    std::cout << "All SyncCinema core tests passed\n";
    return 0;
}
