#include <QApplication>
#include <QCoreApplication>
#include <QTimer>

#include "QtClientWindow.h"

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SyncCinema"));
    QCoreApplication::setApplicationName(QStringLiteral("SyncCinema"));
    QApplication::setStyle(QStringLiteral("Fusion"));

    QtClientWindow window;

    // 仅用于自动化布局检查，例如 "820x560"。正常启动时不会设置它。
    QString requestedSize =
        qEnvironmentVariable("SYNCCINEMA_UI_SIZE");
    QStringList sizeParts = requestedSize.split(QLatin1Char('x'));
    if (sizeParts.size() == 2)
    {
        bool widthValid = false;
        bool heightValid = false;
        int width = sizeParts[0].toInt(&widthValid);
        int height = sizeParts[1].toInt(&heightValid);
        if (widthValid && heightValid)
        {
            window.resize(width, height);
        }
    }

    window.show();

    // 自动化视觉检查入口。普通用户不会设置该环境变量。
    QString screenshotPath =
        qEnvironmentVariable("SYNCCINEMA_UI_SCREENSHOT");
    if (!screenshotPath.isEmpty())
    {
        QTimer::singleShot(
            500,
            &window,
            [&window, screenshotPath]()
            {
                window.grab().save(screenshotPath);
                QCoreApplication::quit();
            }
        );
    }

    return application.exec();
}
