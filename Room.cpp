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

int Room::addClient(SocketHandle clientSocket)
{
    std::lock_guard<std::mutex> lock(mutex_);
    clients_.push_back(clientSocket);

    int clientId = nextClientId_;
    ++nextClientId_;
    return clientId;
}

void Room::removeClient(SocketHandle clientSocket)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto newEnd = std::remove(clients_.begin(), clients_.end(), clientSocket);
    clients_.erase(newEnd, clients_.end());
}

bool Room::broadcastControlMessage(SocketHandle senderSocket, const SyncMessage& message)
{
    std::string tcpMessage = messageToString(message);
    if (tcpMessage == "UNKNOWN\n")
    {
        std::cout << "broadcast refused: unknown command\n";
        return false;
    }

    std::vector<SocketHandle> targets;
    SyncState stateSnapshot;

    {
        // 只在访问共享数据时持有锁。
        // 真正 send 网络数据前先复制目标列表，再释放锁，避免一个慢 client 卡住整个房间。
        std::lock_guard<std::mutex> lock(mutex_);

        Clock::time_point now = Clock::now();
        applyControlMessageLocked(message, now);
        stateSnapshot = getEstimatedStateLocked(now);

        for (SocketHandle clientSocket : clients_)
        {
            if (clientSocket != senderSocket)
            {
                targets.push_back(clientSocket);
            }
        }
    }

    std::cout << "room state: " << syncStateToString(stateSnapshot) << "\n";
    std::cout << "broadcasting to " << targets.size() << " client(s): " << tcpMessage;

    bool allSucceeded = true;
    for (SocketHandle target : targets)
    {
        if (!sendAll(target, tcpMessage))
        {
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

SyncState Room::getState() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return getEstimatedStateLocked(Clock::now());
}

std::size_t Room::getClientCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_.size();
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
    case MessageType::Unknown:
        break;
    }

    state_ = currentState;
    lastStateUpdateTime_ = now;
}

