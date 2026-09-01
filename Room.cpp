#include "Room.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>

namespace
{
    bool sendAll(SocketHandle socket, const std::string& data)
    {
        int totalSent = 0;
        const int totalSize = static_cast<int>(data.size());

        while (totalSent < totalSize)
        {
            int bytesSent = send(
                socket,
                data.c_str() + totalSent,
                totalSize - totalSent,
                0
            );

            if (bytesSent == kSocketError)
            {
                std::cout << "broadcast send failed: " << getSocketError() << "\n";
                return false;
            }

            if (bytesSent == 0)
            {
                std::cout << "broadcast send failed: no bytes sent\n";
                return false;
            }

            totalSent += bytesSent;
        }

        return true;
    }
}

RoomJoinResult Room::joinClient(
    SocketHandle clientSocket,
    const std::string& mediaIdentity)
{
    std::lock_guard<std::mutex> lock(mutex_);

    RoomJoinResult result;

    if (mediaIdentity.empty())
    {
        return result;
    }

    if (clients_.empty())
    {
        // 没有观众时，旧房间生命周期已经结束。首个 client 建立全新的
        // 媒体会话，状态必须从 Stopped/0 开始，不能继承上一部电影。
        state_ = SyncState{};
        mediaIdentity_ = mediaIdentity;
        controlEpoch_ = 0;
        lastStateUpdateTime_ = Clock::now();
    }
    else if (mediaIdentity_ != mediaIdentity)
    {
        result.activeMediaIdentity = mediaIdentity_;
        return result;
    }

    int clientId = nextClientId_;
    ++nextClientId_;
    clients_.push_back(ClientConnection{ clientId, clientSocket });

    result.accepted = true;
    result.clientId = clientId;
    result.activeMediaIdentity = mediaIdentity_;
    return result;
}

void Room::removeClient(SocketHandle clientSocket)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto newEnd = std::remove_if(
        clients_.begin(),
        clients_.end(),
        [clientSocket](const ClientConnection& client)
        {
            return client.socket == clientSocket;
        }
    );
    clients_.erase(newEnd, clients_.end());

    if (clients_.empty())
    {
        // 单房间 MVP 把“最后一个 client 离开”定义为会话结束。
        // 下一个首位加入者会重新选择媒体并从 0 开始。
        state_ = SyncState{};
        mediaIdentity_.clear();
        controlEpoch_ = 0;
        lastStateUpdateTime_ = Clock::now();
    }
}

bool Room::broadcastControlMessage(const SyncMessage& message)
{
    if (!isPlaybackControlMessage(message.type))
    {
        std::cout << "broadcast refused: message is not a playback control\n";
        return false;
    }

    std::vector<ClientConnection> targets;
    SyncState stateSnapshot;
    SyncMessage authoritativeMessage = message;

    {
        // 只在访问共享数据时持有锁。
        // 真正 send 网络数据前先复制目标列表，再释放锁，避免一个慢 client 卡住整个房间。
        std::lock_guard<std::mutex> lock(mutex_);

        Clock::time_point now = Clock::now();
        applyControlMessageLocked(message, now);
        ++controlEpoch_;
        authoritativeMessage.controlEpoch = controlEpoch_;
        stateSnapshot = getEstimatedStateLocked(now);
        targets = clients_;
    }

    std::string tcpMessage = messageToString(authoritativeMessage);

    std::cout << "room state: " << syncStateToString(stateSnapshot) << "\n";
    std::cout << "broadcasting to " << targets.size() << " client(s): " << tcpMessage;

    bool allSucceeded = true;
    for (const ClientConnection& target : targets)
    {
        if (!sendRawMessage(target.socket, tcpMessage))
        {
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

bool Room::sendMessageToClient(SocketHandle clientSocket, const SyncMessage& message)
{
    std::string tcpMessage = messageToString(message);
    if (tcpMessage == "UNKNOWN\n")
    {
        return false;
    }

    return sendRawMessage(clientSocket, tcpMessage);
}

bool Room::sendMessageToClientId(int clientId, const SyncMessage& message)
{
    SocketHandle targetSocket = kInvalidSocket;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto target = std::find_if(
            clients_.begin(),
            clients_.end(),
            [clientId](const ClientConnection& client)
            {
                return client.id == clientId;
            }
        );
        if (target == clients_.end())
        {
            return false;
        }
        targetSocket = target->socket;
    }

    return sendMessageToClient(targetSocket, message);
}

SyncState Room::getState() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return getEstimatedStateLocked(Clock::now());
}

RoomSnapshot Room::getSnapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    RoomSnapshot snapshot;
    snapshot.state = getEstimatedStateLocked(Clock::now());
    snapshot.controlEpoch = controlEpoch_;
    snapshot.mediaIdentity = mediaIdentity_;
    return snapshot;
}

std::size_t Room::getClientCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_.size();
}

std::vector<ClientConnection> Room::getClientSnapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_;
}

bool Room::sendRawMessage(SocketHandle clientSocket, const std::string& tcpMessage)
{
    // server 侧有多个线程可能同时给同一个 client 发消息：
    // 例如一个 client 发 PLAY 触发广播，同时 heartbeat 线程也在发 PING。
    // TCP 是字节流，如果没有发送锁，"PLAY\n" 和 "PING 12\n" 可能在字节层交错。
    std::lock_guard<std::mutex> lock(sendMutex_);
    return sendAll(clientSocket, tcpMessage);
}

SyncState Room::getEstimatedStateLocked(Clock::time_point now) const
{
    SyncState estimatedState = state_;

    if (estimatedState.state == PlaybackState::Playing)
    {
        auto elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastStateUpdateTime_
        ).count();

        if (elapsedMilliseconds > 0)
        {
            estimatedState.positionMilliseconds += elapsedMilliseconds;
            estimatedState.positionSeconds = static_cast<int>(
                estimatedState.positionMilliseconds / 1000
            );
        }
    }

    return estimatedState;
}

void Room::applyControlMessageLocked(const SyncMessage& message, Clock::time_point now)
{
    // 在处理任何控制命令前，先把房间状态推进到“当前时刻”。
    // 例如：上次 SEEK 168 后 PLAY，20 秒后收到 PAUSE，那么这里会先估算到 188 秒，再暂停。
    SyncState currentState = getEstimatedStateLocked(now);

    switch (message.type)
    {
    case MessageType::Play:
        currentState.state = PlaybackState::Playing;
        break;

    case MessageType::Pause:
        currentState.state = PlaybackState::Paused;
        break;

    case MessageType::Seek:
        currentState.positionSeconds = message.positionSeconds;
        currentState.positionMilliseconds = message.positionMilliseconds;
        // SEEK 只改变位置，不改变 Playing/Paused。
        // 如果 seek 前正在播放，那么 seek 后仍然从新位置继续播放。
        break;

    case MessageType::Report:
    case MessageType::Ping:
    case MessageType::Pong:
    case MessageType::Join:
    case MessageType::JoinRejected:
    case MessageType::Snapshot:
    case MessageType::Correction:
    case MessageType::CorrectionResult:
    case MessageType::Unknown:
        break;
    }

    state_ = currentState;
    lastStateUpdateTime_ = now;
}

