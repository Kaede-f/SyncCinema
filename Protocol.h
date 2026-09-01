#pragma once

#include <string>

// Current playback state known by the sync protocol.
enum class PlaybackState
{
    Stopped,
    Playing,
    Paused
};

enum class CorrectionResultStatus
{
    None,
    Applied,
    Rejected
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
    Ping,
    Pong,
    Join,
    JoinRejected,
    Snapshot,
    Correction,
    CorrectionResult,
    Unknown
};

// One parsed sync protocol message.
// Seek uses positionSeconds as the target time.
// Report uses controlEpoch, positionMilliseconds and playbackState to identify
// which authoritative control cycle produced the client sample.
// Snapshot is sent by the server when a client joins an existing room.
struct SyncMessage
{
    MessageType type = MessageType::Unknown;
    int positionSeconds = 0;
    long long positionMilliseconds = 0;
    PlaybackState playbackState = PlaybackState::Stopped;
    int sequenceNumber = 0; // PING/PONG 使用，用来把一次请求和一次响应配对。
    long long controlEpoch = 0; // SNAPSHOT/CONTROL/REPORT/CORRECT：房间权威控制版本。
    std::string mediaIdentity; // JOIN/SNAPSHOT 使用：标识本次房间播放的媒体来源。
    std::string rejectionReason; // REJECT 使用：返回握手失败的稳定错误码。
    long long commandId = 0; // CORRECT/CORRECT_RESULT：校正请求与回执配对。
    long long correctionForwardMilliseconds = 0; // CORRECT：落后端应向前追赶的距离。
    CorrectionResultStatus correctionResultStatus =
        CorrectionResultStatus::None;
    std::string correctionReason; // CORRECT_RESULT：OK 或稳定拒绝原因码。
};

std::string stateToString(PlaybackState state);
std::string messageTypeToString(MessageType type);
std::string correctionResultStatusToString(CorrectionResultStatus status);

// 只有 PLAY / PAUSE / SEEK 会改变房间的权威播放状态。
// 把这个判断集中在协议层，避免 server、Room 和 metrics 各自维护一份不同规则。
bool isPlaybackControlMessage(MessageType type);

// 将媒体路径或 URL 转成跨进程稳定的短标识。
// 该标识只用于房间一致性判断，不是密码学摘要，也不能用于安全校验。
std::string makeMediaIdentity(const std::string& mediaSource);

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
