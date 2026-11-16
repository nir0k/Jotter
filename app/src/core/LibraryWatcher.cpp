#include "core/LibraryWatcher.h"

#include "core/Directory.h"
#include "core/Files.h"

#include <QDir>
#include <QFileInfo>
#include <QtConcurrentRun>

#include <sys/stat.h>

namespace core {
namespace {

/// Files and attachment folders watched one by one at most. Beyond this, a periodic snapshot
/// covers what is not watched.
constexpr qsizetype MaxWatches = 512;

/// How often the snapshot runs while not everything is watched or the library is missing.
constexpr int PollMs = 10 * 1000;

/// Changes are reported once they have settled for this long.
constexpr int SettleMs = 300;

/// Returns the inode of a directory, or an empty string when it is missing or not a directory.
QString directoryInode(const QString &path)
{
    struct stat status{};
    if (::lstat(QFile::encodeName(path).constData(), &status) != 0 || !S_ISDIR(status.st_mode)) {
        return {};
    }
    return QString::number(status.st_ino);
}

/// Captures nanosecond timestamps and inode identity without reading note bodies, and lists
/// the attachment folders with their inodes.
Result<LibraryWatcher::Snapshot> snapshot(const QString &root)
{
    LibraryWatcher::Snapshot result;
    QMap<QString, QString> &files = result.files;
    QStringList folders{root, root + QStringLiteral("/notes")};
    const QString attachmentRoot = root + QStringLiteral("/attachments");
    auto attachments = listDirectory(attachmentRoot);
    if (!attachments) {
        return std::unexpected(attachments.error());
    }
    for (const QString &id : *attachments) {
        const QFileInfo folder(attachmentRoot + u'/' + id);
        if (isValidId(id) && folder.isDir() && !folder.isSymLink()) {
            folders.append(folder.absoluteFilePath());
            result.attachmentFolders.insert(folder.absoluteFilePath(),
                                            directoryInode(folder.absoluteFilePath()));
        }
    }
    for (const QString &folder : std::as_const(folders)) {
        auto names = listDirectory(folder);
        if (!names) {
            return std::unexpected(names.error());
        }
        for (const QString &name : *names) {
            const QString path = folder + u'/' + name;
            struct stat status{};
            if (::lstat(QFile::encodeName(path).constData(), &status) != 0) {
                return fail(ErrorCode::Io, path);
            }
            if (S_ISREG(status.st_mode)) {
                files.insert(path, QStringLiteral("%1:%2:%3:%4:%5:%6")
                                       .arg(status.st_ino)
                                       .arg(status.st_size)
                                       .arg(status.st_mtim.tv_sec)
                                       .arg(status.st_mtim.tv_nsec)
                                       .arg(status.st_ctim.tv_sec)
                                       .arg(status.st_ctim.tv_nsec));
            }
        }
    }
    return result;
}

} // namespace

LibraryWatcher::LibraryWatcher(QString root, QObject *parent)
    : QObject(parent)
    , m_root(std::move(root))
{
    m_settle.setSingleShot(true);
    m_settle.setInterval(SettleMs);
    // A notification only starts a snapshot; changed() follows when the snapshot differs, so a
    // change is reported once.
    connect(&m_settle, &QTimer::timeout, this, &LibraryWatcher::refresh);
    const auto restart = [this] { m_settle.start(); };
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, restart);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, restart);
    connect(&m_scan, &QFutureWatcherBase::finished, this, &LibraryWatcher::snapshotTaken);
    m_poll.setInterval(PollMs);
    connect(&m_poll, &QTimer::timeout, this, &LibraryWatcher::refresh);
    refresh();
}

LibraryWatcher::~LibraryWatcher()
{
    m_scan.waitForFinished();
}

QStringList LibraryWatcher::folders() const
{
    return {QFileInfo(m_root).absolutePath(), m_root, m_root + QStringLiteral("/notes"),
            m_root + QStringLiteral("/attachments")};
}

void LibraryWatcher::refresh()
{
    // A watch follows its inode: a folder recreated under the same path is watched again.
    for (const QString &path : folders()) {
        const QString inode = directoryInode(path);
        if (inode.isEmpty() || m_inodes.value(path) == inode) {
            continue;
        }
        m_watcher.removePath(path);
        if (m_watcher.addPath(path)) {
            m_inodes.insert(path, inode);
        }
    }
    if (m_scan.isRunning()) {
        // A change during the snapshot may be missing from it.
        m_rescan = true;
        return;
    }
    m_scan.setFuture(QtConcurrent::run([root = m_root] { return snapshot(root); }));
}

void LibraryWatcher::snapshotTaken()
{
    const auto result = m_scan.result();
    const bool rescan = std::exchange(m_rescan, false);
    if (!result) {
        if (!std::exchange(m_failed, true)) {
            Q_EMIT changed();
        }
        // The snapshot is retried until the library is back.
        m_poll.start();
    } else {
        if (std::exchange(m_failed, false) || result->files != m_snapshot) {
            m_snapshot = result->files;
            Q_EMIT changed();
        }
        watch(*result);
    }
    if (rescan) {
        refresh();
    }
}

void LibraryWatcher::watch(const Snapshot &snapshot)
{
    // Attachment folders come first, so new files in them are noticed; files go up to the cap.
    // Each wanted path carries its inode: a file replaced by a rename, as every save does, or a
    // recreated folder is watched again, as the old watch follows the old inode.
    QHash<QString, QString> wanted;
    for (auto it = snapshot.attachmentFolders.cbegin();
         it != snapshot.attachmentFolders.cend() && wanted.size() < MaxWatches; ++it) {
        wanted.insert(it.key(), it.value());
    }
    for (auto it = snapshot.files.cbegin();
         it != snapshot.files.cend() && wanted.size() < MaxWatches; ++it) {
        wanted.insert(it.key(), it.value().section(u':', 0, 0));
    }
    const QStringList fixed = folders();
    QStringList stale;
    for (const QString &path : m_watcher.files() + m_watcher.directories()) {
        if (fixed.contains(path)) {
            continue;
        }
        const auto inode = wanted.constFind(path);
        if (inode == wanted.cend() || *inode != m_inodes.value(path)) {
            stale.append(path);
            m_inodes.remove(path);
        }
    }
    if (!stale.isEmpty()) {
        m_watcher.removePaths(stale);
    }
    const QStringList current = m_watcher.files() + m_watcher.directories();
    const QSet<QString> watched(current.cbegin(), current.cend());
    for (auto it = wanted.cbegin(); it != wanted.cend(); ++it) {
        if (!watched.contains(it.key()) && m_watcher.addPath(it.key())) {
            m_inodes.insert(it.key(), it.value());
        }
    }
    if (snapshot.attachmentFolders.size() + snapshot.files.size() > MaxWatches) {
        m_poll.start();
    } else {
        m_poll.stop();
    }
}

} // namespace core
