#include "app/Application.h"

#include <QApplication>
#include <QIcon>

/// Starts Jotter: sets the application's identity and hands over to Application, which handles
/// the command line, a running instance and the first window.
int main(int argc, char *argv[])
{
    // File choosers are the desktop's own: outside KDE, whose platform theme
    // has native dialogs, Qt takes them from the XDG desktop portal, along with the light or dark
    // preference that the Auto mode follows. Without a running portal Qt falls back to its own
    // dialog, and a theme the person set is kept.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORMTHEME") &&
        !qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains(u"KDE", Qt::CaseInsensitive)) {
        qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
    }
    const QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("jotter"));
    QApplication::setApplicationDisplayName(QStringLiteral("Jotter"));
    QApplication::setApplicationVersion(QStringLiteral(JOTTER_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("io.github.nir0k.Jotter"));
    QApplication::setWindowIcon(
        QIcon::fromTheme(QStringLiteral("io.github.nir0k.Jotter"),
                         QIcon(QStringLiteral(":/app/io.github.nir0k.Jotter-256.png"))));

    Application application;
    const int code = application.start();
    return code >= 0 ? code : QApplication::exec();
}
