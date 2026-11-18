#include "app/Application.h"

#include "app/CaptureWindow.h"
#include "app/FirstStartDialog.h"
#include "app/MainWindow.h"
#include "app/SettingsDialog.h"
#include "app/SyncPage.h"
#include "app/SyncStatus.h"
#include "core/Credentials.h"
#include "core/Library.h"
#include "core/LibrarySession.h"
#include "core/SyncScheduler.h"
#include "core/SyncState.h"
#include "editor/Presenter.h"
#include "editor/TextEdit.h"
#include "ui/Icons.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QStyleHints>
#include <QSystemTrayIcon>

namespace {

/// The interface language for the system locale: Russian when the system prefers it,
/// otherwise English.
QString systemLanguage()
{
    for (const QString &language : QLocale::system().uiLanguages()) {
        if (language.startsWith(QLatin1StringView("ru"))) {
            return QStringLiteral("ru");
        }
        if (language.startsWith(QLatin1StringView("en"))) {
            return QStringLiteral("en");
        }
    }
    return QStringLiteral("en");
}

} // namespace

Application::Application() = default;

Application::~Application()
{
    // Windows refer to the session, so they go first.
    delete m_capture;
    delete m_settingsDialog;
    m_tray.reset();
    m_window.reset();
    m_scheduler.reset();
    m_session.reset();
}

int Application::start()
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Jotter, a note manager."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption newNote(QStringLiteral("new-note"),
                                     QStringLiteral("Open the new note window."));
    const QCommandLineOption tray(
        QStringLiteral("tray"),
        QStringLiteral("Start in the system tray without opening the main window."));
    parser.addOptions({newNote, tray});
    parser.process(*qApp);
    const bool capture = parser.isSet(newNote);

    if (m_instance.forward(QApplication::arguments())) {
        return 0;
    }
    m_instance.listen();
    // Only this Jotter runs now, so snapshots left by an earlier run belong to no one.
    editor::TextEdit::removeOldSnapshots();
    connect(&m_instance, &SingleInstance::launched, this,
            [this](const QStringList &arguments, const QByteArray &token) {
                QCommandLineParser forwarded;
                const QCommandLineOption option(QStringLiteral("new-note"));
                const QCommandLineOption toTray(QStringLiteral("tray"));
                forwarded.addOptions({option, toTray});
                // Unknown options of the other launch are ignored; it reported them itself.
                const bool parsed = forwarded.parse(arguments);
                if (parsed && forwarded.isSet(option)) {
                    showCapture(token);
                } else if (!parsed || !forwarded.isSet(toTray)) {
                    // A start in the tray, such as at login, leaves a running Jotter as it is.
                    activate(token);
                }
            });

    // With the tray icon shown, a closed window leaves Jotter running in the tray; without it,
    // or on a desktop without a tray, the last closed window quits as usual. The check runs at
    // each close, since a bar's tray may come and go.
    QApplication::setQuitOnLastWindowClosed(false);
    connect(qApp, &QGuiApplication::lastWindowClosed, this, [this] {
        if (!m_tray || !QSystemTrayIcon::isSystemTrayAvailable()) {
            QApplication::quit();
        }
    });

    const QString language = m_settings.language();
    applyLanguage(language.isEmpty() ? systemLanguage() : language);
    m_theme.setPalette(m_settings.palette());
    m_theme.setMode(m_settings.themeMode());
    applyInterfaceSize(m_settings.interfaceSize());
    editor::Presenter::setStrikeChecked(m_settings.strikeCheckedItems());

    m_window = std::make_unique<MainWindow>(m_settings);
    connect(m_window.get(), &MainWindow::settingsRequested, this, &Application::showSettings);
    connect(m_window.get(), &MainWindow::problemsRequested, this, &Application::showProblems);
    connect(m_window.get(), &MainWindow::syncRequested, this, [this] {
        if (m_scheduler && m_scheduler->isConnected()) {
            m_scheduler->syncNow();
        }
    });
    // The footer says how long ago the last sync was; this keeps it true.
    m_syncClock.start(30 * 1000);
    connect(&m_syncClock, &QTimer::timeout, this, &Application::updateSyncStatus);
    setTrayIcon(m_settings.trayIcon());
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            &Application::updateTrayIcon);
    connect(&m_theme, &ui::Theme::changed, this, &Application::updateTrayIcon);

    const QString path = m_settings.libraryPath();
    QString problem;
    // The new note window, or --tray while the tray icon is on, starts Jotter in the background:
    // the main window stays hidden. Whether the desktop has a tray is not checked, as at login the
    // bar showing it may start after Jotter.
    const auto showFirstWindow = [this, capture](bool inTray) {
        if (capture) {
            showCapture({});
        } else if (!inTray) {
            m_window->show();
        }
    };
    if (!path.isEmpty()) {
        problem = openLibrary(path);
        if (problem.isEmpty()) {
            showFirstWindow(parser.isSet(tray) && m_tray);
            return -1;
        }
    }
    // The first start, or a library that failed to open, always shows the window.
    if (!runFirstStart(problem)) {
        return 0;
    }
    showFirstWindow(false);
    return -1;
}

void Application::applyLanguage(const QString &language)
{
    if (language == m_language) {
        return;
    }
    m_language = language;
    QApplication::removeTranslator(&m_qtTranslator);
    QApplication::removeTranslator(&m_appTranslator);
    const QLocale locale(language);
    QLocale::setDefault(locale);
    if (m_qtTranslator.load(locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                            QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QApplication::installTranslator(&m_qtTranslator);
    }
    if (m_appTranslator.load(locale, QStringLiteral("jotter"), QStringLiteral("_"),
                             QStringLiteral(":/i18n"))) {
        QApplication::installTranslator(&m_appTranslator);
    }
    fillTrayMenu();
}

QString Application::openLibrary(const QString &path)
{
    auto session = core::LibrarySession::open(path);
    if (!session) {
        return core::describe(session.error());
    }
    if (!m_window->savePending() || (m_capture && !m_capture->savePending())) {
        return tr("The note could not be saved. Retry before switching libraries.");
    }
    if (m_capture) {
        // Close and detach before releasing the session; this also discards an empty new note.
        if (!m_capture->close()) {
            return tr("The note could not be saved. Retry before switching libraries.");
        }
        delete m_capture.data();
    }
    if (!m_window->setSession(nullptr)) {
        return tr("The note could not be saved. Retry before switching libraries.");
    }
    if (m_settingsDialog) {
        m_settingsDialog->setSession(nullptr);
        m_settingsDialog->syncPage()->setLibrary(nullptr, nullptr, std::nullopt);
    }
    m_scheduler.reset();
    m_session = std::move(*session);
    m_settings.setLibraryPath(m_session->library().root());
    m_window->setSession(m_session.get());
    if (m_settingsDialog) {
        m_settingsDialog->setSession(m_session.get());
    }
    startSync();
    return {};
}

bool Application::runFirstStart(const QString &problem)
{
    FirstStartDialog dialog(m_language);
    m_firstStart = &dialog;
    if (!problem.isEmpty()) {
        dialog.setProblem(problem);
    }
    connect(&dialog, &FirstStartDialog::languageChosen, this, [this](const QString &language) {
        m_settings.setLanguage(language);
        applyLanguage(language);
    });
    while (dialog.exec() == QDialog::Accepted) {
        // A downloaded library opens already connected.
        if (const auto &download = dialog.download()) {
            if (auto stored = core::writeCredentials(download->libraryId, download->credentials);
                !stored) {
                dialog.setProblem(core::describe(stored.error()));
                continue;
            }
            m_settings.setSyncConnection(download->libraryId, download->connection);
            m_settings.setLastSynced(download->libraryId, QDateTime::currentDateTimeUtc());
        }
        const QString error = openLibrary(dialog.libraryPath());
        if (error.isEmpty()) {
            return true;
        }
        dialog.setProblem(error);
    }
    return false;
}

void Application::showSettings()
{
    if (!m_settingsDialog) {
        m_settingsDialog =
            new SettingsDialog(m_theme.palette(), m_theme.mode(), m_language, m_window.get());
        m_settingsDialog->setAttribute(Qt::WA_DeleteOnClose);
        m_settingsDialog->setSession(m_session.get());
        connectSyncPage();
        connect(m_settingsDialog, &SettingsDialog::paletteChosen, this,
                [this](ui::PaletteId palette) {
                    m_settings.setPalette(palette);
                    m_theme.setPalette(palette);
                });
        m_settingsDialog->setTrayIcon(m_settings.trayIcon(), m_settings.monochromeTrayIcon());
        connect(m_settingsDialog, &SettingsDialog::trayIconChosen, this, [this](bool on) {
            m_settings.setTrayIcon(on);
            setTrayIcon(on);
        });
        connect(m_settingsDialog, &SettingsDialog::monochromeTrayIconChosen, this, [this](bool on) {
            m_settings.setMonochromeTrayIcon(on);
            updateTrayIcon();
        });
        m_settingsDialog->setInterfaceSize(m_settings.interfaceSize());
        m_settingsDialog->setAddLocation(m_settings.addLocation());
        connect(m_settingsDialog, &SettingsDialog::addLocationChosen, this, [this](bool on) {
            m_settings.setAddLocation(on);
            m_window->setLocationsEnabled(on);
        });
        m_settingsDialog->setStrikeCheckedItems(m_settings.strikeCheckedItems());
        connect(m_settingsDialog, &SettingsDialog::strikeCheckedItemsChosen, this, [this](bool on) {
            m_settings.setStrikeCheckedItems(on);
            editor::Presenter::setStrikeChecked(on);
        });
        connect(m_settingsDialog, &SettingsDialog::interfaceSizeChosen, this, [this](int pixels) {
            m_settings.setInterfaceSize(pixels);
            applyInterfaceSize(pixels);
        });
        connect(m_settingsDialog, &SettingsDialog::modeChosen, this, [this](ui::ThemeMode mode) {
            m_settings.setThemeMode(mode);
            m_theme.setMode(mode);
        });
        connect(m_settingsDialog, &SettingsDialog::languageChosen, this,
                [this](const QString &language) {
                    m_settings.setLanguage(language);
                    applyLanguage(language);
                });
        connect(m_settingsDialog, &SettingsDialog::libraryChosen, this,
                [this](const QString &path) {
                    if (const QString error = openLibrary(path); !error.isEmpty()) {
                        qWarning("Cannot open library %s: %s", qPrintable(path), qPrintable(error));
                    }
                });
    }
    m_settingsDialog->show();
    m_settingsDialog->raise();
    m_settingsDialog->activateWindow();
}

void Application::showProblems()
{
    showSettings();
    m_settingsDialog->showSection(SettingsDialog::Section::Library);
}

void Application::startSync()
{
    const QString id = m_session->library().info().id;
    m_scheduler = std::make_unique<core::SyncScheduler>(*m_session);
    m_scheduler->setAfterChanges(m_settings.syncAfterChanges());
    m_scheduler->setInterval(m_settings.syncInterval());
    m_scheduler->setLastSynced(m_settings.lastSynced(id));
    connect(m_scheduler.get(), &core::SyncScheduler::changed, this, &Application::updateSyncStatus);
    connect(m_scheduler.get(), &core::SyncScheduler::synced, this,
            [this, id](const core::SyncReport &report) {
                m_settings.setLastSynced(id, m_scheduler->lastSynced());
                m_window->announceConflicts(report);
            });
    const auto connection = m_settings.syncConnection(id);
    core::Credentials credentials;
    if (connection) {
        auto stored = core::readCredentials(id);
        if (!stored) {
            qWarning("Cannot read the sync credentials: %s", qPrintable(stored.error().detail));
        }
        credentials = stored.value_or(core::Credentials());
    }
    m_scheduler->setConnection(connection, credentials);
    if (m_settingsDialog) {
        m_settingsDialog->syncPage()->setLibrary(m_session.get(), m_scheduler.get(), connection);
    }
    updateSyncStatus();
}

void Application::updateSyncStatus()
{
    if (!m_scheduler) {
        m_window->setSyncStatus({}, {});
        return;
    }
    const SyncStatus status = syncStatus(*m_scheduler);
    const auto &error = m_scheduler->error();
    m_window->setSyncStatus(status,
                            status.failed && error ? core::describe(*error) : tr("Sync now (F5)"));
}

void Application::connectSyncPage()
{
    SyncPage *page = m_settingsDialog->syncPage();
    page->setPreferences(m_settings.syncAfterChanges(), m_settings.syncInterval());
    if (m_session) {
        page->setLibrary(m_session.get(), m_scheduler.get(),
                         m_settings.syncConnection(m_session->library().info().id));
    }
    connect(page, &SyncPage::connectRequested, this, &Application::saveConnection);
    connect(page, &SyncPage::disconnectRequested, this, &Application::disconnectLibrary);
    connect(page, &SyncPage::afterChangesChosen, this, [this](bool on) {
        m_settings.setSyncAfterChanges(on);
        if (m_scheduler) {
            m_scheduler->setAfterChanges(on);
        }
    });
    connect(page, &SyncPage::intervalChosen, this, [this](int minutes) {
        m_settings.setSyncInterval(minutes);
        if (m_scheduler) {
            m_scheduler->setInterval(minutes);
        }
    });
}

void Application::saveConnection(const core::SftpConnection &connection,
                                 const core::Credentials &credentials)
{
    if (!m_session || !m_scheduler) {
        return;
    }
    const QString id = m_session->library().info().id;
    if (auto stored = core::writeCredentials(id, credentials); !stored) {
        QMessageBox::warning(m_settingsDialog, tr("Sync"), core::describe(stored.error()));
        return;
    }
    m_settings.setSyncConnection(id, connection);
    m_scheduler->setConnection(connection, credentials);
    if (m_settingsDialog) {
        m_settingsDialog->syncPage()->setLibrary(m_session.get(), m_scheduler.get(), connection);
    }
}

void Application::disconnectLibrary()
{
    if (!m_session || !m_scheduler) {
        return;
    }
    const QString id = m_session->library().info().id;
    m_scheduler->setConnection(std::nullopt, {});
    m_settings.setSyncConnection(id, std::nullopt);
    m_settings.setLastSynced(id, {});
    m_scheduler->setLastSynced({});
    if (auto removed = core::writeCredentials(id, {}); !removed) {
        qWarning("Cannot remove the sync credentials: %s", qPrintable(removed.error().detail));
    }
    // A state left behind is harmless: connecting an empty storage clears it, and the same
    // library's storage still matches it.
    const QString stateFolder = QFileInfo(core::SyncState::defaultPath(id)).absolutePath();
    if (!QDir(stateFolder).removeRecursively()) {
        qWarning("Cannot remove the sync state in %s", qPrintable(stateFolder));
    }
    if (m_settingsDialog) {
        m_settingsDialog->syncPage()->setLibrary(m_session.get(), m_scheduler.get(), std::nullopt);
    }
    updateSyncStatus();
}

void Application::showCapture(const QByteArray &activationToken)
{
    if (m_session == nullptr) {
        activate(activationToken);
        return;
    }
    if (!activationToken.isEmpty()) {
        qputenv("XDG_ACTIVATION_TOKEN", activationToken);
    }
    if (m_capture) {
        m_capture->raise();
        m_capture->activateWindow();
        return;
    }
    m_capture = new CaptureWindow(*m_session);
    m_capture->setAttribute(Qt::WA_DeleteOnClose);
    if (!m_capture->start()) {
        m_capture->deleteLater();
        return;
    }
    m_capture->show();
    m_capture->activateWindow();
    m_window->locateNewNote(m_capture->noteId());
}

void Application::applyInterfaceSize(int pixels)
{
    QFont font = QApplication::font();
    font.setPixelSize(pixels);
    QApplication::setFont(font);
    m_theme.apply();
}

void Application::activate(const QByteArray &activationToken)
{
    // On Wayland a window may take the focus only with the token of the launch that asked.
    if (!activationToken.isEmpty()) {
        qputenv("XDG_ACTIVATION_TOKEN", activationToken);
    }
    // During the first start no library is open, so the main window would be empty: the first
    // start comes to the front instead, or the dialog open over it, such as Download from SFTP.
    QWidget *window = m_window.get();
    if (m_firstStart) {
        window = QApplication::activeModalWidget();
        if (window == nullptr) {
            window = m_firstStart;
        }
    }
    window->show();
    window->raise();
    window->activateWindow();
}

void Application::setTrayIcon(bool on)
{
    if (!on) {
        m_tray.reset();
        m_trayMenu.reset();
        return;
    }
    if (m_tray) {
        return;
    }
    m_trayMenu = std::make_unique<QMenu>();
    fillTrayMenu();
    m_tray = std::make_unique<QSystemTrayIcon>(currentTrayIcon());
    m_tray->setToolTip(QApplication::applicationDisplayName());
    m_tray->setContextMenu(m_trayMenu.get());
    connect(m_tray.get(), &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger) {
                    activate({});
                }
            });
    m_tray->show();
}

QIcon Application::currentTrayIcon() const
{
    if (!m_settings.monochromeTrayIcon()) {
        return QApplication::windowIcon();
    }
    // The panel follows the system's scheme rather than Jotter's mode; when the system tells
    // none, Jotter's mode decides.
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    const bool dark =
        scheme == Qt::ColorScheme::Unknown ? m_theme.roles().dark : scheme == Qt::ColorScheme::Dark;
    return ui::monochromeIcon(QStringLiteral(":/app/io.github.nir0k.Jotter-symbolic.svg"),
                              ui::roles(m_theme.palette(), dark).onSurface);
}

void Application::updateTrayIcon()
{
    if (m_tray) {
        m_tray->setIcon(currentTrayIcon());
    }
}

void Application::fillTrayMenu()
{
    if (!m_trayMenu) {
        return;
    }
    m_trayMenu->clear();
    m_trayMenu->addAction(tr("New note"), this, [this] { showCapture({}); });
    m_trayMenu->addAction(tr("Open Jotter"), this, [this] { activate({}); });
    m_trayMenu->addSeparator();
    // Closing the windows first saves the open notes and the window state, and discards an
    // empty new note.
    m_trayMenu->addAction(tr("Quit"), this, [this] {
        if (!m_window->savePending() || (m_capture && !m_capture->savePending())) {
            return;
        }
        QApplication::closeAllWindows();
        QApplication::quit();
    });
}
