#pragma once

#include <chrono>

#include "Protocol.h"

struct PlaybackPositionConvergenceConfig
{
    long long toleranceMilliseconds = 250;
    int requiredConsecutiveSamples = 2;
};

struct PlaybackPositionObservation
{
    long long actualPositionMilliseconds = 0;
    long long expectedPositionMilliseconds = 0;
    long long errorMilliseconds = 0;
    int consecutiveSamples = 0;
    bool converged = false;
};

// libVLC 的 seek 是异步命令，API 返回并不代表播放器已经到达目标位置。
// 该跟踪器只负责判断一组位置观测是否已经稳定接近目标，因此不依赖
// socket、线程或真实播放器，可以在核心测试中确定性验证。
class PlaybackPositionConvergenceTracker
{
public:
    using Clock = std::chrono::steady_clock;

    PlaybackPositionConvergenceTracker(
        long long targetPositionMilliseconds,
        PlaybackState playbackState,
        Clock::time_point targetReferenceTime,
        PlaybackPositionConvergenceConfig config = {}
    );

    PlaybackPositionObservation observe(
        long long actualPositionMilliseconds,
        Clock::time_point observedAt
    );

private:
    long long targetPositionMilliseconds_ = 0;
    PlaybackState playbackState_ = PlaybackState::Stopped;
    Clock::time_point targetReferenceTime_{};
    PlaybackPositionConvergenceConfig config_;
    int consecutiveSamples_ = 0;
};
