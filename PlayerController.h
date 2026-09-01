#pragma once

#include <string>

#include "Protocol.h"

// PlayerController 是“播放器控制接口”。
// TCP 层只需要知道：播放器可以打开媒体、播放、暂停、跳转、查询进度。
// 具体底层是 MockPlayer、libVLC，还是将来的其他播放器，都藏在这个接口后面。
class PlayerController
{
public:
    virtual ~PlayerController();

    virtual bool openMedia(const std::string& path) = 0;
    virtual bool play() = 0;
    virtual bool pause() = 0;
    virtual bool seek(int seconds) = 0;

    // 房间快照和同步校正都使用毫秒精度。
    // 单独保留 seek(int seconds) 是为了兼容命令行的 seek <seconds> 交互。
    virtual bool seekMilliseconds(long long milliseconds) = 0;

    // 网络媒体刚开始缓冲时可能暂时不能 seek。
    // client 在应用晚加入快照前查询这个能力，避免把“API 已调用”误当成“播放器已准备好”。
    virtual bool isSeekable() const = 0;

    // 毫秒级进度用于同步观测和后续同步算法。
    // 秒级进度适合命令行展示；毫秒级进度才能看出真实同步偏差。
    virtual long long getPositionMilliseconds() const = 0;
    virtual int getPositionSeconds() const = 0;

    // GUI 播放器把 libVLC 画面嵌入自己的原生窗口。
    // 命令行 MockPlayer 不需要真实窗口，但仍实现同一接口以保持可替换性。
    virtual bool setVideoOutputWindow(void* nativeWindow) = 0;

    virtual long long getDurationMilliseconds() const = 0;
    virtual bool setVolume(int volume) = 0;
    virtual int getVolume() const = 0;
};

// 把协议消息应用到播放器。
// 这个函数让 TCP 代码不需要到处写 MessageType::Play / Pause / Seek 的分支。
bool applyMessageToPlayer(const SyncMessage& message, PlayerController& player);
