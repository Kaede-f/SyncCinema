#include "TcpClient.h"

#include <iostream>
#include <sstream>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "PlayerController.h"
#include "Protocol.h"

#ifdef USE_LIBVLC   // F: 好语法
#include "LibVlcPlayer.h"
using ActivePlayer = LibVlcPlayer;
#else
#include "MockPlayer.h"
using ActivePlayer = ConsoleMockPlayer; // F: 好语法
#endif

namespace
{
    constexpr const char* kServerIp = "127.0.0.1";
    constexpr unsigned short kServerPort = 9000;

    void printClientHelp()
    {
        std::cout << "Available commands:\n";
        std::cout << "  play             play local video and send PLAY to server\n";
        std::cout << "  pause            pause local video and send PAUSE to server\n";
        std::cout << "  seek <seconds>   seek local video and send SEEK, for example: seek 120\n";
        std::cout << "  status           print this client's latest known state\n";
        std::cout << "  help             print this help\n";
        std::cout << "  quit             disconnect and exit client\n";
    }

    void printLocalStatus(const SyncState& state, const PlayerController& player)
    {
        std::cout << "client local status: " << syncStateToString(state)
            << ", playerPosition=" << player.getPositionSeconds() << " seconds\n";
    }

    std::string removeTrailingLineEnd(std::string text)
    {
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        {
            text.pop_back();
        }

        return text;
    }

    bool buildSyncMessageFromInput(const std::string& line, SyncMessage& message, std::string& errorMessage)
    {
        std::istringstream iss(line);
        std::string command;
        iss >> command;

        if (command == "play")
        {
            std::string extra;
            if (iss >> extra)
            {
                errorMessage = "play does not need extra arguments.";
                return false;
            }

            message.type = MessageType::Play;
            message.positionSeconds = 0;
            return true;
        }

        if (command == "pause")
        {
            std::string extra;
            if (iss >> extra)
            {
                errorMessage = "pause does not need extra arguments.";
                return false;
            }

            message.type = MessageType::Pause;
            message.positionSeconds = 0;
            return true;
        }

        if (command == "seek")
        {
            int seconds = 0;
            std::string extra;

            if (!(iss >> seconds) || seconds < 0 || (iss >> extra))
            {
                errorMessage = "seek usage: seek <non-negative seconds>, for example: seek 120";
                return false;
            }

            message.type = MessageType::Seek;
            message.positionSeconds = seconds;
            return true;
        }

        errorMessage = "unknown sync command.";
        return false;
    }

    bool sendAll(SOCKET socket, const std::string& data)
    {
        int totalSent = 0;
        const int totalSize = static_cast<int>(data.size());

        while (totalSent < totalSize)
        {
            // send 把内存中的字节写入 TCP 连接。
            // TCP 是字节流，send 不保证一次就把所有字节都发完，所以这里循环发送剩余部分。
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

    bool receiveLine(SOCKET socket, std::string& receiveBuffer, std::string& line)
    {
        while (true)
        {
            std::size_t lineEnd = receiveBuffer.find('\n');
            if (lineEnd != std::string::npos)
            {
                line = receiveBuffer.substr(0, lineEnd);
                receiveBuffer.erase(0, lineEnd + 1);

                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }

                return true;
            }

            char buffer[512]{};

            // recv 从 TCP 连接读取字节。
            // 返回值 > 0 表示读到了数据；== 0 表示 server 正常断开；SOCKET_ERROR 表示出错。
            int bytesReceived = recv(socket, buffer, sizeof(buffer), 0);

            if (bytesReceived > 0)
            {
                receiveBuffer.append(buffer, bytesReceived);
            }
            else if (bytesReceived == 0)
            {
                std::cout << "server disconnected.\n";
                return false;
            }
            else
            {
                std::cout << "recv failed: " << WSAGetLastError() << "\n";
                return false;
            }
        }
    }
}

void runTcpClient(const std::string& videoPath)
{
    ActivePlayer player;
    if (!player.openMedia(videoPath))
    {
        std::cout << "client failed to open media: " << videoPath << "\n";
        return;
    }

    WSADATA wsaData;

    // WSAStartup 用来初始化 Winsock。
    // Windows 上使用 socket/connect/send/recv 前，必须先让系统准备好 Winsock 库。
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0)
    {
        std::cout << "WSAStartup failed: " << result << "\n";
        return;
    }

    // socket 创建一个 TCP 客户端套接字。
    // AF_INET 表示 IPv4，SOCK_STREAM 表示 TCP 这种可靠字节流协议。
    SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (clientSocket == INVALID_SOCKET)
    {
        std::cout << "socket failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(kServerPort);

    result = inet_pton(AF_INET, kServerIp, &serverAddr.sin_addr);
    if (result <= 0)
    {
        std::cout << "inet_pton failed for server ip: " << kServerIp << "\n";
        closesocket(clientSocket);
        WSACleanup();
        return;
    }

    std::cout << "Connecting to " << kServerIp << ":" << kServerPort << "...\n";

    // connect 主动连接 server 的 IP 和端口。
    // 连接成功后，clientSocket 就可以用于 send 和 recv。
    result = connect(
        clientSocket,
        reinterpret_cast<sockaddr*>(&serverAddr),
        sizeof(serverAddr)
    );

    if (result == SOCKET_ERROR)
    {
        std::cout << "connect failed: " << WSAGetLastError() << "\n";
        closesocket(clientSocket);
        WSACleanup();
        return;
    }

    std::cout << "Connected to server.\n";
    printClientHelp();

    SyncState localState;
    std::string receiveBuffer;
    std::string line;

    while (true)
    {
        std::cout << "> ";

        if (!std::getline(std::cin, line))
        {
            std::cout << "\ninput closed, disconnecting.\n";
            break;
        }

        std::istringstream commandStream(line);
        std::string command;
        commandStream >> command;

        if (command.empty())
        {
            continue;
        }

        // play/pause/seek 是需要同步的播放控制命令：本地播放器执行一次，也发送给 server 执行一次。
        // status/help/quit 只是当前命令行程序的本地命令，不应该改变对端播放器状态。
        if (command == "status")
        {
            printLocalStatus(localState, player);
            continue;
        }

        if (command == "help")
        {
            printClientHelp();
            continue;
        }

        if (command == "quit")
        {
            std::cout << "disconnecting.\n";
            break;
        }

        if (command != "play" && command != "pause" && command != "seek")
        {
            std::cout << "unknown command: " << command << "\n";
            printClientHelp();
            continue;
        }

        SyncMessage message;
        std::string errorMessage;
        if (!buildSyncMessageFromInput(line, message, errorMessage))
        {
            std::cout << errorMessage << "\n";
            continue;
        }

        // 真实同步播放 MVP 的关键点：client 本地先执行播放器控制，再把同样的 SyncMessage 发给 server。
        if (!applyMessageToPlayer(message, player))
        {
            std::cout << "local player command failed, message not sent.\n";
            continue;
        }

        applyMessageToState(message, localState);

        std::string tcpMessage = messageToString(message);

        // 每条 TCP 消息以 '\n' 结尾。
        // 因为 TCP 没有消息包边界，接收方必须靠约定好的分隔符把字节流拆回一条条命令。
        if (!sendAll(clientSocket, tcpMessage))
        {
            break;
        }

        std::cout << "sent: " << removeTrailingLineEnd(tcpMessage) << "\n";

        std::string serverResponse;
        if (!receiveLine(clientSocket, receiveBuffer, serverResponse))
        {
            break;
        }

        std::cout << "server response: " << serverResponse << "\n";
    }

    closesocket(clientSocket);

    // WSACleanup 与 WSAStartup 配对，表示当前程序不再使用 Winsock。
    WSACleanup();
}
