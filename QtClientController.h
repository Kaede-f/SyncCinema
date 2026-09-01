#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <QObject>
#include <QString>
#include <QtGui/qwindowdefs.h>

#include "Protocol.h"

class LibVlcPlayer;
class SyncClientSession;

// QtClientController 是 Qt 界面与纯 C++ 会话层之间的适配器。
// 网络线程不会直接操作 QWidget，而是通过 queued signal 回到 UI 线程。
class QtClientController : public QObject
{
    Q_OBJECT

public:
    explicit QtClientController(QObject* parent = nullptr);
    ~QtClientController() override;

    void connectToRoom(
        const QString& mediaSource,
        const QString& serverHost,
        WId videoWindowId
    );
    void disconnectFromRoom();

    void play();
    void pause();
    void seekSeconds(int seconds);
    void setVolume(int volume);

    bool isConnected() const;
    qint64 positionMilliseconds() const;
    qint64 durationMilliseconds() const;
    int volume() const;

signals:
    void busyChanged(bool busy, const QString& message);
    void connectionChanged(bool connected);
    void playbackChanged(int playbackState, qint64 positionMs);
    void errorOccurred(const QString& message);
    void logMessage(const QString& message);

private:
    void sendControl(const SyncMessage& message);
    void postError(const std::string& message);
    void postLog(const std::string& message);
    void joinStartupThread();
    void releaseResources();

    mutable std::mutex resourcesMutex_;
    std::unique_ptr<LibVlcPlayer> player_;
    std::unique_ptr<SyncClientSession> session_;

    std::thread startupThread_;
    std::atomic_bool startupRunning_{ false };
    std::atomic_bool cancelStartup_{ false };
};
