#pragma once

#include <mutex>
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

    void setEventCallback(PlayerEventCallback callback) override;
    bool openMedia(const std::string& mediaSource) override;
    bool play() override;
    bool pause() override;
    bool seek(int seconds) override;
    bool seekMilliseconds(long long milliseconds) override;
    bool isSeekable() const override;
    long long getPositionMilliseconds() const override;
    int getPositionSeconds() const override;
    bool setVideoOutputWindow(void* nativeWindow) override;
    long long getDurationMilliseconds() const override;
    bool setVolume(int volume) override;
    int getVolume() const override;

private:
    static void handleLibVlcEvent(const libvlc_event_t* event, void* userData);

    bool attachPlayerEvents();
    void detachPlayerEvents();
    void dispatchPlayerEvent(const PlayerEvent& event);
    void releaseCurrentMediaPlayer();
    libvlc_media_t* createMediaFromSource(const std::string& mediaSource);
    void applyVideoOutputWindow();

    libvlc_instance_t* vlcInstance_ = nullptr;
    libvlc_media_player_t* mediaPlayer_ = nullptr;
    void* videoOutputWindow_ = nullptr;
    std::string mediaPath_;
    mutable std::mutex eventCallbackMutex_;
    PlayerEventCallback eventCallback_;
    bool playerEventsAttached_ = false;
};
