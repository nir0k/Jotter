#pragma once

#include "core/SftpStorage.h"

#include <QFutureWatcher>
#include <QWidget>

#include <functional>
#include <memory>

class QLabel;
class QLineEdit;
class QPushButton;
namespace ui {
class Notice;
class SegmentedControl;
} // namespace ui

/// Asks the person whether to trust a server's host key: an unknown key
/// shows its type and fingerprint, a changed one the recorded and the new fingerprint, with a
/// warning; trusting a changed key is confirmed like a destructive action.
///
/// @param parent The window to centre on.
/// @param location The server and folder, as the storage describes them.
/// @param key The server's key.
/// @return true when the person trusts the key.
bool confirmHostKey(QWidget *parent, const QString &location, const core::HostKey &key);

/// The fields of an SFTP connection and its check: host, port, user,
/// remote folder, sign-in method, the key imported from a file and the password or passphrase,
/// neither of which is ever prefilled. Checking connects, asks the person about an unknown or
/// changed host key, signs in, writes and removes a probe file and runs the caller's own check;
/// Browse signs in the same way and lets the person choose or create the folder on the server. The
/// network work runs on a worker thread, and a failure is explained under the fields.
class SyncConnectionForm : public QWidget
{
    Q_OBJECT

public:
    /// A check of the caller run on the worker thread after the probe, such as whether the
    /// folder holds the right library.
    using Check = std::function<core::Result<void>(core::SftpStorage &)>;

    explicit SyncConnectionForm(QWidget *parent = nullptr);
    ~SyncConnectionForm() override;

    /// Returns the connection the fields describe.
    core::SftpConnection connection() const;

    /// Fills the fields with a saved connection, to change it; the key and the secret are not
    /// filled in and must be given again.
    void setConnection(const core::SftpConnection &connection);

    /// Returns the password, or the imported key and its passphrase, as the method needs.
    core::Credentials credentials() const;

    /// Starts checking the connection; checked() follows on success.
    ///
    /// @param check The caller's own check, or none.
    void check(Check check = {});

    /// Shows a failure under the fields, such as one of the caller's next step.
    void showError(const core::Error &error);

    /// Enables or disables the fields, as while the caller works with a checked connection.
    void setFieldsEnabled(bool enabled);

Q_SIGNALS:
    /// Emitted when the connection passed every step, with the signed-in storage.
    void checked(std::shared_ptr<core::SftpStorage> storage);

    /// Emitted when a check starts or ends.
    void checkingChanged(bool checking);

protected:
    void changeEvent(QEvent *event) override;

private:
    void retranslate();
    /// Shows the key and secret fields the sign-in method needs.
    void updateMethod();
    /// Imports the private key from a file the person chooses. The file must be small and
    /// hold a private key in OpenSSH or PEM form; its passphrase is checked on sign-in.
    void chooseKey();
    void setChecking(bool checking);
    /// Connects to the server on a worker thread, for a check or for choosing the folder. Key
    /// sign-in without an imported key asks for one instead of connecting.
    ///
    /// @param browse Whether the connection is for choosing the folder: it then signs in
    ///        without a probe and opens the folder dialog.
    /// @param check The caller's own check, for a check of the connection.
    void start(bool browse, Check check);
    /// Opens the folder on the server in a dialog to choose from (Browse).
    void browse();
    /// Continues after connecting: asks about the host key, then signs in and probes, or
    /// finds the folder to show first.
    void connected();
    void finished();

    QLabel *m_hostLabel;
    QLineEdit *m_host;
    QLabel *m_portLabel;
    QLineEdit *m_port;
    QLabel *m_userLabel;
    QLineEdit *m_user;
    QLabel *m_folderLabel;
    QWidget *m_folderRow;
    QLineEdit *m_folder;
    QPushButton *m_browseFolder;
    QLabel *m_methodLabel;
    ui::SegmentedControl *m_method;
    QLabel *m_keyLabel;
    QWidget *m_keyRow;
    QLabel *m_keyState;
    QPushButton *m_chooseKey;
    QLabel *m_secretLabel;
    QLineEdit *m_secret;
    QLabel *m_hint;
    ui::Notice *m_notice;
    /// The text of the imported private key and the name of the file it came from; both are
    /// cleared once the connection is checked.
    QString m_key;
    QString m_keyName;
    /// Whether the running connection is for choosing the folder.
    bool m_browsing = false;
    Check m_check;
    QFutureWatcher<core::Result<std::shared_ptr<core::SftpStorage>>> m_connect;
    /// Ends with the folder to show first when browsing, and an empty string after a check.
    QFutureWatcher<core::Result<QString>> m_finish;
    std::shared_ptr<core::SftpStorage> m_storage;
};
