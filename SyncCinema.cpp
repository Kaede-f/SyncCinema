// SyncCinema.cpp : Defines the entry point for the application.

#include "SyncCinema.h"

#include <iostream>
#include <string>

#include "TcpClient.h"
#include "TcpServer.h"

void printBanner()
{
    std::cout << "====================================\n";
    std::cout << "        SyncCinema Project\n";
    std::cout << "====================================\n\n";
}

void printModeHelp()
{
    std::cout << "SyncCinema usage:\n";
    std::cout << "  SyncCinema.exe --server \"D:\\videos\\test.mp4\"\n";
    std::cout << "  SyncCinema.exe --client \"D:\\videos\\test.mp4\"\n";
    std::cout << "  SyncCinema.exe --help\n";
}

void runServer(const std::string& videoPath)
{
    runTcpServer(videoPath);
}

void runClient(const std::string& videoPath)
{
    runTcpClient(videoPath);
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

    if (mode != "--server" && mode != "--client")
    {
        std::cout << "Unknown mode: " << mode << "\n";
        printModeHelp();
        return 0;
    }

    if (argc < 3)
    {
        std::cout << "Missing video path.\n";
        printModeHelp();
        return 0;
    }

    std::string videoPath = argv[2];

    if (mode == "--server")
    {
        runServer(videoPath);
    }
    else
    {
        runClient(videoPath);
    }

    return 0;
}

