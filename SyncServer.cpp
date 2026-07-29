#include "SyncServer.h"

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>

#include "Protocol.h"
#include "NetSocket.h"
#include "Room.h"
#include "SyncMetrics.h"

namespace
{
    constexpr unsigned short kServerPort = 9000;
    constexpr auto kHeartbeatInterval = std::chrono::milliseconds(1000);

    using Clock = std::chrono::steady_clock;

    SyncMessage makeSnapshotMessage(const RoomSnapshot& snapshot)
    {
        SyncMessage message;
        message.type = MessageType::Snapshot;
        message.controlEpoch = snapshot.controlEpoch;
        message.playbackState = snapshot.state.state;
        message.positionSeconds = snapshot.state.positionSeconds;
        message.positionMilliseconds = snapshot.state.positionMilliseconds;
        return message;
    }

    void processLine(
        SocketHandle clientSocket,
        int clientId,
        Room& room,
        SyncMetricsCollector& metrics,
        std::mutex& controlCommandMutex,
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

        if (message.type == MessageType::Pong)
        {
            metrics.recordPongReceived(clientId, message.sequenceNumber, Clock::now());
            return;
        }

        if (message.type == MessageType::Ping)
        {
            // 当前协议中 PING 只由 server 主动发出，client 只需要回 PONG。
            // 如果这里收到 PING，说明对端发来了当前 server 不支持的方向，直接忽略。
            return;
        }

        if (message.type == MessageType::Snapshot)
        {
            // SNAPSHOT 是 server -> client 的单向消息。
            // client 伪造快照不能改变房间权威状态。
            std::cout << "server ignored client snapshot message\n";
            return;
        }

        if (message.type == MessageType::Report)
        {
            // 控制命令会同时修改 Room 状态并开启新的 metrics epoch。
            // REPORT 也使用同一把锁读取这两个对象，避免采到“新 Room + 旧 epoch”
            // 或“旧 Room + 新 epoch”这种不一致快照。
            std::lock_guard<std::mutex> controlLock(controlCommandMutex);

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

        std::cout << "server received control: " << line << "\n";

        // server 不再控制本地播放器。
        // 新架构里 server 是“房间协调者”：更新房间状态，然后把控制命令广播给其他 client。
        //
        // 多个 client 可能同时发控制命令。这把锁保证 Room 应用顺序、广播顺序和
        // metrics epoch 顺序完全一致，后续同步算法才有稳定的“先后”语义。
        std::lock_guard<std::mutex> controlLock(controlCommandMutex);
        room.broadcastControlMessage(clientSocket, message);
        RoomSnapshot snapshot = room.getSnapshot();
        metrics.beginControlEpoch(snapshot.controlEpoch, message);
    }

    void handleClient(
        SocketHandle clientSocket,
        int clientId,
        Room& room,
        SyncMetricsCollector& metrics,
        std::mutex& controlCommandMutex)
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

                    processLine(
                        clientSocket,
                        clientId,
                        room,
                        metrics,
                        controlCommandMutex,
                        line
                    );
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

    void heartbeatLoop(Room& room, SyncMetricsCollector& metrics, std::atomic_bool& running)
    {
        int nextSeq = 1;

        while (running)
        {
            std::vector<ClientConnection> clients = room.getClientSnapshot();
            if (!clients.empty())
            {
                SyncMessage ping;
                ping.type = MessageType::Ping;
                ping.sequenceNumber = nextSeq++;

                for (const ClientConnection& client : clients)
                {
                    // 先登记 pending，再发送 PING。
                    // 这样即使 client 很快回 PONG，server 也一定能找到对应的发送时间。
                    Clock::time_point sentAt = Clock::now();
                    metrics.recordPingSent(client.id, ping.sequenceNumber, sentAt);

                    if (!room.sendMessageToClient(client.socket, ping))
                    {
                        std::cout << "heartbeat send failed for client #" << client.id << "\n";
                    }
                }
            }

            std::this_thread::sleep_for(kHeartbeatInterval);
        }
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
    std::mutex controlCommandMutex;
    std::atomic_bool serverRunning{ true };

    std::cout << "SyncServer listening on port " << kServerPort << "...\n";
    std::cout << "Server role: accept clients and broadcast playback commands.\n";
    std::cout << "Press Ctrl+C to stop the server.\n";

    std::thread heartbeatThread(
        heartbeatLoop,
        std::ref(room),
        std::ref(metrics),
        std::ref(serverRunning)
    );

    while (true)
    {
        std::cout << "Waiting for client...\n";

        SocketHandle clientSocket = accept(listenSocket, nullptr, nullptr);
        if (clientSocket == kInvalidSocket)
        {
            std::cout << "accept failed: " << getSocketError() << "\n";
            break;
        }

        int clientId = 0;
        RoomSnapshot initialSnapshot;
        bool snapshotSent = false;

        {
            // 注册 client、读取 Room 和发送首份快照使用与控制命令相同的锁。
            // 因此不可能出现“快照读取完成后、client 加入广播列表前漏掉一条控制命令”的窗口。
            std::lock_guard<std::mutex> controlLock(controlCommandMutex);

            clientId = room.addClient(clientSocket);
            initialSnapshot = room.getSnapshot();
            snapshotSent = room.sendMessageToClient(
                clientSocket,
                makeSnapshotMessage(initialSnapshot)
            );
        }

        if (!snapshotSent)
        {
            std::cout << "initial snapshot send failed for client #" << clientId << "\n";
            room.removeClient(clientSocket);
            metrics.removeClient(clientId);
            closeSocket(clientSocket);
            continue;
        }

        std::cout << "client #" << clientId
            << " connected. online clients: " << room.getClientCount() << "\n";
        std::cout << "initial snapshot sent: epoch=" << initialSnapshot.controlEpoch
            << ", " << syncStateToString(initialSnapshot.state) << "\n";

        // 一个 client 一个线程：每个线程只负责读自己的 clientSocket。
        // room 通过 mutex 保护共享的 client 列表和播放状态。
        std::thread clientThread(
            handleClient,
            clientSocket,
            clientId,
            std::ref(room),
            std::ref(metrics),
            std::ref(controlCommandMutex)
        );
        clientThread.detach();
    }

    serverRunning = false;
    if (heartbeatThread.joinable())
    {
        heartbeatThread.join();
    }

    closeSocket(listenSocket);
    cleanupSockets();
}

