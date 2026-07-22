#pragma once

#include <chrono>
#include <mutex>
#include <vector>

#include "NetSocket.h"
#include "Protocol.h"

struct ClientConnection
{
    int id = 0;
    SocketHandle socket = kInvalidSocket;
};

// Room 表示一个同步观影房间。
//
// 新架构里 server 不再打开视频文件，所以 server 不能直接向播放器询问“现在播到第几秒”。
// Room 使用一个轻量的估算模型维护房间进度：
//   1. state_.positionSeconds 保存“上一次状态基准点”的播放位置。
//   2. lastStateUpdateTime_ 保存这个基准点对应的服务器时间。
//   3. 如果房间处于 Playing，getState() 会用“当前时间 - 基准时间”推算实时进度。
//
// 这个模型足够支撑后续 Qt 进度条：UI 可以定时调用 getState()，拿到随时间推进的房间状态。
class Room
{
public:
    int addClient(SocketHandle clientSocket);
    void removeClient(SocketHandle clientSocket);

    // senderSocket 是发起命令的 client。
    // 广播时会跳过它，避免发送方重复执行自己的命令。
    bool broadcastControlMessage(SocketHandle senderSocket, const SyncMessage& message);
    bool sendMessageToClient(SocketHandle clientSocket, const SyncMessage& message);

    SyncState getState() const;
    std::size_t getClientCount() const;
    std::vector<ClientConnection> getClientSnapshot() const;

private:
    using Clock = std::chrono::steady_clock;

    SyncState getEstimatedStateLocked(Clock::time_point now) const;
    void applyControlMessageLocked(const SyncMessage& message, Clock::time_point now);
    bool sendRawMessage(SocketHandle clientSocket, const std::string& tcpMessage);

    // clients_、state_、lastStateUpdateTime_ 会被多个 client 线程同时访问，
    // 所以所有读写都必须用 mutex_ 保护。
    mutable std::mutex mutex_;
    mutable std::mutex sendMutex_;
    std::vector<ClientConnection> clients_;
    SyncState state_;
    Clock::time_point lastStateUpdateTime_ = Clock::now();
    int nextClientId_ = 1;
};

