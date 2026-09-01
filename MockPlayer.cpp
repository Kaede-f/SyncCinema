#include "MockPlayer.h"

#include <iostream>
#include <utility>

void ConsoleMockPlayer::setEventCallback(PlayerEventCallback callback)
{
    eventCallback_ = std::move(callback);
}

bool ConsoleMockPlayer::openMedia(const std::string& path)
{
    mediaPath_ = path;
    positionSeconds_ = 0;
    positionMilliseconds_ = 0;
    std::cout << "[MockPlayer] open: " << mediaPath_ << "\n";
    emitEvent(PlayerEventType::Opening);
    return true;
}

bool ConsoleMockPlayer::play()
{
    std::cout << "[MockPlayer] play\n";
    emitEvent(PlayerEventType::Playing);
    return true;
}

bool ConsoleMockPlayer::pause()
{
    std::cout << "[MockPlayer] pause\n";
    emitEvent(PlayerEventType::Paused);
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

bool ConsoleMockPlayer::setVideoOutputWindow(void* nativeWindow)
{
    (void)nativeWindow;
    return true;
}

long long ConsoleMockPlayer::getDurationMilliseconds() const
{
    return 0;
}

bool ConsoleMockPlayer::setVolume(int volume)
{
    if (volume < 0 || volume > 100)
    {
        return false;
    }

    volume_ = volume;
    return true;
}

int ConsoleMockPlayer::getVolume() const
{
    return volume_;
}

void ConsoleMockPlayer::emitEvent(PlayerEventType type)
{
    if (eventCallback_)
    {
        eventCallback_(PlayerEvent{ type, 0 });
    }
}
