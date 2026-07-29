#include <chrono>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

#include "Protocol.h"
#include "Room.h"

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
}

int main()
{
    testSnapshotProtocolRoundTrip();
    testSnapshotProtocolRejectsMalformedInput();
    testPlaybackControlClassification();
    testRoomSnapshotAndEpoch();

    if (failureCount != 0)
    {
        std::cout << failureCount << " test(s) failed\n";
        return 1;
    }

    std::cout << "All SyncCinema core tests passed\n";
    return 0;
}
