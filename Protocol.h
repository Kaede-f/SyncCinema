#pragma once

#include <string>

// 播放器当前处于什么状态。
// MVP 阶段不真正控制播放器，只先同步这些“控制意图”和“播放状态”。
enum class PlaybackState
{
    Stopped,
    Playing,
    Paused
};

// 两端都需要维护一份播放状态：
// - state 表示播放/暂停/停止
// - positionSeconds 表示当前播放进度，单位是秒
struct SyncState
{
    PlaybackState state = PlaybackState::Stopped;
    int positionSeconds = 0;
};

// 网络上传输的不是任意字符串，而是少数几种明确的同步消息。
// 这样业务层只关心“播放/暂停/跳转”，不会到处散落字符串比较代码。
enum class MessageType
{
    Play,
    Pause,
    Seek,
    Unknown
};

// 一条同步消息。
// Play/Pause 不需要 positionSeconds；Seek 需要携带跳转到多少秒。
struct SyncMessage
{
    MessageType type = MessageType::Unknown;
    int positionSeconds = 0;
};

std::string stateToString(PlaybackState state);
std::string messageTypeToString(MessageType type);

// 把结构化消息转换成 TCP 上可以发送的文本。
// 每条消息末尾带 '\n'，用来告诉接收端“一条消息到这里结束”。
std::string messageToString(const SyncMessage& message);

// 把 TCP 收到的一行文本转换回结构化消息。
// 如果文本不合法，会返回 MessageType::Unknown。
SyncMessage stringToMessage(const std::string& tcpString);

// 把同步消息应用到播放状态上。
// 真实播放器版本里，这里通常会在“播放器操作成功后”再更新状态。
void applyMessageToState(const SyncMessage& message, SyncState& state);

std::string syncStateToString(const SyncState& state);

// Server 给 client 的简单响应：返回当前 server 状态。
std::string stateResponseToString(const SyncState& state);

