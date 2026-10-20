#pragma once

#ifdef _WIN32 // 如果是 Windows 平台

#include <winsock2.h>
#include <ws2tcpip.h>

using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
constexpr int kSocketError = SOCKET_ERROR;

#else // 如果是 Linux 平台

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>

using SocketHandle = int; // 定义统一类型
constexpr SocketHandle kInvalidSocket = -1; // 定义统一常量
constexpr int kSocketError = -1;

#endif

/*
    封装的目的是为了让 server 业务代码不关注底层平台细节

    存在平台差异的主要 API:

    1、
*/

// 包装函数
bool initializeSockets();

void cleanupSockets();

void closeSocket(SocketHandle socket);

int getSocketError();

// 统一发送入口。Linux 使用 MSG_NOSIGNAL，远端提前断开时让 send 返回错误，
// 而不是用 SIGPIPE 直接终止整个 server 进程。
int sendSocket(SocketHandle socket, const char* data, int length);
