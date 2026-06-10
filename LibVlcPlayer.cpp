#include "LibVlcPlayer.h"

#include <iostream>

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

bool LibVlcPlayer::openMedia(const std::string& path)
{
    if (vlcInstance_ == nullptr)
    {
        std::cout << "[LibVLC] cannot open media because libVLC was not initialized.\n";
        return false;
    }

    releaseCurrentMediaPlayer();

    // libvlc_media_new_path 只告诉 VLC 要播放哪个本地文件，不会通过网络传输视频内容。
    libvlc_media_t* media = libvlc_media_new_path(vlcInstance_, path.c_str());
    if (media == nullptr)
    {
        std::cout << "[LibVLC] failed to create media: " << path << "\n";
        return false;
    }

    mediaPlayer_ = libvlc_media_player_new_from_media(media);
    libvlc_media_release(media);

    if (mediaPlayer_ == nullptr)
    {
        std::cout << "[LibVLC] failed to create media player.\n";
        return false;
    }

    mediaPath_ = path;
    std::cout << "[LibVLC] open: " << mediaPath_ << "\n";
    return true;
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
    if (mediaPlayer_ == nullptr)
    {
        std::cout << "[LibVLC] seek failed: no media opened.\n";
        return false;
    }

    if (seconds < 0)
    {
        std::cout << "[LibVLC] seek failed: seconds must be non-negative.\n";
        return false;
    }

    // libVLC 的时间单位是毫秒；命令行协议里使用秒，所以这里乘以 1000。
    libvlc_media_player_set_time(
        mediaPlayer_,
        static_cast<libvlc_time_t>(seconds) * 1000
    );

    std::cout << "[LibVLC] seek to " << seconds << "\n";
    return true;
}

int LibVlcPlayer::getPositionSeconds() const
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

    return static_cast<int>(milliseconds / 1000);
}

void LibVlcPlayer::releaseCurrentMediaPlayer()
{
    if (mediaPlayer_ != nullptr)
    {
        libvlc_media_player_stop(mediaPlayer_);
        libvlc_media_player_release(mediaPlayer_);
        mediaPlayer_ = nullptr;
    }
}

