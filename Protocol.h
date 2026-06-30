#pragma once

#include <string>

// Current playback state known by the sync protocol.
enum class PlaybackState
{
    Stopped,
    Playing,
    Paused
};

// Playback status shared by the room and clients.
struct SyncState
{
    PlaybackState state = PlaybackState::Stopped;

    // positionSeconds 保留给命令行显示、seek 命令等初学者更直观的场景。
    int positionSeconds = 0;

    // positionMilliseconds 是同步观测和后续同步算法使用的高精度位置。
    // 同步算法关心的是几十到几百毫秒的偏差，只用秒会太粗糙。
    long long positionMilliseconds = 0;
};

// Structured command type used by the protocol layer.
// The network sends text, but the business logic should use SyncMessage.
enum class MessageType
{
    Play,
    Pause,
    Seek,
    Report,
    Unknown
};

// One parsed sync protocol message.
// Seek uses positionSeconds as the target time.
// Report uses positionMilliseconds as the client's current playback position,
// and playbackState as the client's local playback state.
struct SyncMessage
{
    MessageType type = MessageType::Unknown;
    int positionSeconds = 0;
    long long positionMilliseconds = 0;
    PlaybackState playbackState = PlaybackState::Stopped;
};

std::string stateToString(PlaybackState state);
std::string messageTypeToString(MessageType type);

// Serialize a structured command into one TCP line.
// The trailing '\n' is the message boundary.
std::string messageToString(const SyncMessage& message);

// Parse one received TCP line back into a structured command.
// Invalid input returns MessageType::Unknown.
SyncMessage stringToMessage(const std::string& tcpString);

// Apply a sync command to local state.
void applyMessageToState(const SyncMessage& message, SyncState& state);

std::string syncStateToString(const SyncState& state);

// Simple server-to-client state response used by the earlier MVP.
std::string stateResponseToString(const SyncState& state);
