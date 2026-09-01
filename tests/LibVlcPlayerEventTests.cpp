#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>

#include "LibVlcPlayer.h"

int main()
{
    std::mutex eventMutex;
    std::condition_variable eventCondition;
    bool errorReceived = false;

    LibVlcPlayer player;
    player.setEventCallback(
        [&](const PlayerEvent& event)
        {
            if (event.type != PlayerEventType::Error)
            {
                return;
            }

            {
                std::lock_guard<std::mutex> lock(eventMutex);
                errorReceived = true;
            }
            eventCondition.notify_one();
        }
    );

    // openMedia 只创建 libVLC 对象；文件不存在属于稍后发生的异步播放错误。
    // 这个测试专门锁住从 libVLC 内部线程到 PlayerEvent 的转换链路。
    if (!player.openMedia("SyncCinema_test_media_that_does_not_exist.mp4"))
    {
        std::cout << "[FAIL] openMedia should create an asynchronous media request\n";
        return 1;
    }

    if (!player.play())
    {
        std::cout << "[FAIL] play should start the asynchronous media request\n";
        return 1;
    }

    std::unique_lock<std::mutex> lock(eventMutex);
    bool receivedBeforeTimeout = eventCondition.wait_for(
        lock,
        std::chrono::seconds(5),
        [&errorReceived]()
        {
            return errorReceived;
        }
    );

    if (!receivedBeforeTimeout)
    {
        std::cout << "[FAIL] libVLC error event timed out\n";
        return 1;
    }

    std::cout << "[PASS] libVLC asynchronous media error was translated\n";
    return 0;
}
