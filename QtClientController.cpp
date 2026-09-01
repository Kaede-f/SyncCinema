#include "QtClientController.h"

#include <utility>

#include <QMetaObject>

#include "LibVlcPlayer.h"
#include "SyncClientSession.h"

QtClientController::QtClientController(QObject* parent)
    : QObject(parent)
{
}

QtClientController::~QtClientController()
{
    cancelStartup_ = true;
    joinStartupThread();
    releaseResources();
}

void QtClientController::connectToRoom(
    const QString& mediaSource,
    const QString& serverHost,
    WId videoWindowId)
{
    if (startupRunning_ || isConnected())
    {
        emit errorOccurred(QStringLiteral("客户端正在连接或已经连接。"));
        return;
    }

    joinStartupThread();
    // 远端异常断线后，旧会话仍可能保留在 controller 中。
    // 新建连接前按依赖顺序释放它，避免覆盖仍引用旧 player 的 session。
    releaseResources();
    cancelStartup_ = false;
    startupRunning_ = true;
    emit busyChanged(true, QStringLiteral("正在准备播放器..."));

    std::string media = mediaSource.toUtf8().toStdString();
    std::string server = serverHost.toUtf8().toStdString();
    void* nativeWindow = reinterpret_cast<void*>(videoWindowId);

    startupThread_ = std::thread(
        [this, media = std::move(media), server = std::move(server), nativeWindow]()
        {
            auto player = std::make_unique<LibVlcPlayer>();
            if (cancelStartup_)
            {
                startupRunning_ = false;
                return;
            }

            if (!player->setVideoOutputWindow(nativeWindow))
            {
                postError("无法取得视频区域的原生窗口句柄。");
                startupRunning_ = false;
                QMetaObject::invokeMethod(
                    this,
                    [this]()
                    {
                        emit busyChanged(false, QString());
                    },
                    Qt::QueuedConnection
                );
                return;
            }

            if (!player->openMedia(media))
            {
                postError("无法打开媒体，请检查路径或 URL。");
                startupRunning_ = false;
                QMetaObject::invokeMethod(
                    this,
                    [this]()
                    {
                        emit busyChanged(false, QString());
                    },
                    Qt::QueuedConnection
                );
                return;
            }

            SyncClientCallbacks callbacks;
            callbacks.onLog = [this](const std::string& message)
            {
                postLog(message);
            };
            callbacks.onError = [this](const std::string& message)
            {
                postError(message);
            };
            callbacks.onConnectionChanged = [this](bool connected)
            {
                if (!connected)
                {
                    QMetaObject::invokeMethod(
                        this,
                        [this]()
                        {
                            emit connectionChanged(false);
                        },
                        Qt::QueuedConnection
                    );
                }
            };
            callbacks.onPlaybackChanged =
                [this](
                    const SyncClientPlaybackSnapshot& snapshot,
                    SyncClientCommandSource)
            {
                int state = static_cast<int>(snapshot.state.state);
                qint64 position =
                    static_cast<qint64>(snapshot.playerPositionMs);
                QMetaObject::invokeMethod(
                    this,
                    [this, state, position]()
                    {
                        emit playbackChanged(state, position);
                    },
                    Qt::QueuedConnection
                );
            };

            auto session = std::make_unique<SyncClientSession>(
                *player,
                std::move(callbacks)
            );

            QMetaObject::invokeMethod(
                this,
                [this]()
                {
                    emit busyChanged(true, QStringLiteral("正在连接房间..."));
                },
                Qt::QueuedConnection
            );

            std::string errorMessage;
            SyncClientConnectMetrics metrics;
            if (!session->connectToRoom(
                    server,
                    media,
                    errorMessage,
                    &metrics))
            {
                postError(errorMessage);
                startupRunning_ = false;
                QMetaObject::invokeMethod(
                    this,
                    [this]()
                    {
                        emit busyChanged(false, QString());
                        emit connectionChanged(false);
                    },
                    Qt::QueuedConnection
                );
                return;
            }

            if (cancelStartup_)
            {
                session->disconnect();
                startupRunning_ = false;
                return;
            }

            {
                std::lock_guard<std::mutex> lock(resourcesMutex_);
                player_ = std::move(player);
                session_ = std::move(session);
            }

            startupRunning_ = false;
            QMetaObject::invokeMethod(
                this,
                [this, metrics]()
                {
                    emit logMessage(
                        QStringLiteral(
                            "[metric] connect_server_ms=%1 initial_sync_ms=%2"
                        )
                            .arg(metrics.connectServerMs)
                            .arg(metrics.initialSyncMs)
                    );
                    emit busyChanged(false, QString());
                    emit connectionChanged(true);
                },
                Qt::QueuedConnection
            );
        }
    );
}

void QtClientController::disconnectFromRoom()
{
    cancelStartup_ = true;
    releaseResources();
    emit connectionChanged(false);
    emit busyChanged(false, QString());
}

void QtClientController::play()
{
    SyncMessage message;
    message.type = MessageType::Play;
    sendControl(message);
}

void QtClientController::pause()
{
    SyncMessage message;
    message.type = MessageType::Pause;
    sendControl(message);
}

void QtClientController::seekSeconds(int seconds)
{
    if (seconds < 0)
    {
        emit errorOccurred(QStringLiteral("播放位置不能为负数。"));
        return;
    }

    SyncMessage message;
    message.type = MessageType::Seek;
    message.positionSeconds = seconds;
    message.positionMilliseconds =
        static_cast<long long>(seconds) * 1000;
    sendControl(message);
}

void QtClientController::setVolume(int volume)
{
    std::lock_guard<std::mutex> lock(resourcesMutex_);
    if (session_ && !session_->setVolume(volume))
    {
        emit errorOccurred(QStringLiteral("无法设置音量。"));
    }
}

bool QtClientController::isConnected() const
{
    std::lock_guard<std::mutex> lock(resourcesMutex_);
    return session_ && session_->isConnected();
}

qint64 QtClientController::positionMilliseconds() const
{
    std::lock_guard<std::mutex> lock(resourcesMutex_);
    if (!session_)
    {
        return 0;
    }

    return static_cast<qint64>(
        session_->getPlaybackSnapshot().playerPositionMs
    );
}

qint64 QtClientController::durationMilliseconds() const
{
    std::lock_guard<std::mutex> lock(resourcesMutex_);
    if (!session_)
    {
        return 0;
    }

    return static_cast<qint64>(
        session_->getPlaybackSnapshot().durationMs
    );
}

int QtClientController::volume() const
{
    std::lock_guard<std::mutex> lock(resourcesMutex_);
    return session_ ? session_->getVolume() : 80;
}

void QtClientController::sendControl(const SyncMessage& message)
{
    std::lock_guard<std::mutex> lock(resourcesMutex_);
    if (!session_)
    {
        emit errorOccurred(QStringLiteral("请先连接房间。"));
        return;
    }

    std::string errorMessage;
    if (!session_->sendControlMessage(message, errorMessage))
    {
        emit errorOccurred(QString::fromUtf8(errorMessage.c_str()));
    }
}

void QtClientController::postError(const std::string& message)
{
    QString text = QString::fromUtf8(message.c_str());
    QMetaObject::invokeMethod(
        this,
        [this, text]()
        {
            emit errorOccurred(text);
        },
        Qt::QueuedConnection
    );
}

void QtClientController::postLog(const std::string& message)
{
    QString text = QString::fromUtf8(message.c_str());
    QMetaObject::invokeMethod(
        this,
        [this, text]()
        {
            emit logMessage(text);
        },
        Qt::QueuedConnection
    );
}

void QtClientController::joinStartupThread()
{
    if (startupThread_.joinable() &&
        startupThread_.get_id() != std::this_thread::get_id())
    {
        startupThread_.join();
    }
}

void QtClientController::releaseResources()
{
    std::unique_ptr<SyncClientSession> session;
    std::unique_ptr<LibVlcPlayer> player;
    {
        std::lock_guard<std::mutex> lock(resourcesMutex_);
        session = std::move(session_);
        player = std::move(player_);
    }

    if (session)
    {
        session->disconnect();
    }

    // session 持有 player 的引用，必须先销毁 session。
    session.reset();
    player.reset();
}
