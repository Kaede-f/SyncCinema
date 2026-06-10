#pragma once

#include <string>

#include "PlayerController.h"

// ConsoleMockPlayer 是阶段 A/B 使用的假播放器。
// 它不打开真实视频窗口，只在控制台打印行为，方便先验证网络和业务流程。
class ConsoleMockPlayer : public PlayerController
{
public:
    bool openMedia(const std::string& path) override;
    bool play() override;
    bool pause() override;
    bool seek(int seconds) override;
    int getPositionSeconds() const override;

private:
    std::string mediaPath_;
    int positionSeconds_ = 0;
};

