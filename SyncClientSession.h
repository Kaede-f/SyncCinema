#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <winsock2.h>

#include "PlayerController.h"
#include "Protocol.h"

enum class SyncClientCommandSource
{
    InitialSnapshot,
    LocalCommand,
    RemoteCommand
};

struct SyncClientPlaybackSnapshot
{
    SyncState state;
    long long playerPositionMs = 0;
    long long durationMs = 0;
};

struct SyncClientConnectMetrics
{
    long long connectServerMs = 0;
    long long initialSyncMs = 0;
};

// Session 本身不依赖 Qt，也不直接读写控制台。
// CLI 和 Qt 通过回调选择如何展示连接、播放状态和错误。
struct SyncClientCallbacks
{
    std::function<void(const std::string&)> onLog;
    std::function<void(const std::string&)> onError;
    std::function<void(bool)> onConnectionChanged;
    std::function<void(
        const SyncClientPlaybackSnapshot&,
        SyncClientCommandSource)> onPlaybackChanged;
};

// SyncClientSession 封装一个客户端连接的完整生命周期：
//   1. 连接 server 并接收初始 SNAPSHOT；
//   2. 后台接收广播和响应 PING；
//   3. 每秒上报真实播放器进度；
//   4. 串行化同一 TCP socket 上的全部发送。
//
// 它不拥有 PlayerController。调用方必须保证 player 的生命周期长于 session。
class SyncClientSession
{
public:
    explicit SyncClientSession(
        PlayerController& player,
        SyncClientCallbacks callbacks = {}
    );
    ~SyncClientSession();

    SyncClientSession(const SyncClientSession&) = delete;
    SyncClientSession& operator=(const SyncClientSession&) = delete;

    bool connectToRoom(
        const std::string& serverHost,
        const std::string& mediaSource,
        std::string& errorMessage,
        SyncClientConnectMetrics* metrics = nullptr
    );

    void disconnect();

    bool sendControlMessage(
        const SyncMessage& message,
        std::string& errorMessage
    );

    bool isConnected() const;
    SyncClientPlaybackSnapshot getPlaybackSnapshot() const;

    bool setVolume(int volume);
    int getVolume() const;

private:
    using Clock = std::chrono::steady_clock;

    bool connectSocket(const std::string& serverHost, std::string& errorMessage);
    bool receiveInitialSnapshot(
        std::string& receiveBuffer,
        SyncMessage& snapshot,
        Clock::time_point& snapshotReceivedAt,
        std::string& errorMessage
    );
    bool applyInitialSnapshot(
        const SyncMessage& snapshot,
        Clock::time_point snapshotReceivedAt,
        std::string& errorMessage
    );
    bool waitUntilPlayerSeekable();

    bool applyControlMessage(
        const SyncMessage& message,
        SyncClientCommandSource source,
        std::string& errorMessage
    );
    bool sendProtocolMessage(
        const SyncMessage& message,
        std::string& errorMessage
    );

    void receiverLoop(std::string receiveBuffer);
    void progressReportLoop();
    void handleTransportFailure(const std::string& errorMessage);
    void closeSocketAndCleanup();

    void emitLog(const std::string& message) const;
    void emitError(const std::string& message) const;
    void emitConnectionChanged(bool connected) const;
    void emitPlaybackChanged(
        const SyncClientPlaybackSnapshot& snapshot,
        SyncClientCommandSource source
    ) const;

    PlayerController& player_;
    SyncClientCallbacks callbacks_;

    mutable std::mutex lifecycleMutex_;
    mutable std::mutex playerMutex_;
    std::mutex sendMutex_;
    std::mutex stopMutex_;
    std::condition_variable stopCondition_;

    SOCKET socket_ = INVALID_SOCKET;
    bool winsockInitialized_ = false;
    std::atomic_bool running_{ false };
    std::atomic_bool connected_{ false };

    SyncState localState_;
    std::string mediaIdentity_;
    std::thread receiverThread_;
    std::thread progressReportThread_;
};
