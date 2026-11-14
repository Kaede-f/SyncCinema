#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "NetSocket.h"
#include "PrometheusMetrics.h"

// 轻量级、只读的本机监控端点。默认由调用方绑定到 127.0.0.1，供同机
// Prometheus 抓取；它不参与 9000 端口上的业务协议。
class MetricsHttpServer
{
public:
    explicit MetricsHttpServer(PrometheusMetrics& metrics);
    ~MetricsHttpServer();

    MetricsHttpServer(const MetricsHttpServer&) = delete;
    MetricsHttpServer& operator=(const MetricsHttpServer&) = delete;

    bool start(unsigned short requestedPort, std::string& errorMessage);
    void stop();

    bool isRunning() const;
    unsigned short port() const;

private:
    void serveLoop();
    void handleClient(SocketHandle clientSocket);

    PrometheusMetrics& metrics_;
    std::atomic_bool running_{ false };
    SocketHandle listenSocket_ = kInvalidSocket;
    unsigned short port_ = 0;
    std::thread thread_;
};
