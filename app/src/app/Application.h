#pragma once

#include "app/Settings.h"
#include "app/SingleInstance.h"
#include "core/SftpStorage.h"
#include "ui/Theme.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QTranslator>

#include <memory>

namespace core {
class LibrarySession;
class SyncScheduler;
} // namespace core
class MainWindow;
class QMenu;
class QSystemTrayIcon;
class CaptureWindow;
class FirstStartDialog;
class SettingsDialog;

/// The running application: command line, single instance, language, theme, the open library
/// and its windows.
class Application : public QObject
{
    Q_OBJECT

public:
    Application();
    ~Application() override;

    /// Handles the command line and opens the first window.
    ///
    /// @return The exit code for main(): a positive value ends the process before the event
    ///         loop starts, a negative one means the event loop should run.
    int start();

private:
    /// Installs the translations of @p language (`en`, `ru`) and makes it the locale of dates.
    void applyLanguage(const QString &language);

    /// Opens the library at @p path and shows it in the main window.
    ///
    /// @return An error message, or empty on success.
    QString openLibrary(const QString &path);

    /// Shows the first-start dialog until a library is open; returns false when it was closed.
    bool runFirstStart(const QString &problem);

    void showSettings();
    /// Starts the synchronisation of the open library with its saved connection and secret.
    void startSync();
    /// Shows the scheduler's state in the main window and in Settings → Sync.
    void updateSyncStatus();
    /// Connects Settings → Sync to the open library: saves checked connections with their
    /// secrets, disconnects and keeps the device preferences.
    void connectSyncPage();
    /// Saves a checked connection and its credentials for the open library and synchronises
    /// with it; credentials that cannot be stored save nothing.
    void saveConnection(const core::SftpConnection &connection,
                        const core::Credentials &credentials);
    /// Removes the open library's connection, secret and sync state; the files stay.
    void disconnectLibrary();
    /// Opens the new note window with a new note, or raises the open one.
    /// Without the main window shown, Jotter quits when the window closes.
    void showCapture(const QByteArray &activationToken);
    /// Makes @p pixels the root size of the interface: the application font and every size
    /// measured from it.
    void applyInterfaceSize(int pixels);
    void activate(const QByteArray &activationToken);
    /// Shows the system tray icon when @p on, or removes it.
    void setTrayIcon(bool on);
    /// Fills the tray icon's menu in the interface language.
    void fillTrayMenu();
    /// Returns the tray icon in the chosen look: the application icon, or its one-colour drawing
    /// in the text colour of the system's light or dark scheme, so it shows on the panel.
    QIcon currentTrayIcon() const;
    /// Draws the shown tray icon again, after its look or the colour scheme changed.
    void updateTrayIcon();

    Settings m_settings;
    ui::Theme m_theme;
    SingleInstance m_instance;
    QTranslator m_qtTranslator;
    QTranslator m_appTranslator;
    QString m_language;
    std::unique_ptr<core::LibrarySession> m_session;
    // The scheduler uses the session, so it is declared after it and destroyed first.
    std::unique_ptr<core::SyncScheduler> m_scheduler;
    /// Refreshes the "how long ago" of the sync status.
    QTimer m_syncClock;
    std::unique_ptr<MainWindow> m_window;
    QPointer<SettingsDialog> m_settingsDialog;
    QPointer<CaptureWindow> m_capture;
    /// The first start while it runs: until a library is open, activation brings it to the front.
    QPointer<FirstStartDialog> m_firstStart;
    // The tray icon refers to its menu, so the menu is declared first and destroyed last.
    std::unique_ptr<QMenu> m_trayMenu;
    std::unique_ptr<QSystemTrayIcon> m_tray;
};
