#include "ServerTransport.h"

#include "NetSocket.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <deque>
#include <set>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#endif

namespace
{
    constexpr ConnectionId kListener = 0;
    constexpr ConnectionId kStopSignal = 1;
    constexpr std::size_t kMaxConnections = 4096;
    constexpr std::size_t kMaxQueuedBytes = 64 * 1024;
    constexpr std::size_t kMaxQueuedFrames = 2048;
    constexpr auto kDrainTimeout = std::chrono::seconds(1);
    constexpr auto kAcceptRetry = std::chrono::milliseconds(250);
    constexpr auto kFlushTimeout = std::chrono::seconds(5);

    enum Interest : unsigned
    {
        Read = 1,
        Write = 2,
        Hangup = 4,
        Error = 8
    };

    struct Ready
    {
        ConnectionId id;
        unsigned flags;
    };

    [[noreturn]] void socketFailure(const char* operation)
    {
        throw std::system_error(getSocketError(), std::system_category(), operation);
    }

    bool interrupted(int error)
    {
#ifdef _WIN32
        return error == WSAEINTR;
#else
        return error == EINTR;
#endif
    }

    bool wouldBlock(int error)
    {
#ifdef _WIN32
        return error == WSAEWOULDBLOCK;
#else
        return error == EAGAIN || error == EWOULDBLOCK;
#endif
    }

    bool retryAccept(int error)
    {
#ifdef __linux__
        // Linux accept can propagate pending network errors from the new socket.
        return error == ECONNABORTED || error == ENETDOWN || error == EPROTO ||
            error == ENOPROTOOPT || error == EHOSTDOWN || error == ENONET ||
            error == EHOSTUNREACH || error == EOPNOTSUPP || error == ENETUNREACH;
#elif defined(_WIN32)
        return error == WSAECONNRESET;
#else
        return error == ECONNABORTED;
#endif
    }

    class Socket
    {
    public:
        explicit Socket(SocketHandle socket = kInvalidSocket) : socket_(socket) {}
        ~Socket() { reset(); }

        Socket(const Socket&) = delete;
        Socket& operator=(const Socket&) = delete;
        Socket(Socket&& other) noexcept : socket_(std::exchange(other.socket_, kInvalidSocket)) {}

        SocketHandle get() const { return socket_; }

        void reset()
        {
            if (socket_ != kInvalidSocket)
            {
                closeSocket(std::exchange(socket_, kInvalidSocket));
            }
        }

    private:
        SocketHandle socket_;
    };

#ifdef __linux__
    class StopSignals
    {
    public:
        StopSignals()
        {
            sigemptyset(&mask_);
            sigaddset(&mask_, SIGINT);
            sigaddset(&mask_, SIGTERM);
            const int error = pthread_sigmask(SIG_BLOCK, &mask_, &previous_);
            if (error)
            {
                throw std::system_error(error, std::system_category(), "pthread_sigmask");
            }
        }

        ~StopSignals() { pthread_sigmask(SIG_SETMASK, &previous_, nullptr); }

        int open() const
        {
            const int fd = signalfd(-1, &mask_, SFD_NONBLOCK | SFD_CLOEXEC);
            if (fd == -1)
            {
                socketFailure("signalfd");
            }
            return fd;
        }

    private:
        sigset_t mask_{};
        sigset_t previous_{};
    };

    class Readiness
    {
    public:
        Readiness() : signal_(signals_.open()), epoll_(epoll_create1(EPOLL_CLOEXEC))
        {
            if (epoll_.get() == -1)
            {
                socketFailure("epoll_create1");
            }
            add(signal_.get(), kStopSignal, Read);
        }

        void add(SocketHandle socket, ConnectionId id, unsigned interests)
        {
            if (!update(EPOLL_CTL_ADD, socket, id, interests))
            {
                socketFailure("epoll add");
            }
        }

        bool change(SocketHandle socket, ConnectionId id, unsigned interests)
        {
            return update(EPOLL_CTL_MOD, socket, id, interests);
        }

        void remove(SocketHandle socket, ConnectionId)
        {
            epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, socket, nullptr);
        }

        bool wait(int timeoutMs, std::vector<Ready>& ready)
        {
            std::array<epoll_event, 128> events{};
            const int count = epoll_wait(epoll_.get(), events.data(), events.size(), timeoutMs);
            if (count < 0 && !interrupted(errno))
            {
                socketFailure("epoll_wait");
            }
            for (int index = 0; index < count; ++index)
            {
                const auto& event = events[index];
                if (event.data.u64 == kStopSignal)
                {
                    // Consume the pending signals before restoring the caller's signal mask.
                    signalfd_siginfo signal{};
                    while (read(signal_.get(), &signal, sizeof(signal)) > 0) {}
                    return false;
                }
                unsigned flags = 0;
                if (event.events & EPOLLIN) flags |= Read;
                if (event.events & EPOLLOUT) flags |= Write;
                if (event.events & (EPOLLHUP | EPOLLRDHUP)) flags |= Hangup;
                if (event.events & EPOLLERR) flags |= Error;
                ready.push_back({event.data.u64, flags});
            }
            return true;
        }

    private:
        bool update(int operation, SocketHandle socket, ConnectionId id, unsigned interests)
        {
            epoll_event event{};
            event.data.u64 = id;
            if (interests & Read) event.events |= EPOLLIN | EPOLLRDHUP;
            if (interests & Write) event.events |= EPOLLOUT;
            return epoll_ctl(epoll_.get(), operation, socket, &event) == 0;
        }

        StopSignals signals_;
        Socket signal_;
        Socket epoll_;
    };

    Socket openSocket()
    {
        return Socket(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP));
    }

    Socket acceptSocket(SocketHandle listener)
    {
        return Socket(accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
    }

    void reserveDescriptorTable(SocketHandle listener)
    {
        rlimit limits{};
        if (getrlimit(RLIMIT_NOFILE, &limits) != 0 || limits.rlim_cur == 0)
        {
            return;
        }

        // Include the listener, reactor, metrics and log descriptors alongside
        // playback connections. Linux keeps the larger table after the dup closes.
        constexpr rlim_t target = kMaxConnections + 16;
        const auto slot = static_cast<int>((std::min)(target, limits.rlim_cur - 1));
        const Socket reservation(fcntl(listener, F_DUPFD_CLOEXEC, slot));
    }
#else
    static_assert(std::atomic_bool::is_always_lock_free);
    std::atomic_bool stopRequested{false};

    void requestStop(int)
    {
        stopRequested.store(true, std::memory_order_relaxed);
    }

#ifdef _WIN32
    BOOL WINAPI requestConsoleStop(DWORD event)
    {
        if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
        {
            return FALSE;
        }
        requestStop(0);
        return TRUE;
    }

    using PollFd = WSAPOLLFD;
#else
    using PollFd = pollfd;
#endif

    class Readiness
    {
    public:
        Readiness()
        {
            stopRequested.store(false, std::memory_order_relaxed);
            previousInt_ = std::signal(SIGINT, requestStop);
            previousTerm_ = std::signal(SIGTERM, requestStop);
#ifdef _WIN32
            hasConsoleHandler_ = SetConsoleCtrlHandler(requestConsoleStop, TRUE) != FALSE;
#endif
        }

        ~Readiness()
        {
#ifdef _WIN32
            if (hasConsoleHandler_) SetConsoleCtrlHandler(requestConsoleStop, FALSE);
#endif
            if (previousInt_ != SIG_ERR) std::signal(SIGINT, previousInt_);
            if (previousTerm_ != SIG_ERR) std::signal(SIGTERM, previousTerm_);
        }

        void add(SocketHandle socket, ConnectionId id, unsigned interests)
        {
            registrations_.emplace(id, PollFd{socket, mask(interests), 0});
        }

        bool change(SocketHandle, ConnectionId id, unsigned interests)
        {
            registrations_.at(id).events = mask(interests);
            return true;
        }

        void remove(SocketHandle, ConnectionId id) { registrations_.erase(id); }

        bool wait(int timeoutMs, std::vector<Ready>& ready)
        {
            sockets_.clear();
            ids_.clear();
            for (const auto& [id, socket] : registrations_)
            {
                sockets_.push_back(socket);
                ids_.push_back(id);
            }
            timeoutMs = (std::min)(timeoutMs, 100);
#ifdef _WIN32
            if (sockets_.empty())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
                return !stopRequested.load(std::memory_order_relaxed);
            }
            const int count = WSAPoll(sockets_.data(), static_cast<ULONG>(sockets_.size()), timeoutMs);
#else
            const int count = poll(sockets_.data(), sockets_.size(), timeoutMs);
#endif
            if (stopRequested.load(std::memory_order_relaxed))
            {
                return false;
            }
            if (count < 0 && !interrupted(getSocketError()))
            {
                socketFailure("poll");
            }
            if (count <= 0)
            {
                return true;
            }
            for (std::size_t index = 0; index < sockets_.size(); ++index)
            {
                const short events = sockets_[index].revents;
                unsigned flags = 0;
                if (events & POLLIN) flags |= Read;
                if (events & POLLOUT) flags |= Write;
                if (events & POLLHUP) flags |= Hangup;
                if (events & (POLLERR | POLLNVAL)) flags |= Error;
                if (flags) ready.push_back({ids_[index], flags});
            }
            return true;
        }

    private:
        static short mask(unsigned interests)
        {
#ifdef _WIN32
            return static_cast<short>(((interests & Read) ? POLLRDNORM : 0) |
                ((interests & Write) ? POLLWRNORM : 0));
#else
            return static_cast<short>(((interests & Read) ? POLLIN : 0) |
                ((interests & Write) ? POLLOUT : 0));
#endif
        }

        using SignalHandler = void (*)(int);
        SignalHandler previousInt_{};
        SignalHandler previousTerm_{};
#ifdef _WIN32
        bool hasConsoleHandler_ = false;
#endif
        std::unordered_map<ConnectionId, PollFd> registrations_;
        std::vector<PollFd> sockets_;
        std::vector<ConnectionId> ids_;
    };

    Socket makeNonblocking(Socket socket)
    {
        if (socket.get() == kInvalidSocket)
        {
            return socket;
        }
#ifdef _WIN32
        u_long enabled = 1;
        if (ioctlsocket(socket.get(), FIONBIO, &enabled) == kSocketError)
#else
        if (fcntl(socket.get(), F_SETFL, O_NONBLOCK) == -1 ||
            fcntl(socket.get(), F_SETFD, FD_CLOEXEC) == -1)
#endif
        {
            socketFailure("set socket nonblocking");
        }
#ifdef SO_NOSIGPIPE
        const int enabled = 1;
        setsockopt(socket.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
        return socket;
    }

    Socket openSocket()
    {
        return makeNonblocking(Socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)));
    }

    Socket acceptSocket(SocketHandle listener)
    {
        return makeNonblocking(Socket(accept(listener, nullptr, nullptr)));
    }
#endif

    Socket openListener(unsigned short port)
    {
        Socket listener = openSocket();
        if (listener.get() == kInvalidSocket)
        {
            socketFailure("socket");
        }

        const int enabled = 1;
#ifdef _WIN32
        setsockopt(listener.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&enabled), sizeof(enabled));
#else
        setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);
        if (bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == kSocketError)
        {
            socketFailure("bind");
        }
        if (listen(listener.get(), SOMAXCONN) == kSocketError)
        {
            socketFailure("listen");
        }
        return listener;
    }

    void complete(ServerWrite message, bool sent)
    {
        if (message.completed)
        {
            message.completed(sent);
        }
    }

    enum class Phase { Reading, Draining, Closing };

    struct Connection
    {
        Connection(Socket socket, ConnectionId id)
            : socket(std::move(socket)), peer{id}
        {
        }

        Socket socket;
        ServerPeer peer;
        std::string input;
        std::deque<ServerWrite> output;
        std::size_t queuedBytes = 0;
        std::size_t offset = 0;
        unsigned interests = Read;
        Phase phase = Phase::Reading;
        ServerClock::time_point deadline{};
        ServerClock::time_point lastWriteProgress{};
        const char* closeReason = "peer_closed";
        int closeError = 0;
    };

    class Reactor final : public ServerTransport
    {
    public:
        Reactor(unsigned short port, ServerEvents& events)
            : events_(events), listener_(openListener(port))
        {
            readiness_.add(listener_.get(), kListener, Read);
#ifdef __linux__
            // Grow the fd table before started() launches the metrics thread.
            // Once it is shared, Linux can wait for RCU during accept4 growth.
            reserveDescriptorTable(listener_.get());
#endif
        }

        void run()
        {
            try
            {
                events_.started();
                loop();
            }
            catch (const std::system_error&)
            {
                closeAll();
                events_.stopped();
                throw;
            }
            closeAll();
            events_.transportStats(stats_);
            events_.stopped();
        }

        void write(ConnectionId id, ServerWrite message) override
        {
            const auto found = connections_.find(id);
            if (found == connections_.end() || found->second.phase != Phase::Reading)
            {
                complete(std::move(message), false);
                return;
            }
            auto& connection = found->second;
            if (message.frame->size() > kMaxQueuedBytes - connection.queuedBytes ||
                connection.output.size() >= kMaxQueuedFrames)
            {
                ++stats_.queueLimitDisconnects;
                closeSoon(connection, "send_queue_limit");
                complete(std::move(message), false);
                return;
            }
            const bool first = connection.output.empty();
            connection.queuedBytes += message.frame->size();
            stats_.queuedBytes += message.frame->size();
            stats_.peakQueuedBytes = (std::max)(stats_.peakQueuedBytes, stats_.queuedBytes);
            connection.output.push_back(std::move(message));
            if (first)
            {
                connection.lastWriteProgress = ServerClock::now();
                if (connection.peer.clientId != 0)
                {
                    setDeadline(connection, connection.lastWriteProgress + kFlushTimeout);
                }
            }
            updateInterests(connection);
        }

        void closeAfterWrites(ConnectionId id) override
        {
            drain(connections_.at(id), "join_rejected");
        }

    private:
        void loop()
        {
            std::vector<Ready> ready;
            ready.reserve(128);
            auto nextHeartbeat = ServerClock::now() + kHeartbeatInterval;
            while (true)
            {
                auto now = ServerClock::now();
                processDeadlines(now);
                if (now >= nextHeartbeat)
                {
                    events_.transportStats(stats_);
                    events_.heartbeat(*this);
                    nextHeartbeat = now + kHeartbeatInterval;
                }
                reapClosed();

                auto next = nextHeartbeat;
                if (!deadlines_.empty()) next = (std::min)(next, deadlines_.begin()->first);
                if (resumeAccept_ != ServerClock::time_point{}) next = (std::min)(next, resumeAccept_);
                now = ServerClock::now();
                const auto delay = std::chrono::ceil<std::chrono::milliseconds>(next - now).count();
                ready.clear();
                if (!readiness_.wait(static_cast<int>((std::max)(0LL, static_cast<long long>(delay))), ready))
                {
                    return;
                }
                for (const auto& event : ready)
                {
                    now = ServerClock::now();
                    if (!deadlines_.empty() && deadlines_.begin()->first <= now)
                    {
                        processDeadlines(now);
                        reapClosed();
                    }

                    if (event.id == kListener)
                    {
                        if (event.flags & (Error | Hangup))
                        {
                            throw std::system_error(std::make_error_code(std::errc::io_error), "listener");
                        }
                        acceptReady();
                    }
                    else if (auto found = connections_.find(event.id); found != connections_.end())
                    {
                        service(found->second, event.flags);
                    }
                    reapClosed();
                }
            }
        }

        void processDeadlines(ServerClock::time_point now)
        {
            if (resumeAccept_ != ServerClock::time_point{} && resumeAccept_ <= now)
            {
                readiness_.add(listener_.get(), kListener, Read);
                resumeAccept_ = {};
            }
            while (!deadlines_.empty() && deadlines_.begin()->first <= now)
            {
                auto& connection = connections_.at(deadlines_.begin()->second);
                setDeadline(connection, {});
                if (connection.phase == Phase::Reading && connection.peer.clientId == 0)
                {
                    events_.joinTimedOut(*this, connection.peer);
                }
                else if (connection.phase == Phase::Reading)
                {
                    const auto expires = connection.lastWriteProgress + kFlushTimeout;
                    if (expires <= now) closeSoon(connection, "send_timeout");
                    else setDeadline(connection, expires);
                }
                else if (connection.phase == Phase::Draining)
                {
                    closeSoon(connection, "drain_timeout");
                }
            }
        }

        void setDeadline(Connection& connection, ServerClock::time_point deadline)
        {
            const auto id = connection.peer.connectionId;
            if (connection.deadline != ServerClock::time_point{})
            {
                deadlines_.erase({connection.deadline, id});
            }
            connection.deadline = deadline;
            if (deadline != ServerClock::time_point{})
            {
                deadlines_.emplace(deadline, id);
            }
        }

        void acceptReady()
        {
            if (resumeAccept_ != ServerClock::time_point{})
            {
                return;
            }
            for (int attempt = 0; attempt < 64; ++attempt)
            {
                Socket socket = acceptSocket(listener_.get());
                if (socket.get() == kInvalidSocket)
                {
                    const int error = getSocketError();
                    if (wouldBlock(error)) return;
                    if (interrupted(error) || retryAccept(error)) continue;

                    // Resource exhaustion can leave the listener permanently readable.
                    // Existing connections and heartbeat continue during the retry delay.
                    readiness_.remove(listener_.get(), kListener);
                    resumeAccept_ = ServerClock::now() + kAcceptRetry;
                    return;
                }
                if (connections_.size() >= kMaxConnections)
                {
                    continue;
                }
                const ConnectionId id = nextId_++;
                auto& connection = connections_.try_emplace(id, std::move(socket), id).first->second;
                readiness_.add(connection.socket.get(), id, Read);
                setDeadline(connection, connection.peer.acceptedAt + kJoinTimeout);
            }
        }

        void service(Connection& connection, unsigned flags)
        {
            if (connection.phase == Phase::Reading && (flags & (Read | Hangup | Error)))
            {
                readInput(connection);
            }
            if (connection.phase == Phase::Closing)
            {
                return;
            }
            if (flags & Error)
            {
                closeSoon(connection, "socket_error");
                return;
            }
            if (!connection.output.empty() && (flags & (Write | Hangup)))
            {
                flushOutput(connection);
            }
        }

        void readInput(Connection& connection)
        {
            // Limit work per readiness notification. LT reports any unread kernel data again.
            std::array<char, 512> bytes{};
            int count;
            int error;
            do
            {
                count = recv(connection.socket.get(), bytes.data(), static_cast<int>(bytes.size()), 0);
                error = count < 0 ? getSocketError() : 0;
            }
            while (count < 0 && interrupted(error));

            if (count == 0)
            {
                if (connection.peer.clientId == 0)
                {
                    events_.joinTimedOut(*this, connection.peer);
                }
                else
                {
                    drain(connection, "peer_closed");
                }
                return;
            }
            if (count < 0)
            {
                if (!wouldBlock(error)) closeSoon(connection, "recv_error", error);
                return;
            }
            std::string_view input(bytes.data(), static_cast<std::size_t>(count));
            while (!input.empty() && connection.phase == Phase::Reading)
            {
                const auto newline = input.find('\n');
                const auto fragment = input.substr(0, newline);
                if (fragment.size() > kMaxProtocolLineBytes - connection.input.size())
                {
                    if (connection.peer.clientId == 0)
                    {
                        events_.joinTimedOut(*this, connection.peer);
                    }
                    else
                    {
                        closeSoon(connection, "protocol_line_limit");
                    }
                    return;
                }
                connection.input.append(fragment);
                if (newline == std::string_view::npos)
                {
                    break;
                }
                if (!connection.input.empty() && connection.input.back() == '\r')
                {
                    connection.input.pop_back();
                }
                const bool waitingForJoin = connection.peer.clientId == 0;
                events_.receivedLine(*this, connection.peer, connection.input);
                if (waitingForJoin && connection.peer.clientId != 0)
                {
                    setDeadline(connection, connection.lastWriteProgress + kFlushTimeout);
                }
                connection.input.clear();
                input.remove_prefix(newline + 1);
            }
        }

        void flushOutput(Connection& connection)
        {
            std::size_t budget = 32 * 1024;
            while (!connection.output.empty() && budget > 0)
            {
                auto& message = connection.output.front();
                if (connection.offset == 0 && message.stillRelevant && !message.stillRelevant())
                {
                    connection.queuedBytes -= message.frame->size();
                    stats_.queuedBytes -= message.frame->size();
                    popOutput(connection, false);
                    continue;
                }

                const auto bytes = (std::min)(budget, message.frame->size() - connection.offset);
                const int count = sendSocket(connection.socket.get(),
                    message.frame->data() + connection.offset, static_cast<int>(bytes));
                if (count <= 0)
                {
                    const int error = count < 0 ? getSocketError() : 0;
                    if (interrupted(error)) continue;
                    if (wouldBlock(error)) ++stats_.sendWouldBlock;
                    else closeSoon(connection, "send_error", error);
                    return;
                }

                const auto sent = static_cast<std::size_t>(count);
                if (sent < bytes) ++stats_.partialSends;
                connection.offset += sent;
                connection.lastWriteProgress = ServerClock::now();
                connection.queuedBytes -= sent;
                stats_.queuedBytes -= sent;
                budget -= sent;
                if (connection.offset == message.frame->size())
                {
                    popOutput(connection, true);
                }
            }

            if (connection.phase == Phase::Draining && connection.output.empty())
            {
                closeSoon(connection, connection.closeReason);
            }
            else
            {
                if (connection.output.empty()) setDeadline(connection, {});
                updateInterests(connection);
            }
        }

        void popOutput(Connection& connection, bool sent)
        {
            ServerWrite message = std::move(connection.output.front());
            connection.output.pop_front();
            connection.offset = 0;
            complete(std::move(message), sent);
        }

        void updateInterests(Connection& connection)
        {
            const unsigned interests =
                (connection.phase == Phase::Reading ? Read : 0u) |
                (connection.output.empty() ? 0u : Write);
            if (connection.interests != interests && connection.phase != Phase::Closing)
            {
                if (!readiness_.change(connection.socket.get(), connection.peer.connectionId, interests))
                {
                    closeSoon(connection, "readiness_error", getSocketError());
                    return;
                }
                connection.interests = interests;
            }
        }

        void drain(Connection& connection, const char* reason)
        {
            if (connection.phase != Phase::Reading)
            {
                return;
            }
            if (connection.output.empty())
            {
                closeSoon(connection, reason);
                return;
            }
            connection.phase = Phase::Draining;
            connection.closeReason = reason;
            setDeadline(connection, ServerClock::now() + kDrainTimeout);
            updateInterests(connection);
        }

        void closeSoon(Connection& connection, const char* reason, int error = 0)
        {
            if (connection.phase != Phase::Closing)
            {
                connection.phase = Phase::Closing;
                connection.closeReason = reason;
                connection.closeError = error;
                closing_.push_back(connection.peer.connectionId);
            }
        }

        void reapClosed()
        {
            for (const auto id : closing_)
            {
                auto found = connections_.find(id);
                auto& connection = found->second;
                setDeadline(connection, {});
                readiness_.remove(connection.socket.get(), id);
                connection.socket.reset();
                stats_.queuedBytes -= connection.queuedBytes;
                while (!connection.output.empty())
                {
                    popOutput(connection, false);
                }
                events_.disconnected(connection.peer, connection.closeReason, connection.closeError);
                connections_.erase(found);
            }
            closing_.clear();
        }

        void closeAll()
        {
            readiness_.remove(listener_.get(), kListener);
            listener_.reset();
            for (auto& entry : connections_)
            {
                closeSoon(entry.second, "server_shutdown");
            }
            reapClosed();
        }

        ServerEvents& events_;
        Readiness readiness_;
        Socket listener_;
        std::unordered_map<ConnectionId, Connection> connections_;
        std::set<std::pair<ServerClock::time_point, ConnectionId>> deadlines_;
        std::vector<ConnectionId> closing_;
        ServerTransportStats stats_;
        ServerClock::time_point resumeAccept_{};
        ConnectionId nextId_ = kStopSignal + 1;
    };
}

void runServerTransport(unsigned short port, ServerEvents& events)
{
    Reactor reactor(port, events);
    reactor.run();
}
