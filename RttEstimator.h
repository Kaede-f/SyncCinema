#pragma once

#include <cstddef>
#include <deque>

struct RttStatistics
{
    long long totalSamples = 0;
    std::size_t windowSamples = 0;
    long long latestUs = -1;
    long long lifetimeMinUs = -1;
    long long lifetimeMaxUs = -1;
    long long baselineUs = -1;
    long long rollingAverageUs = -1;
    long long rollingMedianUs = -1;
    long long rollingP95Us = -1;

    bool available() const
    {
        return totalSamples > 0;
    }
};

// 只用最近一段 RTT 样本估计当前网络状态。
// lifetime min/max 用于诊断历史异常；同步补偿使用窗口 P10 作为基线，
// 避免一次进程暂停或旧网络环境永久污染后续估计。
class RttEstimator
{
public:
    explicit RttEstimator(std::size_t windowCapacity = 60);

    RttStatistics addSample(long long rttUs);
    RttStatistics snapshot() const;
    void clear();

private:
    std::size_t windowCapacity_ = 60;
    std::deque<long long> recentSamplesUs_;
    long long totalSamples_ = 0;
    long long latestUs_ = -1;
    long long lifetimeMinUs_ = -1;
    long long lifetimeMaxUs_ = -1;
};
