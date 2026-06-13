#include "Client.h"

#include <atomic>
#include <cctype>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
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
    constexpr const char* kServerIp = "127.0.0.1";
    constexpr unsigned short kServerPort = 9000;

    void printClientHelp()
    {
        std::cout << "Available commands:\n";
        std::cout << "  play             play local video and broadcast PLAY\n";
        std::cout << "  pause            pause local video and broadcast PAUSE\n";
        std::cout << "  seek <seconds>   seek local video and broadcast SEEK, for example: seek 120\n";
        std::cout << "  status           print local sync state and player position\n";
        std::cout << "  help             print this help\n";
        std::cout << "  quit             disconnect and exit client\n";
    }

    std::string toLowerAscii(std::string text)
    {
        for (char& ch : text)
        {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }

        return text;
    }

    bool buildSyncMessageFromInput(const std::string& line, SyncMessage& message, std::string& errorMessage)
    {
        std::istringstream iss(line);
        std::string command;
        iss >> command;
        command = toLowerAscii(command);

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
                std::cout << "send failed: no bytes sent\n";
                return false;
            }

            totalSent += bytesSent;
        }

        return true;
    }

    bool connectToServer(SOCKET clientSocket)
    {
        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(kServerPort);

        int result = inet_pton(AF_INET, kServerIp, &serverAddr.sin_addr);
        if (result <= 0)
        {
            std::cout << "inet_pton failed for server ip: " << kServerIp << "\n";
            return false;
        }

        std::cout << "Connecting to " << kServerIp << ":" << kServerPort << "...\n";

        result = connect(
            clientSocket,
            reinterpret_cast<sockaddr*>(&serverAddr),
            sizeof(serverAddr)
        );

        if (result == SOCKET_ERROR)
        {
            std::cout << "connect failed: " << WSAGetLastError() << "\n";
            return false;
        }

        std::cout << "Connected to server.\n";
        return true;
    }

    bool applyControlMessage(
        const SyncMessage& message,
        PlayerController& player,
        SyncState& localState,
        std::mutex& playerMutex,
        const std::string& sourceLabel)
    {
        // local thread 和 broadcast thread 都会控制同一个播放器。
        // mutex 保证同一时间只有一个线程进入播放器控制逻辑，避免 play/seek/pause 同时打架。
        std::lock_guard<std::mutex> lock(playerMutex);

        if (!applyMessageToPlayer(message, player))
        {
            std::cout << sourceLabel << " player command failed\n";
            return false;
        }

        applyMessageToState(message, localState);
        std::cout << sourceLabel << " applied. " << syncStateToString(localState)
            << ", playerPosition=" << player.getPositionSeconds() << " seconds\n";
        return true;
    }

    void handleLocalInput(
        SOCKET clientSocket,
        PlayerController& player,
        SyncState& localState,
        std::mutex& playerMutex,
        std::atomic_bool& running)
    {
        std::string line;
        printClientHelp();

        while (running)
        {
            std::cout << "> ";

            if (!std::getline(std::cin, line))
            {
                running = false;
                shutdown(clientSocket, SD_BOTH);
                break;
            }

            std::istringstream commandStream(line);
            std::string command;
            commandStream >> command;
            command = toLowerAscii(command);

            if (command.empty())
            {
                continue;
            }

            if (command == "help")
            {
                printClientHelp();
                continue;
            }

            if (command == "status")
            {
                std::lock_guard<std::mutex> lock(playerMutex);
                std::cout << "local status: " << syncStateToString(localState)
                    << ", playerPosition=" << player.getPositionSeconds() << " seconds\n";
                continue;
            }

            if (command == "quit")
            {
                running = false;
                shutdown(clientSocket, SD_BOTH);
                break;
            }

            SyncMessage message;
            std::string errorMessage;
            if (!buildSyncMessageFromInput(line, message, errorMessage))
            {
                std::cout << errorMessage << "\n";
                continue;
            }

            // 发送方先控制自己的本地播放器，再把同一条 SyncMessage 发给 server。
            // server 会广播给其他 client，但不会再发回给发送方，避免重复执行。
            if (!applyControlMessage(message, player, localState, playerMutex, "local"))
            {
                continue;
            }

            std::string tcpMessage = messageToString(message);
            if (!sendAll(clientSocket, tcpMessage))
            {
                running = false;
                shutdown(clientSocket, SD_BOTH);
                break;
            }

            std::cout << "sent: " << tcpMessage;
        }
    }

    void handleBroadcast(
        SOCKET clientSocket,
        PlayerController& player,
        SyncState& localState,
        std::mutex& playerMutex,
        std::atomic_bool& running)
    {
        std::string receiveBuffer;
        char buffer[512]{};

        while (running)
        {
            int bytesReceived = recv(clientSocket, buffer, sizeof(buffer), 0);

            if (bytesReceived > 0)
            {
                receiveBuffer.append(buffer, bytesReceived);

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

                    if (line.empty())
                    {
                        continue;
                    }

                    std::cout << "received broadcast: " << line << "\n";

                    SyncMessage message = stringToMessage(line);
                    if (message.type == MessageType::Unknown)
                    {
                        std::cout << "ignored unknown broadcast message\n";
                        continue;
                    }

                    applyControlMessage(message, player, localState, playerMutex, "remote");
                }
            }
            else if (bytesReceived == 0)
            {
                std::cout << "server disconnected\n";
                running = false;
                break;
            }
            else
            {
                int error = WSAGetLastError();
                if (running)
                {
                    std::cout << "recv failed: " << error << "\n";
                }
                running = false;
                break;
            }
        }
    }
}

void runClient(const std::string& videoPath)
{
    ActivePlayer player;
    if (!player.openMedia(videoPath))
    {
        std::cout << "client failed to open media: " << videoPath << "\n";
        return;
    }

    WSADATA wsaData;

    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0)
    {
        std::cout << "WSAStartup failed: " << result << "\n";
        return;
    }

    SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (clientSocket == INVALID_SOCKET)
    {
        std::cout << "socket failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return;
    }

    if (!connectToServer(clientSocket))
    {
        closesocket(clientSocket);
        WSACleanup();
        return;
    }

    SyncState localState;
    std::mutex playerMutex;
    std::atomic_bool running{ true };

    // 注意这里使用 std::ref。
    // std::thread 默认会复制参数；播放器不能复制成两份，否则本地输入线程和广播线程控制的就不是同一个对象。
    // std::ref(player) 明确告诉 C++：把同一个 player 对象按引用传给线程函数。
    std::thread localThread(
        handleLocalInput,
        clientSocket,
        std::ref(player),
        std::ref(localState),
        std::ref(playerMutex),
        std::ref(running)
    );

    std::thread broadcastThread(
        handleBroadcast,
        clientSocket,
        std::ref(player),
        std::ref(localState),
        std::ref(playerMutex),
        std::ref(running)
    );

    localThread.join();
    broadcastThread.join();

    closesocket(clientSocket);
    WSACleanup();
}
