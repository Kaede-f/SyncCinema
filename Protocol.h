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
    int positionSeconds = 0;
};

// Structured command type used by the protocol layer.
// The network sends text, but the business logic should use SyncMessage.
enum class MessageType
{
    Play,
    Pause,
    Seek,
    Unknown
};

// One parsed sync command.
// Play/Pause do not use positionSeconds; Seek uses it as the target time.
struct SyncMessage
{
    MessageType type = MessageType::Unknown;
    int positionSeconds = 0;
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
