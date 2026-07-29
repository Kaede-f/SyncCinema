#include "MockPlayer.h"

#include <iostream>

bool ConsoleMockPlayer::openMedia(const std::string& path)
{
    mediaPath_ = path;
    positionSeconds_ = 0;
    positionMilliseconds_ = 0;
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

    return seekMilliseconds(static_cast<long long>(seconds) * 1000);
}

bool ConsoleMockPlayer::seekMilliseconds(long long milliseconds)
{
    if (milliseconds < 0)
    {
        std::cout << "[MockPlayer] seek failed: milliseconds must be non-negative\n";
        return false;
    }

    positionMilliseconds_ = milliseconds;
    positionSeconds_ = static_cast<int>(milliseconds / 1000);
    std::cout << "[MockPlayer] seek to " << positionMilliseconds_ << " ms\n";
    return true;
}

bool ConsoleMockPlayer::isSeekable() const
{
    return !mediaPath_.empty();
}

long long ConsoleMockPlayer::getPositionMilliseconds() const
{
    return positionMilliseconds_;
}

int ConsoleMockPlayer::getPositionSeconds() const
{
    return positionSeconds_;
}
