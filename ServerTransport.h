#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

using ConnectionId = std::uint64_t;
using ServerFrame = std::shared_ptr<const std::string>;
using ServerClock = std::chrono::steady_clock;
inline constexpr auto kJoinTimeout = std::chrono::seconds(5);
inline constexpr auto kHeartbeatInterval = std::chrono::seconds(1);
inline constexpr std::size_t kMaxProtocolLineBytes = 4096;

struct ServerWrite
{
    ServerFrame frame;
    // Called once, after the full frame is written to the kernel or abandoned.
    std::function<void(bool)> completed;
    // Checked before the first byte. A partially written frame must be finished.
    std::function<bool()> stillRelevant;
};

struct ServerPeer
{
    ConnectionId connectionId;
    ServerClock::time_point acceptedAt = ServerClock::now();
    int clientId = 0;
    std::string sessionId{};
    bool joinResultRecorded = false;
    bool snapshotWritten = false;
};

struct ServerTransportStats
{
    std::size_t queuedBytes = 0;
    std::size_t peakQueuedBytes = 0;
    std::uint64_t queueLimitDisconnects = 0;
    std::uint64_t sendWouldBlock = 0;
    std::uint64_t partialSends = 0;
};

class ServerTransport
{
public:
    virtual ~ServerTransport() = default;
    // Write completions run before the connection and its peer are released.
    virtual void write(ConnectionId id, ServerWrite message) = 0;
    virtual void closeAfterWrites(ConnectionId id) = 0;
};

// Transport serializes these calls and all write completions.
class ServerEvents
{
public:
    virtual ~ServerEvents() = default;
    virtual void started() = 0;
    virtual void stopped() = 0;
    virtual void receivedLine(ServerTransport&, ServerPeer&, const std::string&) = 0;
    virtual void joinTimedOut(ServerTransport&, ServerPeer&) = 0;
    virtual void disconnected(ServerPeer&, const char* reason, int error) = 0;
    virtual void heartbeat(ServerTransport&) = 0;
    virtual void transportStats(const ServerTransportStats&) = 0;
};

void runServerTransport(unsigned short port, ServerEvents& events);
