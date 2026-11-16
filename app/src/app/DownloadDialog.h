#pragma once

#include "core/SftpStorage.h"
#include "core/SyncEngine.h"

#include <QDialog>
#include <QFutureWatcher>

#include <optional>

class QLabel;
class QLineEdit;
class QPushButton;
class SyncConnectionForm;
namespace ui {
class Notice;
}

/// A library downloaded from a sync storage, ready to be opened already connected.
struct DownloadedLibrary
{
    /// The library folder on this device.
    QString root;
    QString libraryId;
    core::SftpConnection connection;
    /// The password, or the key and its passphrase, to be stored with the connection.
    core::Credentials credentials;
};

/// Download from SFTP on the first start: the connection, checked as in
/// Settings, and a local folder, suggested as for a new library, that is empty or missing. The
/// library is downloaded into it with a sync run, so it opens already connected.
class DownloadDialog : public QDialog
{
    Q_OBJECT

public:
    explicit DownloadDialog(QWidget *parent = nullptr);
    ~DownloadDialog() override;

    /// The downloaded library, valid after the dialog was accepted.
    const std::optional<DownloadedLibrary> &library() const { return m_result; }

    /// Closes the dialog unless the connection is checked or the library downloads, as
    /// Escape must not interrupt them.
    void reject() override;

protected:
    void changeEvent(QEvent *event) override;

private:
    void retranslate();
    void startDownload();
    /// Downloads the library through a checked connection on a worker thread.
    void download(const std::shared_ptr<core::SftpStorage> &storage);
    void downloaded();
    void setBusy(bool busy);

    QLabel *m_title;
    QLabel *m_text;
    SyncConnectionForm *m_form;
    QLabel *m_locationLabel;
    QLineEdit *m_location;
    QPushButton *m_browse;
    QLabel *m_progress;
    ui::Notice *m_notice;
    QPushButton *m_cancel;
    QPushButton *m_download;
    bool m_busy = false;
    core::Credentials m_credentials;
    QFutureWatcher<core::Result<QString>> m_run;
    std::optional<DownloadedLibrary> m_result;
};
