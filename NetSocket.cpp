#include "NetSocket.h"

#include <cstddef>

#ifdef _WIN32 // 如果是 Windows 平台

bool initializeSockets()
{
    WSADATA wsaData;

    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) return false;

    return true;
    
}

void cleanupSockets()
{
    WSACleanup();
}

void closeSocket(SocketHandle socket)
{
    closesocket(socket);
}

int getSocketError()
{
    return WSAGetLastError();
}

int sendSocket(SocketHandle socket, const char* data, int length)
{
    return send(socket, data, length, 0);
}

#else // 如果是 Linux 平台

bool initializeSockets()
{
    return true;
}

void cleanupSockets()
{

}

void closeSocket(SocketHandle socket)
{
    close(socket);
}

int getSocketError()
{
    return errno;
}

int sendSocket(SocketHandle socket, const char* data, int length)
{
#ifdef MSG_NOSIGNAL
    return send(socket, data, static_cast<std::size_t>(length), MSG_NOSIGNAL);
#else
    return send(socket, data, static_cast<std::size_t>(length), 0);
#endif
}

#endif
