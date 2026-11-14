#include "QtClientWindow.h"

#include <algorithm>
#include <limits>

#include <QCloseEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include "PlayerController.h"
#include "QtClientController.h"

namespace
{
    constexpr int kProgressTimerIntervalMs = 250;
    constexpr int kDefaultVolume = 80;
}

QtClientWindow::QtClientWindow(QWidget* parent)
    : QMainWindow(parent),
      controller_(new QtClientController(this))
{
    buildUi();
    applyStyle();
    connectSignals();
    loadSettings();
    setConnectedUi(false);

    progressTimer_ = new QTimer(this);
    progressTimer_->setInterval(kProgressTimerIntervalMs);
    connect(
        progressTimer_,
        &QTimer::timeout,
        this,
        &QtClientWindow::updateProgress
    );
    progressTimer_->start();
}

QtClientWindow::~QtClientWindow() = default;

bool QtClientWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == videoSurface_ &&
        event->type() == QEvent::MouseButtonDblClick)
    {
        toggleFullScreenMode();
        return true;
    }

    return QMainWindow::eventFilter(watched, event);
}

void QtClientWindow::closeEvent(QCloseEvent* event)
{
    saveSettings();
    controller_->disconnectFromRoom();
    QMainWindow::closeEvent(event);
}

void QtClientWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && isFullScreen())
    {
        toggleFullScreenMode();
        event->accept();
        return;
    }

    QMainWindow::keyPressEvent(event);
}

void QtClientWindow::buildUi()
{
    setWindowTitle(QStringLiteral("SyncCinema"));
    resize(1180, 760);
    setMinimumSize(820, 560);

    auto* centralWidget = new QWidget(this);
    auto* pageLayout = new QVBoxLayout(centralWidget);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);
    setCentralWidget(centralWidget);

    topBar_ = new QWidget(centralWidget);
    topBar_->setObjectName(QStringLiteral("topBar"));
    topBar_->setFixedHeight(72);
    auto* topLayout = new QHBoxLayout(topBar_);
    topLayout->setContentsMargins(20, 14, 20, 14);
    topLayout->setSpacing(10);

    auto* brandLabel = new QLabel(QStringLiteral("SyncCinema"), topBar_);
    brandLabel->setObjectName(QStringLiteral("brandLabel"));
    brandLabel->setMinimumWidth(128);

    mediaSourceEdit_ = new QLineEdit(topBar_);
    mediaSourceEdit_->setObjectName(QStringLiteral("mediaSourceEdit"));
    mediaSourceEdit_->setPlaceholderText(QStringLiteral("媒体文件或 URL"));
    mediaSourceEdit_->setClearButtonEnabled(true);

    browseButton_ = new QToolButton(topBar_);
    browseButton_->setIcon(
        style()->standardIcon(QStyle::SP_DialogOpenButton)
    );
    browseButton_->setToolTip(QStringLiteral("选择本地媒体"));
    browseButton_->setFixedSize(40, 40);

    serverHostEdit_ = new QLineEdit(topBar_);
    serverHostEdit_->setObjectName(QStringLiteral("serverHostEdit"));
    serverHostEdit_->setPlaceholderText(QStringLiteral("服务器地址"));
    serverHostEdit_->setMaximumWidth(190);

    connectButton_ = new QPushButton(QStringLiteral("连接"), topBar_);
    connectButton_->setObjectName(QStringLiteral("connectButton"));
    connectButton_->setFixedSize(78, 40);

    statusIndicator_ = new QLabel(topBar_);
    statusIndicator_->setObjectName(QStringLiteral("statusIndicator"));
    statusIndicator_->setFixedSize(9, 9);

    statusText_ = new QLabel(QStringLiteral("未连接"), topBar_);
    statusText_->setObjectName(QStringLiteral("statusText"));
    statusText_->setMinimumWidth(58);

    topLayout->addWidget(brandLabel);
    topLayout->addWidget(mediaSourceEdit_, 1);
    topLayout->addWidget(browseButton_);
    topLayout->addWidget(serverHostEdit_);
    topLayout->addWidget(connectButton_);
    topLayout->addSpacing(4);
    topLayout->addWidget(statusIndicator_);
    topLayout->addWidget(statusText_);

    videoSurface_ = new QWidget(centralWidget);
    videoSurface_->setObjectName(QStringLiteral("videoSurface"));
    videoSurface_->setAttribute(Qt::WA_NativeWindow);
    videoSurface_->setMinimumHeight(360);
    videoSurface_->setSizePolicy(
        QSizePolicy::Expanding,
        QSizePolicy::Expanding
    );
    videoSurface_->installEventFilter(this);

    auto* videoLayout = new QGridLayout(videoSurface_);
    videoLayout->setContentsMargins(0, 0, 0, 0);
    videoStatusLabel_ = new QLabel(QStringLiteral("未连接"), videoSurface_);
    videoStatusLabel_->setObjectName(QStringLiteral("videoStatusLabel"));
    videoStatusLabel_->setAlignment(Qt::AlignCenter);
    videoStatusLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    videoLayout->addWidget(videoStatusLabel_, 0, 0, Qt::AlignCenter);

    controlBar_ = new QWidget(centralWidget);
    controlBar_->setObjectName(QStringLiteral("controlBar"));
    controlBar_->setFixedHeight(92);
    auto* controlLayout = new QVBoxLayout(controlBar_);
    controlLayout->setContentsMargins(18, 10, 18, 12);
    controlLayout->setSpacing(8);

    auto* progressLayout = new QHBoxLayout();
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(12);
    progressSlider_ = new QSlider(Qt::Horizontal, controlBar_);
    progressSlider_->setObjectName(QStringLiteral("progressSlider"));
    progressSlider_->setRange(0, 0);
    timeLabel_ = new QLabel(QStringLiteral("00:00 / 00:00"), controlBar_);
    timeLabel_->setObjectName(QStringLiteral("timeLabel"));
    timeLabel_->setMinimumWidth(118);
    timeLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    progressLayout->addWidget(progressSlider_, 1);
    progressLayout->addWidget(timeLabel_);

    auto* buttonsLayout = new QHBoxLayout();
    buttonsLayout->setContentsMargins(0, 0, 0, 0);
    buttonsLayout->setSpacing(8);

    playPauseButton_ = new QToolButton(controlBar_);
    playPauseButton_->setIcon(
        style()->standardIcon(QStyle::SP_MediaPlay)
    );
    playPauseButton_->setToolTip(QStringLiteral("播放"));
    playPauseButton_->setFixedSize(42, 42);

    volumeButton_ = new QToolButton(controlBar_);
    volumeButton_->setIcon(
        style()->standardIcon(QStyle::SP_MediaVolume)
    );
    volumeButton_->setToolTip(QStringLiteral("静音"));
    volumeButton_->setFixedSize(38, 38);

    volumeSlider_ = new QSlider(Qt::Horizontal, controlBar_);
    volumeSlider_->setObjectName(QStringLiteral("volumeSlider"));
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(kDefaultVolume);
    volumeSlider_->setFixedWidth(110);

    fullScreenButton_ = new QToolButton(controlBar_);
    fullScreenButton_->setIcon(
        style()->standardIcon(QStyle::SP_TitleBarMaxButton)
    );
    fullScreenButton_->setToolTip(QStringLiteral("全屏"));
    fullScreenButton_->setFixedSize(38, 38);

    buttonsLayout->addWidget(playPauseButton_);
    buttonsLayout->addSpacing(8);
    buttonsLayout->addWidget(volumeButton_);
    buttonsLayout->addWidget(volumeSlider_);
    buttonsLayout->addStretch(1);
    buttonsLayout->addWidget(fullScreenButton_);

    controlLayout->addLayout(progressLayout);
    controlLayout->addLayout(buttonsLayout);

    pageLayout->addWidget(topBar_);
    pageLayout->addWidget(videoSurface_, 1);
    pageLayout->addWidget(controlBar_);
}

void QtClientWindow::applyStyle()
{
    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget {
            background: #f4f5f7;
            color: #20242a;
            font-family: "Microsoft YaHei UI";
            font-size: 13px;
        }

        QWidget#topBar {
            background: #ffffff;
            border-bottom: 1px solid #dfe3e8;
        }

        QWidget#topBar QLabel {
            background: transparent;
        }

        QLabel#brandLabel {
            color: #16191d;
            font-size: 19px;
            font-weight: 700;
        }

        QLineEdit {
            min-height: 38px;
            padding: 0 11px;
            background: #f7f8fa;
            border: 1px solid #d4d9df;
            border-radius: 5px;
            selection-background-color: #1f9d7a;
        }

        QLineEdit:focus {
            background: #ffffff;
            border-color: #1f9d7a;
        }

        QPushButton#connectButton {
            background: #1f9d7a;
            color: #ffffff;
            border: 0;
            border-radius: 5px;
            font-weight: 600;
        }

        QPushButton#connectButton:hover {
            background: #178765;
        }

        QPushButton#connectButton:disabled {
            background: #aab4bd;
        }

        QLabel#statusIndicator {
            background: #9aa3ad;
            border-radius: 4px;
        }

        QLabel#statusText {
            color: #66717d;
            font-size: 12px;
        }

        QWidget#videoSurface {
            background: #090a0c;
        }

        QLabel#videoStatusLabel {
            background: rgba(24, 27, 31, 210);
            color: #f3f5f7;
            border: 1px solid #3a3f47;
            border-radius: 6px;
            padding: 9px 15px;
        }

        QWidget#controlBar {
            background: #191b20;
            border-top: 1px solid #30343b;
        }

        QWidget#controlBar QLabel {
            background: transparent;
            color: #e7eaee;
        }

        QToolButton {
            background: transparent;
            border: 0;
            border-radius: 5px;
            padding: 6px;
        }

        QToolButton:hover {
            background: #30343b;
        }

        QToolButton:pressed {
            background: #3b414a;
        }

        QToolButton:disabled {
            opacity: 0.45;
        }

        QSlider::groove:horizontal {
            height: 4px;
            background: #4a5059;
            border-radius: 2px;
        }

        QSlider::sub-page:horizontal {
            background: #28b589;
            border-radius: 2px;
        }

        QSlider::handle:horizontal {
            width: 14px;
            height: 14px;
            margin: -5px 0;
            background: #f3f5f7;
            border: 1px solid #c7cdd4;
            border-radius: 7px;
        }

        QSlider::handle:horizontal:hover {
            background: #ffffff;
            border-color: #28b589;
        }
    )"));
}

void QtClientWindow::connectSignals()
{
    connect(
        browseButton_,
        &QToolButton::clicked,
        this,
        &QtClientWindow::chooseLocalMedia
    );
    connect(
        connectButton_,
        &QPushButton::clicked,
        this,
        &QtClientWindow::toggleConnection
    );
    connect(
        playPauseButton_,
        &QToolButton::clicked,
        this,
        &QtClientWindow::togglePlayback
    );
    connect(
        progressSlider_,
        &QSlider::sliderReleased,
        this,
        [this]()
        {
            if (connected_)
            {
                controller_->seekSeconds(progressSlider_->value());
            }
        }
    );
    connect(
        volumeSlider_,
        &QSlider::valueChanged,
        controller_,
        &QtClientController::setVolume
    );
    connect(
        volumeButton_,
        &QToolButton::clicked,
        this,
        [this]()
        {
            int nextVolume = volumeSlider_->value() == 0
                ? kDefaultVolume
                : 0;
            volumeSlider_->setValue(nextVolume);
        }
    );
    connect(
        fullScreenButton_,
        &QToolButton::clicked,
        this,
        &QtClientWindow::toggleFullScreenMode
    );

    connect(
        controller_,
        &QtClientController::busyChanged,
        this,
        &QtClientWindow::setBusyUi
    );
    connect(
        controller_,
        &QtClientController::connectionChanged,
        this,
        &QtClientWindow::setConnectedUi
    );
    connect(
        controller_,
        &QtClientController::playbackChanged,
        this,
        [this](int state, qint64 positionMs)
        {
            playbackState_ = static_cast<PlaybackState>(state);
            playPauseButton_->setIcon(
                style()->standardIcon(
                    playbackState_ == PlaybackState::Playing
                        ? QStyle::SP_MediaPause
                        : QStyle::SP_MediaPlay
                )
            );
            playPauseButton_->setToolTip(
                playbackState_ == PlaybackState::Playing
                    ? QStringLiteral("暂停")
                    : QStringLiteral("播放")
            );
            if (!progressSlider_->isSliderDown())
            {
                progressSlider_->setValue(
                    static_cast<int>(positionMs / 1000)
                );
            }
        }
    );
    connect(
        controller_,
        &QtClientController::mediaStatusChanged,
        this,
        &QtClientWindow::setMediaStatusUi
    );
    connect(
        controller_,
        &QtClientController::errorOccurred,
        this,
        [this](const QString& message)
        {
            if (!message.isEmpty())
            {
                QMessageBox::warning(
                    this,
                    QStringLiteral("SyncCinema"),
                    message
                );
            }
        }
    );
}

void QtClientWindow::loadSettings()
{
    QSettings settings;
    mediaSourceEdit_->setText(
        settings.value(QStringLiteral("mediaSource")).toString()
    );
    serverHostEdit_->setText(
        settings.value(
            QStringLiteral("serverHost"),
            QStringLiteral("127.0.0.1")
        ).toString()
    );
    volumeSlider_->setValue(
        settings.value(QStringLiteral("volume"), kDefaultVolume).toInt()
    );
}

void QtClientWindow::saveSettings() const
{
    QSettings settings;
    settings.setValue(
        QStringLiteral("mediaSource"),
        mediaSourceEdit_->text().trimmed()
    );
    settings.setValue(
        QStringLiteral("serverHost"),
        serverHostEdit_->text().trimmed()
    );
    settings.setValue(
        QStringLiteral("volume"),
        volumeSlider_->value()
    );
}

void QtClientWindow::chooseLocalMedia()
{
    QString currentPath = mediaSourceEdit_->text().trimmed();
    QString startDirectory = QFileInfo(currentPath).exists()
        ? QFileInfo(currentPath).absolutePath()
        : QString();

    QString fileName = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择媒体"),
        startDirectory,
        QStringLiteral(
            "视频文件 (*.mp4 *.mkv *.avi *.mov *.m4v);;所有文件 (*.*)"
        )
    );
    if (!fileName.isEmpty())
    {
        mediaSourceEdit_->setText(fileName);
    }
}

void QtClientWindow::toggleConnection()
{
    if (connected_)
    {
        controller_->disconnectFromRoom();
        return;
    }

    QString mediaSource = mediaSourceEdit_->text().trimmed();
    QString serverHost = serverHostEdit_->text().trimmed();
    if (mediaSource.isEmpty() || serverHost.isEmpty())
    {
        QMessageBox::information(
            this,
            QStringLiteral("SyncCinema"),
            QStringLiteral("请填写媒体地址和服务器地址。")
        );
        return;
    }

    saveSettings();
    controller_->connectToRoom(
        mediaSource,
        serverHost,
        videoSurface_->winId()
    );
}

void QtClientWindow::togglePlayback()
{
    if (playbackState_ == PlaybackState::Playing)
    {
        controller_->pause();
    }
    else
    {
        controller_->play();
    }
}

void QtClientWindow::toggleFullScreenMode()
{
    if (isFullScreen())
    {
        showNormal();
        topBar_->show();
        fullScreenButton_->setIcon(
            style()->standardIcon(QStyle::SP_TitleBarMaxButton)
        );
        fullScreenButton_->setToolTip(QStringLiteral("全屏"));
    }
    else
    {
        topBar_->hide();
        showFullScreen();
        fullScreenButton_->setIcon(
            style()->standardIcon(QStyle::SP_TitleBarNormalButton)
        );
        fullScreenButton_->setToolTip(QStringLiteral("退出全屏"));
    }
}

void QtClientWindow::updateProgress()
{
    if (!connected_)
    {
        return;
    }

    qint64 positionMs = controller_->positionMilliseconds();
    qint64 durationMs = controller_->durationMilliseconds();
    int durationSeconds = static_cast<int>(std::min<qint64>(
        durationMs / 1000,
        (std::numeric_limits<int>::max)()
    ));

    if (durationSeconds > 0 &&
        progressSlider_->maximum() != durationSeconds)
    {
        progressSlider_->setRange(0, durationSeconds);
    }

    if (!progressSlider_->isSliderDown())
    {
        progressSlider_->setValue(static_cast<int>(std::min<qint64>(
            positionMs / 1000,
            progressSlider_->maximum()
        )));
    }

    updateTimeLabel(positionMs, durationMs);
}

void QtClientWindow::updateTimeLabel(
    qint64 positionMs,
    qint64 durationMs)
{
    timeLabel_->setText(
        formatTime(positionMs) +
        QStringLiteral(" / ") +
        formatTime(durationMs)
    );
}

void QtClientWindow::setConnectedUi(bool connected)
{
    connected_ = connected;
    busy_ = false;
    connectButton_->setEnabled(true);
    connectButton_->setText(
        connected ? QStringLiteral("断开") : QStringLiteral("连接")
    );
    mediaSourceEdit_->setEnabled(!connected);
    serverHostEdit_->setEnabled(!connected);
    browseButton_->setEnabled(!connected);
    playPauseButton_->setEnabled(connected);
    progressSlider_->setEnabled(connected);
    volumeButton_->setEnabled(connected);
    volumeSlider_->setEnabled(connected);

    statusText_->setText(
        connected ? QStringLiteral("已连接") : QStringLiteral("未连接")
    );
    statusIndicator_->setStyleSheet(
        connected
            ? QStringLiteral("background: #1f9d7a; border-radius: 4px;")
            : QStringLiteral("background: #9aa3ad; border-radius: 4px;")
    );

    if (connected)
    {
        if (!mediaStatusVisible_)
        {
            videoStatusLabel_->setText(QStringLiteral("已连接"));
            QTimer::singleShot(
                1200,
                videoStatusLabel_,
                [this]()
                {
                    if (connected_ && !busy_ && !mediaStatusVisible_)
                    {
                        videoStatusLabel_->hide();
                    }
                }
            );
        }
        // loadSettings 已恢复用户上次选择的音量；连接成功后把它应用到
        // 新创建的播放器，而不是用播放器默认值覆盖界面设置。
        controller_->setVolume(volumeSlider_->value());
    }
    else
    {
        mediaStatusVisible_ = false;
        playbackState_ = PlaybackState::Stopped;
        playPauseButton_->setIcon(
            style()->standardIcon(QStyle::SP_MediaPlay)
        );
        progressSlider_->setRange(0, 0);
        updateTimeLabel(0, 0);
        videoStatusLabel_->setText(QStringLiteral("未连接"));
        videoStatusLabel_->show();
    }
}

void QtClientWindow::setMediaStatusUi(
    int playerEventType,
    int bufferingPercent)
{
    const auto eventType = static_cast<PlayerEventType>(playerEventType);
    switch (eventType)
    {
    case PlayerEventType::Opening:
        videoStatusLabel_->setText(QStringLiteral("正在打开媒体..."));
        mediaStatusVisible_ = true;
        break;
    case PlayerEventType::Buffering:
        videoStatusLabel_->setText(
            QStringLiteral("正在缓冲... %1%").arg(bufferingPercent)
        );
        mediaStatusVisible_ = true;
        break;
    case PlayerEventType::Playing:
    case PlayerEventType::Paused:
        mediaStatusVisible_ = false;
        videoStatusLabel_->hide();
        return;
    case PlayerEventType::Stopped:
        videoStatusLabel_->setText(QStringLiteral("播放已停止"));
        mediaStatusVisible_ = true;
        break;
    case PlayerEventType::EndReached:
        videoStatusLabel_->setText(QStringLiteral("播放结束"));
        mediaStatusVisible_ = true;
        break;
    case PlayerEventType::Error:
        videoStatusLabel_->setText(
            QStringLiteral("媒体播放失败，请检查地址或格式")
        );
        mediaStatusVisible_ = true;
        break;
    }

    videoStatusLabel_->show();
}

void QtClientWindow::setBusyUi(
    bool busy,
    const QString& message)
{
    busy_ = busy;
    connectButton_->setEnabled(!busy);
    mediaSourceEdit_->setEnabled(!busy && !connected_);
    serverHostEdit_->setEnabled(!busy && !connected_);
    browseButton_->setEnabled(!busy && !connected_);

    if (busy)
    {
        statusText_->setText(QStringLiteral("连接中"));
        statusIndicator_->setStyleSheet(
            QStringLiteral("background: #e5a13b; border-radius: 4px;")
        );
        videoStatusLabel_->setText(message);
        videoStatusLabel_->show();
    }
    else if (!connected_)
    {
        setConnectedUi(false);
    }
}

QString QtClientWindow::formatTime(qint64 milliseconds) const
{
    qint64 totalSeconds = std::max<qint64>(0, milliseconds / 1000);
    qint64 hours = totalSeconds / 3600;
    qint64 minutes = (totalSeconds % 3600) / 60;
    qint64 seconds = totalSeconds % 60;

    if (hours > 0)
    {
        return QStringLiteral("%1:%2:%3")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }

    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}
