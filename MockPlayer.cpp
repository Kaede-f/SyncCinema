#include "MockPlayer.h"

#include <iostream>

bool ConsoleMockPlayer::openMedia(const std::string& path)
{
    mediaPath_ = path;
    positionSeconds_ = 0;
    std::cout << "[MockPlayer] open: " << mediaPath_ << "\n";
    return true;
}

bool ConsoleMockPlayer::play()
{
    std::cout << "[MockPlayer] play\n";
    return true;
}

bool ConsoleMockPlayer::pause()
{
    std::cout << "[MockPlayer] pause\n";
    return true;
}

bool ConsoleMockPlayer::seek(int seconds)
{
    if (seconds < 0)
    {
        std::cout << "[MockPlayer] seek failed: seconds must be non-negative\n";
        return false;
    }

    positionSeconds_ = seconds;
    std::cout << "[MockPlayer] seek to " << positionSeconds_ << "\n";
    return true;
}

int ConsoleMockPlayer::getPositionSeconds() const
{
    return positionSeconds_;
}

