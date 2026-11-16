#pragma once

#include "core/SftpStorage.h"

#include <QDialog>
#include <QFutureWatcher>

#include <memory>

class QLabel;
class QListWidget;
class QPushButton;
namespace ui {
class Notice;
}

/// Choosing the folder of a connection on the SFTP server: the folders in
/// the current one, Up, a note whether it holds a Jotter library, is empty or holds other files,
/// and New folder. Every request to the server runs on a worker thread.
class RemoteFolderDialog : public QDialog
{
    Q_OBJECT

public:
    /// @param storage A signed-in storage whose folder is the server's root, `/`.
    /// @param start The absolute folder to show first.
    /// @param parent The window to centre on.
    RemoteFolderDialog(std::shared_ptr<core::SftpStorage> storage, const QString &start,
                       QWidget *parent = nullptr);
    ~RemoteFolderDialog() override;

    /// The chosen absolute folder, valid after the dialog was accepted.
    QString folder() const { return m_path; }

protected:
    void changeEvent(QEvent *event) override;

private:
    void retranslate();
    /// Lists @p path on a worker thread and shows it when the listing comes.
    void showFolder(const QString &path);
    /// Asks for a name, creates the folder inside the shown one and opens it.
    void createFolder();
    void listed();
    void setBusy(bool busy);
    /// Shows what the shown folder holds, from its listing.
    void updateContent(const QList<core::RemoteEntry> &entries);

    std::shared_ptr<core::SftpStorage> m_storage;
    /// The folder shown, and the one being listed.
    QString m_path;
    QString m_pending;
    /// The listing of the folder shown.
    QList<core::RemoteEntry> m_entries;
    QLabel *m_pathLabel;
    QPushButton *m_up;
    QListWidget *m_list;
    QLabel *m_content;
    ui::Notice *m_notice;
    QPushButton *m_newFolder;
    QPushButton *m_cancel;
    QPushButton *m_choose;
    QFutureWatcher<core::Result<QList<core::RemoteEntry>>> m_listing;
};
