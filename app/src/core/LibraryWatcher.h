#pragma once

#include "core/Error.h"

#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QTimer>

namespace core {

/// Watches file replacements and in-place edits. A worker snapshot confirms each notification
/// and restores subscriptions after recreation; it also runs periodically while the library has
/// more files than are watched one by one, or while the library is missing.
class LibraryWatcher : public QObject
{
    Q_OBJECT

public:
    /// What a snapshot of the library found.
    struct Snapshot
    {
        /// The identity, size and times of every regular file, by path.
        QMap<QString, QString> files;
        /// The inode of every attachment folder, by path.
        QMap<QString, QString> attachmentFolders;
    };

    /// Starts watching a library, including its parent so a missing root may return.
    explicit LibraryWatcher(QString root, QObject *parent = nullptr);
    ~LibraryWatcher() override;

Q_SIGNALS:
    /// Emitted once changes have settled for a short moment.
    void changed();

private:
    /// Refreshes subscriptions and starts a non-overlapping worker snapshot.
    void refresh();
    /// Reports a snapshot that differs from the last one and updates the watches.
    void snapshotTaken();
    /// Watches the attachment folders and files of a snapshot up to the cap and polls beyond it.
    ///
    /// @param snapshot The latest snapshot.
    void watch(const Snapshot &snapshot);
    /// Returns the folders always watched: the parent, the root, `notes` and `attachments`.
    QStringList folders() const;

    QString m_root;
    QFileSystemWatcher m_watcher;
    QTimer m_settle;
    QTimer m_poll;
    QFutureWatcher<Result<Snapshot>> m_scan;
    QMap<QString, QString> m_snapshot;
    /// The inode each watched path had when its watch was added.
    QHash<QString, QString> m_inodes;
    bool m_failed = false;
    /// Whether a notification came while a snapshot was running.
    bool m_rescan = false;
};

} // namespace core
