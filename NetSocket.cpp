#include "NetSocket.h"

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

#endif
