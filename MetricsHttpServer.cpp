#include "MetricsHttpServer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <sstream>

namespace
{
    constexpr std::size_t kMaxRequestBytes = 4096;

    bool sendComplete(SocketHandle socket, const std::string& payload)
    {
        std::size_t sentTotal = 0;
        while (sentTotal < payload.size())
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
        return true;
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

    void setReceiveTimeout(SocketHandle socket)
    {
#ifdef _WIN32
        DWORD timeoutMilliseconds = 2000;
        setsockopt(
            socket,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeoutMilliseconds),
            sizeof(timeoutMilliseconds)
        );
#else
        timeval timeout{};
        timeout.tv_sec = 2;
        setsockopt(
            socket,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            sizeof(timeout)
        );
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
    const SocketHandle socketToClose = listenSocket_;
    if (socketToClose != kInvalidSocket)
    {
#ifdef _WIN32
        shutdown(socketToClose, SD_BOTH);
#else
        shutdown(socketToClose, SHUT_RDWR);
#endif
        closeSocket(socketToClose);
    }
    if (thread_.joinable())
    {
        thread_.join();
    }
    // worker 退出前保持句柄数值不变，避免 stop 与 serveLoop 并发读写。
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
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSocket_, &readSet);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000;
#ifdef _WIN32
        const int selected = select(0, &readSet, nullptr, nullptr, &timeout);
#else
        const int selected = select(
            listenSocket_ + 1,
            &readSet,
            nullptr,
            nullptr,
            &timeout
        );
#endif
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
    setReceiveTimeout(clientSocket);
    std::string request;
    std::array<char, 1024> buffer{};
    while (request.size() < kMaxRequestBytes &&
        request.find("\r\n\r\n") == std::string::npos)
    {
        const int received = recv(
            clientSocket,
            buffer.data(),
            static_cast<int>(buffer.size()),
            0
        );
        if (received <= 0)
        {
            return;
        }
        request.append(buffer.data(), static_cast<std::size_t>(received));
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
            )
        );
        return;
    }
    if (requestLine.starts_with("GET /healthz "))
    {
        sendComplete(
            clientSocket,
            makeResponse("200 OK", "text/plain; charset=utf-8", "ok\n")
        );
        return;
    }

    sendComplete(
        clientSocket,
        makeResponse("404 Not Found", "text/plain; charset=utf-8", "not found\n")
    );
}
