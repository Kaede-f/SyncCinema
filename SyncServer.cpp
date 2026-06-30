#include "SyncServer.h"

#include <iostream>
#include <string>
#include <thread>

#include "Protocol.h"
#include "NetSocket.h"
#include "Room.h"
#include "SyncMetrics.h"

namespace
{
    constexpr unsigned short kServerPort = 9000;

    void processLine(
        SocketHandle clientSocket,
        int clientId,
        Room& room,
        SyncMetricsCollector& metrics,
        const std::string& line)
    {
        if (line.empty())
        {
            return;
        }

        SyncMessage message = stringToMessage(line);
        if (message.type == MessageType::Unknown)
        {
            std::cout << "server ignored unknown message: " << line << "\n";
            return;
        }
        else if (message.type == MessageType::Report)
        {
            SyncState roomState = room.getState();
            SyncMetricSample sample;
            sample.clientId = clientId;
            sample.clientPositionMs = message.positionMilliseconds;
            sample.roomPositionMs = roomState.positionMilliseconds;
            sample.clientState = message.playbackState;
            sample.roomState = roomState.state;

            metrics.recordProgressReport(sample);
            return;
        }
        else
        {
            std::cout << "server received control: " << line << "\n";

            // server 不再控制本地播放器。
            // 新架构里 server 是“房间协调者”：更新房间状态，然后把控制命令广播给其他 client。
            room.broadcastControlMessage(clientSocket, message);
        }

    }

    void handleClient(
        SocketHandle clientSocket,
        int clientId,
        Room& room,
        SyncMetricsCollector& metrics)
    {
        std::string receiveBuffer;
        char buffer[512]{};

        while (true)
        {
            int bytesReceived = recv(clientSocket, buffer, sizeof(buffer), 0);

            if (bytesReceived > 0)
            {
                receiveBuffer.append(buffer, bytesReceived);

                // TCP 是字节流，没有天然消息边界。
                // 因此仍然沿用项目协议：每条消息以 '\n' 结尾，server 按行拆分。
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

                    processLine(clientSocket, clientId, room, metrics, line);
                }
            }
            else if (bytesReceived == 0)
            {
                std::cout << "client disconnected\n";
                break;
            }
            else
            {
                std::cout << "recv failed: " << getSocketError() << "\n";
                break;
            }
        }

        room.removeClient(clientSocket);
        metrics.removeClient(clientId);
        closeSocket(clientSocket);
        std::cout << "client removed. online clients: " << room.getClientCount() << "\n";
    }
}

void runSyncServer()
{
    if (!initializeSockets()) return;

    SocketHandle listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == kInvalidSocket)
    {
        std::cout << "socket failed: " << getSocketError() << "\n";
        cleanupSockets();
        return;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(kServerPort);

    int result = bind(
        listenSocket,
        reinterpret_cast<sockaddr*>(&serverAddr),
        sizeof(serverAddr)
    );

    if (result == kSocketError)
    {
        std::cout << "bind failed: " << getSocketError() << "\n";
        closeSocket(listenSocket);
        cleanupSockets();
        return;
    }

    result = listen(listenSocket, SOMAXCONN);
    if (result == kSocketError)
    {
        std::cout << "listen failed: " << getSocketError() << "\n";
        closeSocket(listenSocket);
        cleanupSockets();
        return;
    }

    Room room;
    SyncMetricsCollector metrics;

    std::cout << "SyncServer listening on port " << kServerPort << "...\n";
    std::cout << "Server role: accept clients and broadcast playback commands.\n";
    std::cout << "Press Ctrl+C to stop the server.\n";

    while (true)
    {
        std::cout << "Waiting for client...\n";

        SocketHandle clientSocket = accept(listenSocket, nullptr, nullptr);
        if (clientSocket == kInvalidSocket)
        {
            std::cout << "accept failed: " << getSocketError() << "\n";
            break;
        }

        int clientId = room.addClient(clientSocket);
        std::cout << "client #" << clientId
            << " connected. online clients: " << room.getClientCount() << "\n";

        // 一个 client 一个线程：每个线程只负责读自己的 clientSocket。
        // room 通过 mutex 保护共享的 client 列表和播放状态。
        std::thread clientThread(
            handleClient,
            clientSocket,
            clientId,
            std::ref(room),
            std::ref(metrics)
        );
        clientThread.detach();
    }

    closeSocket(listenSocket);
    cleanupSockets();
}

