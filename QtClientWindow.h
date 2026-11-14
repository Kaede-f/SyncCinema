#pragma once

#include <QMainWindow>

#include "Protocol.h"

class QLabel;
class QLineEdit;
class QKeyEvent;
class QPushButton;
class QSlider;
class QTimer;
class QToolButton;
class QWidget;

class QtClientController;

class QtClientWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit QtClientWindow(QWidget* parent = nullptr);
    ~QtClientWindow() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void buildUi();
    void applyStyle();
    void connectSignals();
    void loadSettings();
    void saveSettings() const;

    void chooseLocalMedia();
    void toggleConnection();
    void togglePlayback();
    void toggleFullScreenMode();
    void updateProgress();
    void updateTimeLabel(qint64 positionMs, qint64 durationMs);
    void setConnectedUi(bool connected);
    void setBusyUi(bool busy, const QString& message);
    void setMediaStatusUi(int playerEventType, int bufferingPercent);
    QString formatTime(qint64 milliseconds) const;

    QtClientController* controller_ = nullptr;

    QWidget* topBar_ = nullptr;
    QLineEdit* mediaSourceEdit_ = nullptr;
    QLineEdit* serverHostEdit_ = nullptr;
    QToolButton* browseButton_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QLabel* statusIndicator_ = nullptr;
    QLabel* statusText_ = nullptr;

    QWidget* videoSurface_ = nullptr;
    QLabel* videoStatusLabel_ = nullptr;

    QWidget* controlBar_ = nullptr;
    QToolButton* playPauseButton_ = nullptr;
    QSlider* progressSlider_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QToolButton* volumeButton_ = nullptr;
    QSlider* volumeSlider_ = nullptr;
    QToolButton* fullScreenButton_ = nullptr;

    QTimer* progressTimer_ = nullptr;
    PlaybackState playbackState_ = PlaybackState::Stopped;
    bool connected_ = false;
    bool busy_ = false;
    bool mediaStatusVisible_ = false;
};
