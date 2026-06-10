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
    virtual int getPositionSeconds() const = 0;
};

// 把协议消息应用到播放器。
// 这个函数让 TCP 代码不需要到处写 MessageType::Play / Pause / Seek 的分支。
bool applyMessageToPlayer(const SyncMessage& message, PlayerController& player);

