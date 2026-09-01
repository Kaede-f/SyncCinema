#include "Client.h"

#include <cctype>
#include <chrono>
#include <format>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include "PlayerController.h"
#include "Protocol.h"
#include "SyncClientSession.h"

#ifdef USE_LIBVLC
#include "LibVlcPlayer.h"
using ActivePlayer = LibVlcPlayer;
#else
#include "MockPlayer.h"
using ActivePlayer = ConsoleMockPlayer;
#endif

namespace
{
    using Clock = std::chrono::steady_clock;

    void printLine(std::mutex& consoleMutex, const std::string& text)
    {
        std::lock_guard<std::mutex> lock(consoleMutex);
        std::cout << text << "\n";
    }

    void printClientHelp(std::mutex& consoleMutex)
    {
        std::lock_guard<std::mutex> lock(consoleMutex);
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
            ch = static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch))
            );
        }
        return text;
    }

    bool buildSyncMessageFromInput(
        const std::string& line,
        SyncMessage& message,
        std::string& errorMessage)
    {
        std::istringstream input(line);
        std::string command;
        input >> command;
        command = toLowerAscii(command);

        if (command == "play" || command == "pause")
        {
            std::string extra;
            if (input >> extra)
            {
                errorMessage = command + " does not need extra arguments.";
                return false;
            }

            message.type = command == "play"
                ? MessageType::Play
                : MessageType::Pause;
            return true;
        }

        if (command == "seek")
        {
            int seconds = 0;
            std::string extra;
            if (!(input >> seconds) || seconds < 0 || (input >> extra))
            {
                errorMessage =
                    "seek usage: seek <non-negative seconds>, for example: seek 120";
                return false;
            }

            message.type = MessageType::Seek;
            message.positionSeconds = seconds;
            message.positionMilliseconds =
                static_cast<long long>(seconds) * 1000;
            return true;
        }

        errorMessage = "unknown sync command.";
        return false;
    }
}

void runClient(
    const std::string& mediaSource,
    const std::string& serverIp)
{
    std::mutex consoleMutex;

    Clock::time_point playerInitStartedAt = Clock::now();
    ActivePlayer player;
    long long playerInitMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - playerInitStartedAt
        ).count();
    printLine(
        consoleMutex,
        std::format("[metric] player_init_ms={}", playerInitMs)
    );

    Clock::time_point openMediaStartedAt = Clock::now();
    if (!player.openMedia(mediaSource))
    {
        printLine(
            consoleMutex,
            "client failed to open media: " + mediaSource
        );
        return;
    }

    long long openMediaMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - openMediaStartedAt
        ).count();
    printLine(
        consoleMutex,
        std::format(
            "[metric] open_media_ms={} source={}",
            openMediaMs,
            mediaSource
        )
    );

    SyncClientCallbacks callbacks;
    callbacks.onLog = [&consoleMutex](const std::string& message)
    {
        printLine(consoleMutex, message);
    };
    callbacks.onError = [&consoleMutex](const std::string& message)
    {
        printLine(consoleMutex, "client error: " + message);
    };
    callbacks.onConnectionChanged =
        [&consoleMutex](bool connected)
    {
        printLine(
            consoleMutex,
            connected ? "Connected to server." : "Disconnected from server."
        );
    };

    SyncClientSession session(player, std::move(callbacks));
    SyncClientConnectMetrics connectMetrics;
    std::string errorMessage;

    printLine(
        consoleMutex,
        "Connecting to " + serverIp + ":9000..."
    );
    if (!session.connectToRoom(
            serverIp,
            errorMessage,
            &connectMetrics))
    {
        printLine(consoleMutex, "client connection failed: " + errorMessage);
        return;
    }

    printLine(
        consoleMutex,
        std::format(
            "[metric] connect_server_ms={} server={}",
            connectMetrics.connectServerMs,
            serverIp
        )
    );
    printLine(
        consoleMutex,
        std::format(
            "[metric] initial_sync_ms={}",
            connectMetrics.initialSyncMs
        )
    );

    printClientHelp(consoleMutex);

    std::string line;
    while (session.isConnected())
    {
        {
            std::lock_guard<std::mutex> lock(consoleMutex);
            std::cout << "> ";
        }

        if (!std::getline(std::cin, line))
        {
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
            printClientHelp(consoleMutex);
            continue;
        }

        if (command == "status")
        {
            SyncClientPlaybackSnapshot snapshot =
                session.getPlaybackSnapshot();
            printLine(
                consoleMutex,
                "local status: " + syncStateToString(snapshot.state) +
                ", playerPositionMs=" +
                std::to_string(snapshot.playerPositionMs) +
                ", durationMs=" +
                std::to_string(snapshot.durationMs)
            );
            continue;
        }

        if (command == "quit")
        {
            break;
        }

        SyncMessage message;
        errorMessage.clear();
        if (!buildSyncMessageFromInput(line, message, errorMessage))
        {
            printLine(consoleMutex, errorMessage);
            continue;
        }

        if (!session.sendControlMessage(message, errorMessage))
        {
            printLine(consoleMutex, "command failed: " + errorMessage);
        }
    }

    session.disconnect();
}
