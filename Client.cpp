#include "Client.h"

#include <atomic>
#include <cctype>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <format>
#include <thread>
#include <chrono>
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
    constexpr auto kProgressReportInterval = std::chrono::seconds(1);
    using Clock = std::chrono::steady_clock;

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
            message.positionMilliseconds = static_cast<long long>(seconds) * 1000;
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

    bool connectToServer(SOCKET clientSocket, const std::string& serverIp)
    {
        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(kServerPort);

        int result = inet_pton(AF_INET, serverIp.c_str(), &serverAddr.sin_addr);
        if (result <= 0)
        {
            std::cout << "inet_pton failed for server ip: " << serverIp << "\n";
            return false;
        }

        std::cout << "Connecting to " << serverIp << ":" << kServerPort << "...\n";

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
        long long playerPositionMs = player.getPositionMilliseconds();
        localState.positionMilliseconds = playerPositionMs;
        localState.positionSeconds = static_cast<int>(playerPositionMs / 1000);

        std::cout << sourceLabel << " applied. " << syncStateToString(localState)
            << ", playerPositionMs=" << playerPositionMs << "\n";
        return true;
    }

    void handleLocalInput(
        SOCKET clientSocket,
        PlayerController& player,
        SyncState& localState,
        std::mutex& playerMutex,
        std::mutex& sendMutex,
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
                    << ", playerPositionMs=" << player.getPositionMilliseconds() << "\n";
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
            bool sent = false;
            {
                std::lock_guard<std::mutex> lock(sendMutex);
                sent = sendAll(clientSocket, tcpMessage);
            }

            if (!sent)
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
        std::mutex& sendMutex,
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

                    SyncMessage message = stringToMessage(line);
                    if (message.type == MessageType::Unknown)
                    {
                        std::cout << "ignored unknown broadcast message: " << line << "\n";
                        continue;
                    }

                    if (message.type == MessageType::Ping)
                    {
                        SyncMessage pong;
                        pong.type = MessageType::Pong;
                        pong.sequenceNumber = message.sequenceNumber;

                        // broadcast thread 和 progress report thread 会共享同一个 socket。
                        // 这里也必须使用 sendMutex，否则 "PONG 13\n" 和 "REPORT ...\n"
                        // 可能在 TCP 字节流中拼到一起，server 就会看到脏消息。
                        std::string tcpMessage = messageToString(pong);
                        bool sent = false;
                        {
                            std::lock_guard<std::mutex> lock(sendMutex);
                            sent = sendAll(clientSocket, tcpMessage);
                        }

                        if (!sent)
                        {
                            running = false;
                            shutdown(clientSocket, SD_BOTH);
                            break;
                        }

                        continue;
                    }

                    if (message.type == MessageType::Pong || message.type == MessageType::Report)
                    {
                        // 正常情况下 client 不会从 server 收到 PONG/REPORT。
                        // 如果未来协议扩展到双向状态同步，这里再单独处理。
                        continue;
                    }

                    std::cout << "received broadcast: " << line << "\n";
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

    // client 定期上报线程
    void reportPlaybackProgress(
        SOCKET clientSocket,
        PlayerController& player,
        SyncState& localState,
        std::mutex& playerMutex,
        std::mutex& sendMutex,
        std::atomic_bool& running
    )
    {
        SyncMessage message;
        message.type = MessageType::Report;

        while (running)
        {
            {
                std::lock_guard<std::mutex> guard(playerMutex);
                message.positionMilliseconds = player.getPositionMilliseconds();
                message.positionSeconds = static_cast<int>(message.positionMilliseconds / 1000);
                message.playbackState = localState.state;
            }

            std::string tcpMessage = messageToString(message);

            bool sent = false;
            {
                std::lock_guard<std::mutex> lock(sendMutex);
                sent = sendAll(clientSocket, tcpMessage);
            }

            if (!sent)
            {
                running = false;
                shutdown(clientSocket, SD_BOTH);
                break;
            }

            // 上报一次休息一秒
            std::this_thread::sleep_for(kProgressReportInterval);
        }
    }
}

void runClient(const std::string& mediaSource, const std::string& serverIp)
{
    // 记录播放器初始化的时长
    auto playInitStart = Clock::now();

    ActivePlayer player;

    auto playInitEnd = Clock::now();
    auto playInitMs = std::chrono::duration_cast<std::chrono::milliseconds>(playInitEnd - playInitStart).count();
    std::cout << std::format("[metric] player_init_ms={}\n", playInitMs);

    // 记录打开媒体的时长
    auto openMediaStart = Clock::now();

    if (!player.openMedia(mediaSource))
    {
        std::cout << "client failed to open media: " << mediaSource << "\n";
        return;
    }

    auto openMediaEnd = Clock::now();
    auto openMediaMs = std::chrono::duration_cast<std::chrono::milliseconds>(openMediaEnd - openMediaStart).count();
    std::cout << std::format("[metric] open_media_ms={} source={}\n", openMediaMs, mediaSource);

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

    // 记录连接服务器的时长
    auto connectServerStart = Clock::now();

    if (!connectToServer(clientSocket, serverIp))
    {
        closesocket(clientSocket);
        WSACleanup();
        return;
    }

    auto connectServerEnd = Clock::now();
    auto connectServerMs = std::chrono::duration_cast<std::chrono::milliseconds>(connectServerEnd - connectServerStart).count();
    std::cout << std::format("[metric] connect_server_ms={} server={}\n", connectServerMs, serverIp);

    SyncState localState;
    std::mutex playerMutex;
    std::mutex sendMutex;
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
        std::ref(sendMutex),
        std::ref(running)
    );

    std::thread broadcastThread(
        handleBroadcast,
        clientSocket,
        std::ref(player),
        std::ref(localState),
        std::ref(playerMutex),
        std::ref(sendMutex),
        std::ref(running)
    );

    std::thread progressReportThread(
        reportPlaybackProgress,
        clientSocket,
        std::ref(player),
        std::ref(localState),
        std::ref(playerMutex),
        std::ref(sendMutex),
        std::ref(running)
    );

    localThread.join();
    broadcastThread.join();
    progressReportThread.join();

    closesocket(clientSocket);
    WSACleanup();
}
