// SyncCinema.cpp : Defines the entry point for the application.

#include "SyncCinema.h"

#include <iostream>
#include <string>

#include "Client.h"
#include "SyncServer.h"

namespace
{
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
        std::cout << "  SyncCinema.exe --help\n";
        std::cout << "\n";
        std::cout << "New broadcast architecture:\n";
        std::cout << "  server only coordinates clients and does not open a video file.\n";
        std::cout << "  every client opens the same local video file and receives broadcast commands.\n";
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
        if (argc < 3)
        {
            std::cout << "Missing video path for client.\n";
            printModeHelp();
            return 0;
        }

        std::string videoPath = argv[2];
        runClient(videoPath);
        return 0;
    }

    std::cout << "Unknown mode: " << mode << "\n";
    printModeHelp();
    return 0;
}

