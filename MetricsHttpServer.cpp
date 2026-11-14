#include "MetricsHttpServer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <sstream>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#endif

namespace
{
    constexpr std::size_t kMaxRequestBytes = 4096;
    constexpr auto kRequestDeadline = std::chrono::seconds(2);
    constexpr auto kSocketTimeout = std::chrono::milliseconds(250);
    using Clock = std::chrono::steady_clock;

    bool sendComplete(SocketHandle socket, const std::string& payload, const std::atomic_bool& running)
    {
        std::size_t sentTotal = 0;
        const auto deadline = Clock::now() + kRequestDeadline;
        while (sentTotal < payload.size() && running && Clock::now() < deadline)
        {
            const std::size_t remaining = payload.size() - sentTotal;
            const int chunkSize = static_cast<int>((std::min)(
                remaining,
                static_cast<std::size_t>(0x7fffffff)
            ));
            const int sent = sendSocket(
                socket,
                payload.data() + sentTotal,
                chunkSize
            );
            if (sent <= 0)
            {
                return false;
            }
            sentTotal += static_cast<std::size_t>(sent);
        }
        return sentTotal == payload.size();
    }

    std::string makeResponse(
        const char* status,
        const char* contentType,
        const std::string& body)
    {
        std::ostringstream response;
        response << "HTTP/1.1 " << status << "\r\n"
            << "Content-Type: " << contentType << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n"
            << "Cache-Control: no-store\r\n\r\n"
            << body;
        return response.str();
    }

    bool setSocketTimeouts(SocketHandle socket)
    {
#ifdef _WIN32
        const auto value = static_cast<DWORD>(kSocketTimeout.count());
        const auto* timeout = reinterpret_cast<const char*>(&value);
#else
        const timeval value{0, kSocketTimeout.count() * 1000};
        const auto* timeout = &value;
#endif
        return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, timeout, sizeof(value)) == 0 &&
            setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, timeout, sizeof(value)) == 0;
    }

    int waitForConnection(SocketHandle socket)
    {
#ifdef _WIN32
        WSAPOLLFD fd{socket, POLLRDNORM, 0};
        return WSAPoll(&fd, 1, static_cast<int>(kSocketTimeout.count()));
#else
        pollfd fd{socket, POLLIN, 0};
        return poll(&fd, 1, static_cast<int>(kSocketTimeout.count()));
#endif
    }

    bool setListenerNonblocking(SocketHandle socket)
    {
#ifdef _WIN32
        u_long enabled = 1;
        return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
        const int flags = fcntl(socket, F_GETFL);
        return flags != -1 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    }
}

MetricsHttpServer::MetricsHttpServer(PrometheusMetrics& metrics)
    : metrics_(metrics)
{
}

MetricsHttpServer::~MetricsHttpServer()
{
    stop();
}

bool MetricsHttpServer::start(
    unsigned short requestedPort,
    std::string& errorMessage)
{
    if (running_)
    {
        errorMessage = "metrics HTTP server is already running";
        return false;
    }

    SocketHandle candidate = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (candidate == kInvalidSocket)
    {
        errorMessage = "metrics socket failed: " +
            std::to_string(getSocketError());
        return false;
    }
    if (!setListenerNonblocking(candidate))
    {
        errorMessage = "metrics nonblocking listener failed: " +
            std::to_string(getSocketError());
        closeSocket(candidate);
        return false;
    }

    int reuseAddress = 1;
    setsockopt(
        candidate,
        SOL_SOCKET,
        SO_REUSEADDR,
#ifdef _WIN32
        reinterpret_cast<const char*>(&reuseAddress),
#else
        &reuseAddress,
#endif
        sizeof(reuseAddress)
    );

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(requestedPort);
    if (bind(
            candidate,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)) == kSocketError)
    {
        errorMessage = "metrics bind failed: " +
            std::to_string(getSocketError());
        closeSocket(candidate);
        return false;
    }

    if (listen(candidate, 8) == kSocketError)
    {
        errorMessage = "metrics listen failed: " +
            std::to_string(getSocketError());
        closeSocket(candidate);
        return false;
    }

    sockaddr_in boundAddress{};
#ifdef _WIN32
    int boundAddressLength = sizeof(boundAddress);
#else
    socklen_t boundAddressLength = sizeof(boundAddress);
#endif
    if (getsockname(
            candidate,
            reinterpret_cast<sockaddr*>(&boundAddress),
            &boundAddressLength) == kSocketError)
    {
        errorMessage = "metrics getsockname failed: " +
            std::to_string(getSocketError());
        closeSocket(candidate);
        return false;
    }

    listenSocket_ = candidate;
    port_ = ntohs(boundAddress.sin_port);
    running_ = true;
    try
    {
        thread_ = std::thread(&MetricsHttpServer::serveLoop, this);
    }
    catch (...)
    {
        running_ = false;
        closeSocket(listenSocket_);
        listenSocket_ = kInvalidSocket;
        port_ = 0;
        errorMessage = "metrics worker thread could not be started";
        return false;
    }
    return true;
}

void MetricsHttpServer::stop()
{
    running_ = false;
    // The worker owns all listener operations until it has stopped using the fd.
    if (thread_.joinable())
    {
        thread_.join();
    }
    if (listenSocket_ != kInvalidSocket)
    {
        closeSocket(listenSocket_);
    }
    listenSocket_ = kInvalidSocket;
    port_ = 0;
}

bool MetricsHttpServer::isRunning() const
{
    return running_;
}

unsigned short MetricsHttpServer::port() const
{
    return port_;
}

void MetricsHttpServer::serveLoop()
{
    while (running_)
    {
        const int selected = waitForConnection(listenSocket_);
        if (!running_)
        {
            break;
        }
        if (selected <= 0)
        {
            continue;
        }

        SocketHandle client = accept(listenSocket_, nullptr, nullptr);
        if (client == kInvalidSocket)
        {
            continue;
        }
        handleClient(client);
        closeSocket(client);
    }
}

void MetricsHttpServer::handleClient(SocketHandle clientSocket)
{
#ifdef _WIN32
    // accept inherits the listener's nonblocking mode on Winsock.
    u_long nonblocking = 0;
    if (ioctlsocket(clientSocket, FIONBIO, &nonblocking) != 0)
    {
        return;
    }
#endif
    if (!setSocketTimeouts(clientSocket))
    {
        return;
    }
    const auto deadline = Clock::now() + kRequestDeadline;
    std::string request;
    std::array<char, 1024> buffer{};
    while (running_ && Clock::now() < deadline && request.size() < kMaxRequestBytes)
    {
        const int received = recv(
            clientSocket,
            buffer.data(),
            static_cast<int>((std::min)(buffer.size(), kMaxRequestBytes - request.size())),
            0
        );
        if (received <= 0)
        {
            return;
        }
        request.append(buffer.data(), static_cast<std::size_t>(received));
        if (request.find("\r\n\r\n") != std::string::npos)
        {
            break;
        }
    }
    if (!running_ || request.find("\r\n\r\n") == std::string::npos)
    {
        return;
    }

    const std::size_t lineEnd = request.find("\r\n");
    const std::string requestLine = request.substr(0, lineEnd);
    if (requestLine.starts_with("GET /metrics "))
    {
        sendComplete(
            clientSocket,
            makeResponse(
                "200 OK",
                "text/plain; version=0.0.4; charset=utf-8",
                metrics_.render()
            ),
            running_
        );
        return;
    }
    if (requestLine.starts_with("GET /healthz "))
    {
        sendComplete(
            clientSocket,
            makeResponse("200 OK", "text/plain; charset=utf-8", "ok\n"),
            running_
        );
        return;
    }

    sendComplete(
        clientSocket,
        makeResponse("404 Not Found", "text/plain; charset=utf-8", "not found\n"),
        running_
    );
}
