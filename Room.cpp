#include "Room.h"

#include <algorithm>
#include <chrono>
#include <string>

#include "Telemetry.h"

RoomJoinResult Room::joinClient(
    ConnectionId connectionId,
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
        sessionId_ = generateTelemetrySessionId();
        controlEpoch_ = 0;
        sessionStartedAt_ = Clock::now();
        lastStateUpdateTime_ = sessionStartedAt_;
        result.sessionStarted = true;
    }
    else if (mediaIdentity_ != mediaIdentity)
    {
        result.activeMediaIdentity = mediaIdentity_;
        result.sessionId = sessionId_;
        return result;
    }

    int clientId = nextClientId_;
    ++nextClientId_;
    clients_.push_back(ClientConnection{ clientId, connectionId });

    result.accepted = true;
    result.clientId = clientId;
    result.activeMediaIdentity = mediaIdentity_;
    result.sessionId = sessionId_;
    return result;
}

RoomRemovalResult Room::removeClient(ConnectionId connectionId)
{
    std::lock_guard<std::mutex> lock(mutex_);

    RoomRemovalResult result;
    result.sessionId = sessionId_;

    const std::size_t previousSize = clients_.size();

    auto newEnd = std::remove_if(
        clients_.begin(),
        clients_.end(),
        [connectionId](const ClientConnection& client)
        {
            return client.connectionId == connectionId;
        }
    );
    clients_.erase(newEnd, clients_.end());
    result.clientRemoved = clients_.size() != previousSize;
    result.remainingClients = clients_.size();

    if (result.clientRemoved && clients_.empty())
    {
        // 单房间 MVP 把“最后一个 client 离开”定义为会话结束。
        // 下一个首位加入者会重新选择媒体并从 0 开始。
        state_ = SyncState{};
        mediaIdentity_.clear();
        sessionId_.clear();
        controlEpoch_ = 0;
        const Clock::time_point endedAt = Clock::now();
        result.sessionDurationMilliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                endedAt - sessionStartedAt_
            ).count();
        sessionStartedAt_ = {};
        lastStateUpdateTime_ = endedAt;
        result.sessionEnded = true;
    }

    return result;
}

std::optional<SyncMessage> Room::applyControlMessage(const SyncMessage& message)
{
    if (!isPlaybackControlMessage(message.type))
    {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    applyControlMessageLocked(message, Clock::now());
    SyncMessage authoritativeMessage = message;
    authoritativeMessage.controlEpoch = ++controlEpoch_;
    return authoritativeMessage;
}

std::optional<ConnectionId> Room::findConnection(int clientId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto target = std::find_if(
        clients_.begin(),
        clients_.end(),
        [clientId](const ClientConnection& client) { return client.id == clientId; }
    );
    if (target == clients_.end())
    {
        return std::nullopt;
    }
    return target->connectionId;
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
    snapshot.sessionId = sessionId_;
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

