#include "LibVlcPlayer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <utility>

namespace
{
    constexpr std::array<libvlc_event_type_t, 7> kObservedPlayerEvents{
        libvlc_MediaPlayerOpening,
        libvlc_MediaPlayerBuffering,
        libvlc_MediaPlayerPlaying,
        libvlc_MediaPlayerPaused,
        libvlc_MediaPlayerStopped,
        libvlc_MediaPlayerEndReached,
        libvlc_MediaPlayerEncounteredError
    };

    bool isNetworkMediaSource(const std::string& mediaSource)
    {
        return mediaSource.starts_with("http://") || mediaSource.starts_with("https://");
    }

    std::string normalizeLocalPathForLibVlc(std::string path)
    {
#ifdef _WIN32
        std::replace(path.begin(), path.end(), '/', '\\');
#endif
        return path;
    }
}

void LibVlcPlayer::setEventCallback(PlayerEventCallback callback)
{
    std::lock_guard<std::mutex> lock(eventCallbackMutex_);
    eventCallback_ = std::move(callback);
}

LibVlcPlayer::LibVlcPlayer()
{
    // libvlc_new 创建 libVLC 运行实例。
    // 这里传 0/nullptr 表示先使用 libVLC 默认配置，让它自己创建默认视频窗口。
    vlcInstance_ = libvlc_new(0, nullptr);
    if (vlcInstance_ == nullptr)
    {
        std::cout << "[LibVLC] libvlc_new failed. Check VLC runtime files.\n";
    }
}

LibVlcPlayer::~LibVlcPlayer()
{
    releaseCurrentMediaPlayer();

    if (vlcInstance_ != nullptr)
    {
        libvlc_release(vlcInstance_);
        vlcInstance_ = nullptr;
    }
}

bool LibVlcPlayer::openMedia(const std::string& mediaSource)
{
    if (vlcInstance_ == nullptr)
    {
        std::cout << "[LibVLC] cannot open media because libVLC was not initialized.\n";
        return false;
    }

    releaseCurrentMediaPlayer();

    libvlc_media_t* media = createMediaFromSource(mediaSource);
    if (media == nullptr)
    {
        std::cout << "[LibVLC] failed to create media: " << mediaSource << "\n";
        return false;
    }

    mediaPlayer_ = libvlc_media_player_new_from_media(media);
    libvlc_media_release(media);

    if (mediaPlayer_ == nullptr)
    {
        std::cout << "[LibVLC] failed to create media player.\n";
        return false;
    }

    if (!attachPlayerEvents())
    {
        std::cout << "[LibVLC] failed to subscribe to player events.\n";
        releaseCurrentMediaPlayer();
        return false;
    }

    applyVideoOutputWindow();
    mediaPath_ = mediaSource;
    std::cout << "[LibVLC] open: " << mediaPath_ << "\n";
    return true;
}

void LibVlcPlayer::handleLibVlcEvent(
    const libvlc_event_t* event,
    void* userData)
{
    if (event == nullptr || userData == nullptr)
    {
        return;
    }

    auto* player = static_cast<LibVlcPlayer*>(userData);
    PlayerEvent translated;

    switch (event->type)
    {
    case libvlc_MediaPlayerOpening:
        translated.type = PlayerEventType::Opening;
        break;
    case libvlc_MediaPlayerBuffering:
        translated.type = PlayerEventType::Buffering;
        translated.bufferingPercent = std::clamp(
            static_cast<int>(std::lround(
                event->u.media_player_buffering.new_cache
            )),
            0,
            100
        );
        break;
    case libvlc_MediaPlayerPlaying:
        translated.type = PlayerEventType::Playing;
        break;
    case libvlc_MediaPlayerPaused:
        translated.type = PlayerEventType::Paused;
        break;
    case libvlc_MediaPlayerStopped:
        translated.type = PlayerEventType::Stopped;
        break;
    case libvlc_MediaPlayerEndReached:
        translated.type = PlayerEventType::EndReached;
        break;
    case libvlc_MediaPlayerEncounteredError:
        translated.type = PlayerEventType::Error;
        break;
    default:
        return;
    }

    player->dispatchPlayerEvent(translated);
}

bool LibVlcPlayer::attachPlayerEvents()
{
    if (mediaPlayer_ == nullptr)
    {
        return false;
    }

    libvlc_event_manager_t* eventManager =
        libvlc_media_player_event_manager(mediaPlayer_);
    if (eventManager == nullptr)
    {
        return false;
    }

    std::size_t attachedCount = 0;
    for (libvlc_event_type_t eventType : kObservedPlayerEvents)
    {
        if (libvlc_event_attach(
                eventManager,
                eventType,
                &LibVlcPlayer::handleLibVlcEvent,
                this) != 0)
        {
            for (std::size_t index = 0; index < attachedCount; ++index)
            {
                libvlc_event_detach(
                    eventManager,
                    kObservedPlayerEvents[index],
                    &LibVlcPlayer::handleLibVlcEvent,
                    this
                );
            }
            return false;
        }
        ++attachedCount;
    }

    playerEventsAttached_ = true;
    return true;
}

void LibVlcPlayer::detachPlayerEvents()
{
    if (!playerEventsAttached_ || mediaPlayer_ == nullptr)
    {
        return;
    }

    libvlc_event_manager_t* eventManager =
        libvlc_media_player_event_manager(mediaPlayer_);
    if (eventManager != nullptr)
    {
        for (libvlc_event_type_t eventType : kObservedPlayerEvents)
        {
            libvlc_event_detach(
                eventManager,
                eventType,
                &LibVlcPlayer::handleLibVlcEvent,
                this
            );
        }
    }

    playerEventsAttached_ = false;
}

void LibVlcPlayer::dispatchPlayerEvent(const PlayerEvent& event)
{
    PlayerEventCallback callback;
    {
        std::lock_guard<std::mutex> lock(eventCallbackMutex_);
        callback = eventCallback_;
    }

    // 回调在锁外执行，避免 UI 处理过程中反向调用播放器导致死锁。
    if (callback)
    {
        callback(event);
    }
}

bool LibVlcPlayer::play()
{
    if (mediaPlayer_ == nullptr)
    {
        std::cout << "[LibVLC] play failed: no media opened.\n";
        return false;
    }

    // libvlc_media_player_play 开始或继续播放。
    if (libvlc_media_player_play(mediaPlayer_) != 0)
    {
        std::cout << "[LibVLC] play failed.\n";
        return false;
    }

    std::cout << "[LibVLC] play\n";
    return true;
}

bool LibVlcPlayer::pause()
{
    if (mediaPlayer_ == nullptr)
    {
        std::cout << "[LibVLC] pause failed: no media opened.\n";
        return false;
    }

    // set_pause(1) 明确进入暂停状态；相比 toggle pause，更适合网络同步命令。
    libvlc_media_player_set_pause(mediaPlayer_, 1);
    std::cout << "[LibVLC] pause\n";
    return true;
}

bool LibVlcPlayer::seek(int seconds)
{
    if (seconds < 0)
    {
        std::cout << "[LibVLC] seek failed: seconds must be non-negative.\n";
        return false;
    }

    return seekMilliseconds(static_cast<long long>(seconds) * 1000);
}

bool LibVlcPlayer::seekMilliseconds(long long milliseconds)
{
    if (mediaPlayer_ == nullptr)
    {
        std::cout << "[LibVLC] seek failed: no media opened.\n";
        return false;
    }

    if (milliseconds < 0)
    {
        std::cout << "[LibVLC] seek failed: milliseconds must be non-negative.\n";
        return false;
    }

    // libVLC 的时间单位本来就是毫秒，因此快照不需要先降精度到整秒。
    libvlc_media_player_set_time(
        mediaPlayer_,
        static_cast<libvlc_time_t>(milliseconds)
    );

    std::cout << "[LibVLC] seek to " << milliseconds << " ms\n";
    return true;
}

bool LibVlcPlayer::isSeekable() const
{
    return mediaPlayer_ != nullptr &&
        libvlc_media_player_is_seekable(mediaPlayer_) != 0;
}

long long LibVlcPlayer::getPositionMilliseconds() const
{
    if (mediaPlayer_ == nullptr)
    {
        return 0;
    }

    libvlc_time_t milliseconds = libvlc_media_player_get_time(mediaPlayer_);
    if (milliseconds < 0)
    {
        return 0;
    }

    return static_cast<long long>(milliseconds);
}

int LibVlcPlayer::getPositionSeconds() const
{
    return static_cast<int>(getPositionMilliseconds() / 1000);
}

bool LibVlcPlayer::setVideoOutputWindow(void* nativeWindow)
{
    videoOutputWindow_ = nativeWindow;
    applyVideoOutputWindow();
    return nativeWindow != nullptr;
}

long long LibVlcPlayer::getDurationMilliseconds() const
{
    if (mediaPlayer_ == nullptr)
    {
        return 0;
    }

    libvlc_time_t duration = libvlc_media_player_get_length(mediaPlayer_);
    return duration > 0 ? static_cast<long long>(duration) : 0;
}

bool LibVlcPlayer::setVolume(int volume)
{
    if (mediaPlayer_ == nullptr)
    {
        return false;
    }

    int clampedVolume = std::clamp(volume, 0, 100);
    return libvlc_audio_set_volume(mediaPlayer_, clampedVolume) == 0;
}

int LibVlcPlayer::getVolume() const
{
    if (mediaPlayer_ == nullptr)
    {
        return 100;
    }

    int volume = libvlc_audio_get_volume(mediaPlayer_);
    return volume >= 0 ? volume : 100;
}

void LibVlcPlayer::releaseCurrentMediaPlayer()
{
    if (mediaPlayer_ != nullptr)
    {
        // 先解绑再 stop/release，避免销毁流程产生的 Stopped 事件回调已释放对象。
        detachPlayerEvents();
        libvlc_media_player_stop(mediaPlayer_);
        libvlc_media_player_release(mediaPlayer_);
        mediaPlayer_ = nullptr;
    }
}

libvlc_media_t* LibVlcPlayer::createMediaFromSource(const std::string& mediaSource)
{
    if (isNetworkMediaSource(mediaSource))
    {
        return libvlc_media_new_location(vlcInstance_, mediaSource.c_str());
    }

    std::string localPath = normalizeLocalPathForLibVlc(mediaSource);
    return libvlc_media_new_path(vlcInstance_, localPath.c_str());
}

void LibVlcPlayer::applyVideoOutputWindow()
{
#ifdef _WIN32
    if (mediaPlayer_ != nullptr && videoOutputWindow_ != nullptr)
    {
        libvlc_media_player_set_hwnd(mediaPlayer_, videoOutputWindow_);
    }
#else
    // 当前 Qt client 只在 Windows 构建。保留分支便于未来扩展其他平台。
    (void)videoOutputWindow_;
#endif
}
