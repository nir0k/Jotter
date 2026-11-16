#pragma once

#include "core/SftpStorage.h"

#include <QWidget>

#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class SyncConnectionForm;
namespace core {
class LibrarySession;
class SyncScheduler;
} // namespace core
namespace ui {
class Notice;
}

/// Settings → Sync: the SFTP connection as an integration
/// card — its state, the folder, an explanation of any error with what to do, Sync now,
/// Change and Disconnect, or the connection fields with Check and save — and the preferences of
/// when synchronisation runs.
///
/// The page only checks connections; saving them, the credentials and the preferences is left
/// to the application through its signals.
class SyncPage : public QWidget
{
    Q_OBJECT

public:
    explicit SyncPage(QWidget *parent = nullptr);

    /// Shows the open library and its synchronisation; null for no open library.
    ///
    /// @param session The open library.
    /// @param scheduler Its scheduler, which tells the state of synchronisation.
    /// @param connection The library's saved connection, or no value when it has none.
    void setLibrary(core::LibrarySession *session, core::SyncScheduler *scheduler,
                    const std::optional<core::SftpConnection> &connection);

    /// Shows the device preferences of when synchronisation runs.
    void setPreferences(bool afterChanges, int intervalMinutes);

Q_SIGNALS:
    /// Emitted with a connection that passed its check, to be saved for the library.
    void connectRequested(const core::SftpConnection &connection,
                          const core::Credentials &credentials);
    /// Emitted when the person confirmed Disconnect.
    void disconnectRequested();
    void afterChangesChosen(bool on);
    void intervalChosen(int minutes);

protected:
    void changeEvent(QEvent *event) override;

private:
    void retranslate();
    /// Shows the scheduler's state: the status line, the notice and the buttons.
    void update();
    /// Shows the connection fields, filled with the saved connection when there is one.
    void editConnection();
    void checkAndSave();
    void disconnectLibrary();
    /// Acts on the notice's button: Retry, or trusting the server's key after confirming it.
    void noticeAction();

    core::LibrarySession *m_session = nullptr;
    core::SyncScheduler *m_scheduler = nullptr;
    std::optional<core::SftpConnection> m_connection;

    QLabel *m_cardTitle;
    QLabel *m_statusIcon;
    QLabel *m_status;
    QLabel *m_intro;
    QLabel *m_folder;
    ui::Notice *m_notice;
    QWidget *m_actions;
    QPushButton *m_syncNow;
    QPushButton *m_change;
    QPushButton *m_disconnect;
    SyncConnectionForm *m_form;
    QWidget *m_formActions;
    QPushButton *m_save;
    QPushButton *m_cancel;

    QLabel *m_optionsTitle;
    QCheckBox *m_afterChanges;
    QLabel *m_intervalTitle;
    QComboBox *m_interval;
    QLabel *m_optionsHint;
};
