#include "SyncClientSession.h"

#include <algorithm>
#include <sstream>
#include <utility>

#include <ws2tcpip.h>

#include "SyncCorrectionExecutor.h"

namespace
{
    constexpr unsigned short kServerPort = 9000;
    constexpr auto kProgressReportInterval = std::chrono::seconds(1);
    constexpr auto kInitialSnapshotTimeout = std::chrono::seconds(5);
    constexpr auto kPlayerSeekableTimeout = std::chrono::seconds(15);
    constexpr auto kPlayerReadyPollInterval = std::chrono::milliseconds(25);

    bool tryTakeProtocolLine(std::string& receiveBuffer, std::string& line)
    {
        std::size_t lineEnd = receiveBuffer.find('\n');
        if (lineEnd == std::string::npos)
        {
            return false;
        }

        line = receiveBuffer.substr(0, lineEnd);
        receiveBuffer.erase(0, lineEnd + 1);

        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        return true;
    }

    bool sendAll(
        SOCKET socket,
        const std::string& data,
        std::string& errorMessage)
    {
        std::size_t totalSent = 0;
        while (totalSent < data.size())
        {
            int remaining = static_cast<int>(data.size() - totalSent);
            int bytesSent = send(
                socket,
                data.data() + totalSent,
                remaining,
                0
            );

            if (bytesSent == SOCKET_ERROR)
            {
                errorMessage = "send failed: " +
                    std::to_string(WSAGetLastError());
                return false;
            }

            if (bytesSent == 0)
            {
                errorMessage = "send failed: no bytes sent";
                return false;
            }

            totalSent += static_cast<std::size_t>(bytesSent);
        }

        return true;
    }

    const char* sourceLabel(SyncClientCommandSource source)
    {
        switch (source)
        {
        case SyncClientCommandSource::InitialSnapshot:
            return "initial";
        case SyncClientCommandSource::RemoteCommand:
            return "remote";
        case SyncClientCommandSource::Correction:
            return "correction";
        case SyncClientCommandSource::LocalCommand:
        default:
            return "local";
        }
    }
}

SyncClientSession::SyncClientSession(
    PlayerController& player,
    SyncClientCallbacks callbacks)
    : player_(player),
      callbacks_(std::move(callbacks))
{
}

SyncClientSession::~SyncClientSession()
{
    disconnect();
}

bool SyncClientSession::connectToRoom(
    const std::string& serverHost,
    const std::string& mediaSource,
    std::string& errorMessage,
    SyncClientConnectMetrics* metrics)
{
    std::unique_lock<std::mutex> lifecycleLock(lifecycleMutex_);

    if (running_ || connected_)
    {
        errorMessage = "client session is already connected";
        return false;
    }

    WSADATA wsaData{};
    int startupResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (startupResult != 0)
    {
        errorMessage = "WSAStartup failed: " + std::to_string(startupResult);
        return false;
    }
    winsockInitialized_ = true;

    mediaIdentity_ = makeMediaIdentity(mediaSource);

    Clock::time_point connectStartedAt = Clock::now();
    if (!connectSocket(serverHost, errorMessage))
    {
        closeSocketAndCleanup();
        return false;
    }

    if (metrics != nullptr)
    {
        metrics->connectServerMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - connectStartedAt
            ).count();
    }

    SyncMessage joinMessage;
    joinMessage.type = MessageType::Join;
    joinMessage.mediaIdentity = mediaIdentity_;
    if (!sendProtocolMessage(joinMessage, errorMessage))
    {
        closeSocketAndCleanup();
        return false;
    }

    running_ = true;

    std::string initialReceiveBuffer;
    SyncMessage initialSnapshot;
    Clock::time_point snapshotReceivedAt{};
    if (!receiveInitialSnapshot(
            initialReceiveBuffer,
            initialSnapshot,
            snapshotReceivedAt,
            errorMessage))
    {
        running_ = false;
        closeSocketAndCleanup();
        return false;
    }

    Clock::time_point initialSyncStartedAt = Clock::now();
    if (!applyInitialSnapshot(
            initialSnapshot,
            snapshotReceivedAt,
            errorMessage))
    {
        running_ = false;
        closeSocketAndCleanup();
        return false;
    }

    if (metrics != nullptr)
    {
        metrics->initialSyncMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - initialSyncStartedAt
            ).count();
    }

    connected_ = true;
    receiverThread_ = std::thread(
        &SyncClientSession::receiverLoop,
        this,
        std::move(initialReceiveBuffer)
    );
    progressReportThread_ = std::thread(
        &SyncClientSession::progressReportLoop,
        this
    );

    lifecycleLock.unlock();
    emitConnectionChanged(true);
    return true;
}

void SyncClientSession::disconnect()
{
    std::unique_lock<std::mutex> lifecycleLock(lifecycleMutex_);

    running_ = false;
    stopCondition_.notify_all();

    if (socket_ != INVALID_SOCKET)
    {
        shutdown(socket_, SD_BOTH);
    }

    lifecycleLock.unlock();

    if (receiverThread_.joinable() &&
        receiverThread_.get_id() != std::this_thread::get_id())
    {
        receiverThread_.join();
    }

    if (progressReportThread_.joinable() &&
        progressReportThread_.get_id() != std::this_thread::get_id())
    {
        progressReportThread_.join();
    }

    lifecycleLock.lock();
    closeSocketAndCleanup();
    bool wasConnected = connected_.exchange(false);
    mediaIdentity_.clear();
    lifecycleLock.unlock();

    if (wasConnected)
    {
        emitConnectionChanged(false);
    }
}

bool SyncClientSession::sendControlMessage(
    const SyncMessage& message,
    std::string& errorMessage)
{
    if (!isPlaybackControlMessage(message.type))
    {
        errorMessage = "only PLAY, PAUSE and SEEK are client control messages";
        return false;
    }

    if (!connected_)
    {
        errorMessage = "client is not connected";
        return false;
    }

    // 本地操作也必须等待 server 分配 epoch 后回显。这样多个 client
    // 同时操作时，所有播放器都严格按 server 的同一顺序执行。
    if (!sendProtocolMessage(message, errorMessage))
    {
        handleTransportFailure(errorMessage);
        return false;
    }

    emitLog("sent: " + messageTypeToString(message.type));
    return true;
}

bool SyncClientSession::isConnected() const
{
    return connected_;
}

SyncClientPlaybackSnapshot SyncClientSession::getPlaybackSnapshot() const
{
    std::lock_guard<std::mutex> lock(playerMutex_);

    SyncClientPlaybackSnapshot snapshot;
    snapshot.state = localState_;
    snapshot.playerPositionMs = player_.getPositionMilliseconds();
    snapshot.durationMs = player_.getDurationMilliseconds();
    snapshot.state.positionMilliseconds = snapshot.playerPositionMs;
    snapshot.state.positionSeconds =
        static_cast<int>(snapshot.playerPositionMs / 1000);
    return snapshot;
}

bool SyncClientSession::setVolume(int volume)
{
    std::lock_guard<std::mutex> lock(playerMutex_);
    return player_.setVolume(std::clamp(volume, 0, 100));
}

int SyncClientSession::getVolume() const
{
    std::lock_guard<std::mutex> lock(playerMutex_);
    return player_.getVolume();
}

bool SyncClientSession::connectSocket(
    const std::string& serverHost,
    std::string& errorMessage)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addresses = nullptr;
    std::string port = std::to_string(kServerPort);
    int addressResult = getaddrinfo(
        serverHost.c_str(),
        port.c_str(),
        &hints,
        &addresses
    );

    if (addressResult != 0)
    {
        errorMessage = "cannot resolve server address: " + serverHost;
        return false;
    }

    int lastSocketError = 0;
    for (addrinfo* address = addresses;
        address != nullptr;
        address = address->ai_next)
    {
        SOCKET candidate = socket(
            address->ai_family,
            address->ai_socktype,
            address->ai_protocol
        );
        if (candidate == INVALID_SOCKET)
        {
            lastSocketError = WSAGetLastError();
            continue;
        }

        if (connect(
                candidate,
                address->ai_addr,
                static_cast<int>(address->ai_addrlen)) == 0)
        {
            socket_ = candidate;
            break;
        }

        lastSocketError = WSAGetLastError();
        closesocket(candidate);
    }

    freeaddrinfo(addresses);

    if (socket_ == INVALID_SOCKET)
    {
        errorMessage = "connect failed: " +
            std::to_string(lastSocketError);
        return false;
    }

    emitLog("connected to " + serverHost + ":" + port);
    return true;
}

bool SyncClientSession::receiveInitialSnapshot(
    std::string& receiveBuffer,
    SyncMessage& snapshot,
    Clock::time_point& snapshotReceivedAt,
    std::string& errorMessage)
{
    Clock::time_point deadline = Clock::now() + kInitialSnapshotTimeout;
    char buffer[512]{};

    while (running_ && Clock::now() < deadline)
    {
        std::string line;
        while (tryTakeProtocolLine(receiveBuffer, line))
        {
            if (line.empty())
            {
                continue;
            }

            SyncMessage message = stringToMessage(line);
            if (message.type == MessageType::Snapshot)
            {
                snapshot = message;
                snapshotReceivedAt = Clock::now();
                return true;
            }

            if (message.type == MessageType::JoinRejected)
            {
                errorMessage = message.rejectionReason == "MEDIA_MISMATCH"
                    ? "room is already using a different media source"
                    : "server rejected the media join request: " +
                        message.rejectionReason;
                return false;
            }

            if (message.type == MessageType::Ping)
            {
                SyncMessage pong;
                pong.type = MessageType::Pong;
                pong.sequenceNumber = message.sequenceNumber;
                if (!sendProtocolMessage(pong, errorMessage))
                {
                    return false;
                }
                continue;
            }

            errorMessage =
                "protocol error: expected initial SNAPSHOT, received: " +
                line;
            return false;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socket_, &readSet);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;

        int selectResult = select(
            0,
            &readSet,
            nullptr,
            nullptr,
            &timeout
        );
        if (selectResult == SOCKET_ERROR)
        {
            errorMessage = "initial snapshot select failed: " +
                std::to_string(WSAGetLastError());
            return false;
        }

        if (selectResult == 0)
        {
            continue;
        }

        int bytesReceived = recv(socket_, buffer, sizeof(buffer), 0);
        if (bytesReceived > 0)
        {
            receiveBuffer.append(
                buffer,
                static_cast<std::size_t>(bytesReceived)
            );
            continue;
        }

        if (bytesReceived == 0)
        {
            errorMessage =
                "server disconnected before initial snapshot";
        }
        else
        {
            errorMessage = "initial snapshot recv failed: " +
                std::to_string(WSAGetLastError());
        }
        return false;
    }

    errorMessage = running_
        ? "initial snapshot timed out after 5000 ms"
        : "connection cancelled";
    return false;
}

bool SyncClientSession::applyInitialSnapshot(
    const SyncMessage& snapshot,
    Clock::time_point snapshotReceivedAt,
    std::string& errorMessage)
{
    if (snapshot.type != MessageType::Snapshot)
    {
        errorMessage = "initial message is not a SNAPSHOT";
        return false;
    }

    if (snapshot.mediaIdentity != mediaIdentity_)
    {
        errorMessage =
            "initial snapshot media identity does not match the opened media";
        return false;
    }

    Clock::time_point applyStartedAt = Clock::now();
    SyncClientPlaybackSnapshot playbackSnapshot;

    {
        std::lock_guard<std::mutex> lock(playerMutex_);

        bool needsPreparedPosition =
            snapshot.playbackState != PlaybackState::Stopped ||
            snapshot.positionMilliseconds > 0;

        long long targetPositionMs = snapshot.positionMilliseconds;
        if (needsPreparedPosition)
        {
            if (!player_.play())
            {
                errorMessage =
                    "initial snapshot failed: player could not start";
                return false;
            }

            if (!waitUntilPlayerSeekable())
            {
                errorMessage =
                    "initial snapshot failed: media did not become seekable";
                return false;
            }

            if (snapshot.playbackState == PlaybackState::Playing)
            {
                targetPositionMs +=
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        Clock::now() - snapshotReceivedAt
                    ).count();
            }

            if (!player_.seekMilliseconds(targetPositionMs))
            {
                errorMessage =
                    "initial snapshot failed: seek command failed";
                return false;
            }

            if (snapshot.playbackState != PlaybackState::Playing &&
                !player_.pause())
            {
                errorMessage =
                    "initial snapshot failed: pause command failed";
                return false;
            }
        }

        applyMessageToState(snapshot, localState_);
        currentControlEpoch_ = snapshot.controlEpoch;
        localState_.positionMilliseconds = targetPositionMs;
        localState_.positionSeconds =
            static_cast<int>(targetPositionMs / 1000);

        playbackSnapshot.state = localState_;
        playbackSnapshot.playerPositionMs =
            player_.getPositionMilliseconds();
        playbackSnapshot.durationMs =
            player_.getDurationMilliseconds();
    }

    long long applyDurationMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - applyStartedAt
        ).count();

    std::ostringstream metric;
    metric << "[metric] type=initial_sync"
        << " epoch=" << snapshot.controlEpoch
        << " state=" << stateToString(snapshot.playbackState)
        << " snapshot_pos_ms=" << snapshot.positionMilliseconds
        << " target_pos_ms=" << playbackSnapshot.state.positionMilliseconds
        << " apply_ms=" << applyDurationMs
        << " success=1";
    emitLog(metric.str());
    emitPlaybackChanged(
        playbackSnapshot,
        SyncClientCommandSource::InitialSnapshot
    );
    return true;
}

bool SyncClientSession::waitUntilPlayerSeekable()
{
    Clock::time_point deadline = Clock::now() + kPlayerSeekableTimeout;
    while (running_ && Clock::now() < deadline)
    {
        if (player_.isSeekable())
        {
            return true;
        }

        std::this_thread::sleep_for(kPlayerReadyPollInterval);
    }

    return running_ && player_.isSeekable();
}

bool SyncClientSession::applyControlMessage(
    const SyncMessage& message,
    SyncClientCommandSource source,
    std::string& errorMessage)
{
    SyncClientPlaybackSnapshot snapshot;

    {
        std::lock_guard<std::mutex> lock(playerMutex_);

        if (source == SyncClientCommandSource::RemoteCommand)
        {
            if (message.controlEpoch <= currentControlEpoch_)
            {
                errorMessage = "ignored stale or duplicate CONTROL epoch";
                return false;
            }
            if (message.controlEpoch != currentControlEpoch_ + 1)
            {
                errorMessage = "CONTROL epoch gap detected";
                return false;
            }
        }

        if (!applyMessageToPlayer(message, player_))
        {
            errorMessage = std::string(sourceLabel(source)) +
                " player command failed";
            return false;
        }

        applyMessageToState(message, localState_);
        if (source == SyncClientCommandSource::RemoteCommand)
        {
            currentControlEpoch_ = message.controlEpoch;
        }
        long long playerPositionMs =
            player_.getPositionMilliseconds();
        localState_.positionMilliseconds = playerPositionMs;
        localState_.positionSeconds =
            static_cast<int>(playerPositionMs / 1000);

        snapshot.state = localState_;
        snapshot.playerPositionMs = playerPositionMs;
        snapshot.durationMs = player_.getDurationMilliseconds();
    }

    emitLog(
        std::string(sourceLabel(source)) +
        " applied: " +
        syncStateToString(snapshot.state)
    );
    emitPlaybackChanged(snapshot, source);
    return true;
}

bool SyncClientSession::sendProtocolMessage(
    const SyncMessage& message,
    std::string& errorMessage)
{
    std::lock_guard<std::mutex> lock(sendMutex_);

    if (socket_ == INVALID_SOCKET)
    {
        errorMessage = "send failed: socket is closed";
        return false;
    }

    return sendAll(socket_, messageToString(message), errorMessage);
}

bool SyncClientSession::executeCorrectionMessage(
    const SyncMessage& message,
    std::string& errorMessage)
{
    SyncClientPlaybackSnapshot playbackSnapshot;
    SyncCorrectionExecution execution;

    {
        std::lock_guard<std::mutex> lock(playerMutex_);
        execution = executeSyncCorrection(
            message,
            currentControlEpoch_,
            localState_,
            player_
        );

        if (execution.applied)
        {
            playbackSnapshot.state = localState_;
            playbackSnapshot.playerPositionMs =
                execution.resultMessage.positionMilliseconds;
            playbackSnapshot.durationMs = player_.getDurationMilliseconds();
        }
    }

    std::ostringstream metric;
    metric << "[metric] type=correction_execution"
        << " command_id=" << message.commandId
        << " epoch=" << message.controlEpoch
        << " state=" << stateToString(message.playbackState)
        << " forward_ms=" << message.correctionForwardMilliseconds
        << " target_pos_ms=" << execution.requestedTargetMilliseconds
        << " observed_pos_ms="
        << execution.resultMessage.positionMilliseconds
        << " status="
        << correctionResultStatusToString(
            execution.resultMessage.correctionResultStatus
        )
        << " reason=" << execution.resultMessage.correctionReason;
    emitLog(metric.str());

    if (execution.applied)
    {
        emitPlaybackChanged(
            playbackSnapshot,
            SyncClientCommandSource::Correction
        );
    }

    return sendProtocolMessage(execution.resultMessage, errorMessage);
}

void SyncClientSession::receiverLoop(std::string receiveBuffer)
{
    char buffer[512]{};

    while (running_)
    {
        std::string line;
        while (tryTakeProtocolLine(receiveBuffer, line))
        {
            if (line.empty())
            {
                continue;
            }

            SyncMessage message = stringToMessage(line);
            if (message.type == MessageType::Unknown)
            {
                emitLog("ignored unknown server message: " + line);
                continue;
            }

            if (message.type == MessageType::Ping)
            {
                SyncMessage pong;
                pong.type = MessageType::Pong;
                pong.sequenceNumber = message.sequenceNumber;

                std::string errorMessage;
                if (!sendProtocolMessage(pong, errorMessage))
                {
                    handleTransportFailure(errorMessage);
                    return;
                }
                continue;
            }

            if (message.type == MessageType::Correction)
            {
                std::string errorMessage;
                if (!executeCorrectionMessage(message, errorMessage))
                {
                    handleTransportFailure(errorMessage);
                    return;
                }
                continue;
            }

            if (message.type == MessageType::Pong ||
                message.type == MessageType::Report ||
                message.type == MessageType::Join ||
                message.type == MessageType::JoinRejected ||
                message.type == MessageType::Snapshot ||
                message.type == MessageType::CorrectionResult)
            {
                continue;
            }

            std::string errorMessage;
            if (!applyControlMessage(
                    message,
                    SyncClientCommandSource::RemoteCommand,
                    errorMessage))
            {
                if (errorMessage == "ignored stale or duplicate CONTROL epoch")
                {
                    emitLog(errorMessage);
                    continue;
                }

                emitError(errorMessage);
                if (errorMessage == "CONTROL epoch gap detected")
                {
                    handleTransportFailure(errorMessage);
                    return;
                }
            }
        }

        if (!running_)
        {
            break;
        }

        int bytesReceived = recv(socket_, buffer, sizeof(buffer), 0);
        if (bytesReceived > 0)
        {
            receiveBuffer.append(
                buffer,
                static_cast<std::size_t>(bytesReceived)
            );
            continue;
        }

        if (bytesReceived == 0)
        {
            handleTransportFailure("server disconnected");
        }
        else if (running_)
        {
            handleTransportFailure(
                "recv failed: " + std::to_string(WSAGetLastError())
            );
        }
        break;
    }
}

void SyncClientSession::progressReportLoop()
{
    while (running_)
    {
        SyncMessage report;
        report.type = MessageType::Report;

        {
            std::lock_guard<std::mutex> lock(playerMutex_);
            report.controlEpoch = currentControlEpoch_;
            report.positionMilliseconds =
                player_.getPositionMilliseconds();
            report.positionSeconds =
                static_cast<int>(report.positionMilliseconds / 1000);
            report.playbackState = localState_.state;
        }

        std::string errorMessage;
        if (!sendProtocolMessage(report, errorMessage))
        {
            handleTransportFailure(errorMessage);
            break;
        }

        std::unique_lock<std::mutex> stopLock(stopMutex_);
        stopCondition_.wait_for(
            stopLock,
            kProgressReportInterval,
            [this]()
            {
                return !running_;
            }
        );
    }
}

void SyncClientSession::handleTransportFailure(
    const std::string& errorMessage)
{
    bool wasRunning = running_.exchange(false);
    stopCondition_.notify_all();

    if (socket_ != INVALID_SOCKET)
    {
        shutdown(socket_, SD_BOTH);
    }

    if (wasRunning)
    {
        emitError(errorMessage);
    }

    if (connected_.exchange(false))
    {
        emitConnectionChanged(false);
    }
}

void SyncClientSession::closeSocketAndCleanup()
{
    if (socket_ != INVALID_SOCKET)
    {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    if (winsockInitialized_)
    {
        WSACleanup();
        winsockInitialized_ = false;
    }
}

void SyncClientSession::emitLog(const std::string& message) const
{
    if (callbacks_.onLog)
    {
        callbacks_.onLog(message);
    }
}

void SyncClientSession::emitError(const std::string& message) const
{
    if (callbacks_.onError)
    {
        callbacks_.onError(message);
    }
}

void SyncClientSession::emitConnectionChanged(bool connected) const
{
    if (callbacks_.onConnectionChanged)
    {
        callbacks_.onConnectionChanged(connected);
    }
}

void SyncClientSession::emitPlaybackChanged(
    const SyncClientPlaybackSnapshot& snapshot,
    SyncClientCommandSource source) const
{
    if (callbacks_.onPlaybackChanged)
    {
        callbacks_.onPlaybackChanged(snapshot, source);
    }
}
