// SyncCinema.cpp : Defines the entry point for the application.

#include "SyncCinema.h"

#include <iostream>
#include <string>

#include "Client.h"
#include "SyncServer.h"

namespace
{
    constexpr const char* kDefaultServerIp = "127.0.0.1";

    void printBanner()
    {
        std::cout << "====================================\n";
        std::cout << "        SyncCinema Project\n";
        std::cout << "====================================\n\n";
    }

    void printModeHelp()
    {
        std::cout << "SyncCinema usage:\n";
        std::cout << "  SyncCinema.exe --server\n";
        std::cout << "  SyncCinema.exe --client \"D:\\videos\\test.mp4\"\n";
        std::cout << "  SyncCinema.exe --client \"D:\\videos\\test.mp4\" 127.0.0.1\n";
        std::cout << "  SyncCinema.exe --client \"http://server/videos/test.mp4\" 127.0.0.1\n";
        std::cout << "  SyncCinema.exe --help\n";
        std::cout << "\n";
        std::cout << "New broadcast architecture:\n";
        std::cout << "  server only coordinates clients and does not open a video file.\n";
        std::cout << "  every client opens the same media source and receives broadcast commands.\n";
    }
}

int main(int argc, char* argv[])
{
    printBanner();

    if (argc < 2)
    {
        std::cout << "No mode specified.\n";
        printModeHelp();
        return 0;
    }

    std::string mode = argv[1];

    if (mode == "--help")
    {
        printModeHelp();
        return 0;
    }

    if (mode == "--server")
    {
        runSyncServer();
        return 0;
    }

    if (mode == "--client")
    {
        if (argc < 3 || argc > 4)
        {
            std::cout << "Illegal client command format.\n";
            printModeHelp();
            return 0;
        }

        std::string mediaSource = argv[2];
        std::string serverIp = (argc == 4) ? argv[3] : kDefaultServerIp;
        runClient(mediaSource, serverIp);

        return 0;
    }

    std::cout << "Unknown mode: " << mode << "\n";
    printModeHelp();
    return 0;
}
