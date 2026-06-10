#include "TcpServer.h"

#include <iostream>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "PlayerController.h"
#include "Protocol.h"

#ifdef USE_LIBVLC
#include "LibVlcPlayer.h"
using ActivePlayer = LibVlcPlayer;
#else
#include "MockPlayer.h"
using ActivePlayer = ConsoleMockPlayer;
#endif

namespace
{
    constexpr unsigned short kServerPort = 9000;

    bool sendAll(SOCKET socket, const std::string& data)
    {
        int totalSent = 0;
        const int totalSize = static_cast<int>(data.size());

        while (totalSent < totalSize)
        {
            // send 把响应写回 TCP 连接。
            // TCP 发送的是字节流，一次 send 不保证把 data 全部发完，所以用循环补发剩余字节。
            int bytesSent = send(
                socket,
                data.c_str() + totalSent,
                totalSize - totalSent,
                0
            );

            if (bytesSent == SOCKET_ERROR)
            {
                std::cout << "send failed: " << WSAGetLastError() << "\n";
                return false;
            }

            if (bytesSent == 0)
            {
                std::cout << "send failed: connection closed while sending.\n";
                return false;
            }

            totalSent += bytesSent;
        }

        return true;
    }

    void printServerState(const SyncState& state)
    {
        std::cout << "server status: " << syncStateToString(state) << "\n";
    }

    bool processOneLine(
        SOCKET clientSocket,
        const std::string& line,
        SyncState& serverState,
        PlayerController& player)
    {
        if (line.empty())
        {
            return true;
        }

        std::cout << "received raw: " << line << "\n";

        SyncMessage message = stringToMessage(line);
        if (message.type == MessageType::Unknown)
        {
            std::cout << "message parse failed.\n";
            return sendAll(clientSocket, "ERROR Unknown message\n");
        }

        std::cout << "parsed message: " << messageTypeToString(message.type);
        if (message.type == MessageType::Seek)
        {
            std::cout << " " << message.positionSeconds;
        }
        std::cout << "\n";

        // 播放器控制和状态更新放在一起，但不把 libVLC 或 MockPlayer 的细节写进 TCP 逻辑。
        // applyMessageToPlayer 负责“命令如何控制播放器”，applyMessageToState 负责“命令如何改变同步状态”。
        if (!applyMessageToPlayer(message, player))
        {
            std::cout << "player command failed.\n";
            return sendAll(clientSocket, "ERROR Player command failed\n");
        }

        applyMessageToState(message, serverState);
        printServerState(serverState);

        // server 返回当前状态，client 收到后打印。后续多客户端版本可以把这里扩展成广播。
        return sendAll(clientSocket, stateResponseToString(serverState));
    }

    void handleClient(SOCKET clientSocket, SyncState& serverState, PlayerController& player)
    {
        std::string receiveBuffer;
        char buffer[512]{};

        while (true)
        {
            // recv 从 clientSocket 读取 TCP 字节流。
            // 返回值 > 0 表示读到字节；== 0 表示对方正常断开；SOCKET_ERROR 表示网络错误。
            int bytesReceived = recv(clientSocket, buffer, sizeof(buffer), 0);

            if (bytesReceived > 0)
            {
                receiveBuffer.append(buffer, bytesReceived);

                // TCP 没有天然的“消息边界”。
                // 用 '\n' 做边界后，哪怕一次 recv 收到多条消息，或者只收到半条消息，也能稳定拆分。
                while (true)
                {
                    std::size_t lineEnd = receiveBuffer.find('\n');
                    if (lineEnd == std::string::npos)
                    {
                        break;
                    }

                    std::string line = receiveBuffer.substr(0, lineEnd);
                    receiveBuffer.erase(0, lineEnd + 1);

                    if (!line.empty() && line.back() == '\r')
                    {
                        line.pop_back();
                    }

                    if (!processOneLine(clientSocket, line, serverState, player))
                    {
                        return;
                    }
                }
            }
            else if (bytesReceived == 0)
            {
                std::cout << "client disconnected.\n";
                return;
            }
            else
            {
                std::cout << "recv failed: " << WSAGetLastError() << "\n";
                return;
            }
        }
    }
}

void runTcpServer(const std::string& videoPath)
{
    ActivePlayer player;
    if (!player.openMedia(videoPath))
    {
        std::cout << "server failed to open media: " << videoPath << "\n";
        return;
    }

    WSADATA wsaData;

    // WSAStartup 初始化 Winsock。
    // Windows 上的 socket/bind/listen/accept/send/recv 都属于 Winsock，使用前必须初始化。
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0)
    {
        std::cout << "WSAStartup failed: " << result << "\n";
        return;
    }

    // listenSocket 是监听套接字，只负责绑定端口、监听连接、accept 新连接。
    // 真正与某个 client 收发数据的是 accept 返回的 clientSocket。
    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET)
    {
        std::cout << "socket failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(kServerPort);

    // bind 把 listenSocket 绑定到本机 9000 端口。
    result = bind(
        listenSocket,
        reinterpret_cast<sockaddr*>(&serverAddr),
        sizeof(serverAddr)
    );

    if (result == SOCKET_ERROR)
    {
        std::cout << "bind failed: " << WSAGetLastError() << "\n";
        closesocket(listenSocket);
        WSACleanup();
        return;
    }

    // listen 让 socket 进入被动等待连接的状态。
    result = listen(listenSocket, SOMAXCONN);
    if (result == SOCKET_ERROR)
    {
        std::cout << "listen failed: " << WSAGetLastError() << "\n";
        closesocket(listenSocket);
        WSACleanup();
        return;
    }

    std::cout << "TCP server listening on port " << kServerPort << "...\n";
    std::cout << "Press Ctrl+C to stop the server.\n";

    SyncState serverState;

    while (true)
    {
        std::cout << "Waiting for client...\n";

        // accept 阻塞等待一个 client 连接。
        // 成功后得到 clientSocket，后续 recv/send 都使用 clientSocket。
        SOCKET clientSocket = accept(listenSocket, nullptr, nullptr);
        if (clientSocket == INVALID_SOCKET)
        {
            std::cout << "accept failed: " << WSAGetLastError() << "\n";
            break;
        }

        std::cout << "Client connected.\n";
        printServerState(serverState);

        handleClient(clientSocket, serverState, player);
        closesocket(clientSocket);
    }

    closesocket(listenSocket);

    // WSACleanup 和 WSAStartup 配对，表示程序不再使用 Winsock。
    WSACleanup();
}
