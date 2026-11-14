#pragma once

#include <mutex>

// 所有 server-side [metric] 输出共用同一把锁。
// inline 函数中的局部 static 在整个程序中只有一份，跨 cpp 文件仍能互斥。
inline std::mutex& metricLogMutex()
{
    static std::mutex mutex;
    return mutex;
}
