#pragma once

#include <string>

#include <vlc/vlc.h>

#include "PlayerController.h"

// LibVlcPlayer 是真实播放器封装。
// TCP 层仍然只通过 PlayerController 调用 play/pause/seek，不直接接触 libVLC 的 C API。
class LibVlcPlayer : public PlayerController
{
public:
    LibVlcPlayer();
    ~LibVlcPlayer() override;

    bool openMedia(const std::string& mediaSource) override;
    bool play() override;
    bool pause() override;
    bool seek(int seconds) override;
    bool seekMilliseconds(long long milliseconds) override;
    bool isSeekable() const override;
    long long getPositionMilliseconds() const override;
    int getPositionSeconds() const override;

private:
    void releaseCurrentMediaPlayer();
    libvlc_media_t* createMediaFromSource(const std::string& mediaSource);

    libvlc_instance_t* vlcInstance_ = nullptr;
    libvlc_media_player_t* mediaPlayer_ = nullptr;
    std::string mediaPath_;
};
