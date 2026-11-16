#pragma once

#include "core/Error.h"

#include <QBuffer>
#include <QByteArray>
#include <QList>
#include <QString>

#include <functional>

namespace core {

/// A regular file or a folder in a sync storage.
struct RemoteEntry
{
    QString name;
    bool folder = false;
    qint64 size = 0;
    /// Modification time in milliseconds since the epoch, at the precision of the storage.
    qint64 modified = 0;
};

/// A sync storage: a remote folder holding a copy of the library. Every sync
/// storage implements this interface; the sync engine knows no other.
///
/// Paths are relative to the remote folder, with `/` between their parts; the empty path is the
/// remote folder itself. An implementation is used by one thread at a time.
class RemoteStorage
{
public:
    virtual ~RemoteStorage() = default;

    /// Installs a cooperative lease check, called between bounded listing and transfer batches.
    void setCheckpoint(std::function<Result<void>()> checkpoint)
    {
        m_checkpoint = std::move(checkpoint);
    }

    /// Runs the lease check on this storage's owning thread, suppressing recursive checks while
    /// the callback reads or renews the lock through the same connection.
    Result<void> checkpoint()
    {
        if (!m_checkpoint || m_checking) {
            return {};
        }
        m_checking = true;
        auto result = m_checkpoint();
        m_checking = false;
        return result;
    }

    /// Describes the remote folder for messages, such as `user@host:/path`.
    virtual QString location() const = 0;

    /// Returns how precise modification times are, in milliseconds: changes this close to each
    /// other may carry the same time. A sync run waits this long after its last change before
    /// it releases the lock, so later changes carry later times.
    virtual int timePrecisionMs() const = 0;

    /// Lists the regular files and folders of a folder; other kinds of entries are left out.
    ///
    /// @param path The folder.
    /// @return The entries in no particular order, or NotFound for a missing folder.
    virtual Result<QList<RemoteEntry>> list(const QString &path) = 0;

    /// Reads a whole file into memory, for small files such as notes and the lock.
    ///
    /// @param path The file.
    /// @return The contents, or NotFound.
    Result<QByteArray> read(const QString &path)
    {
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly);
        if (auto copied = readTo(path, buffer); !copied) {
            return std::unexpected(copied.error());
        }
        return buffer.data();
    }

    /// Writes a small file from memory; see writeFrom().
    ///
    /// @param path The file.
    /// @param data The new contents.
    /// @return The written file as a listing shows it.
    Result<RemoteEntry> write(const QString &path, const QByteArray &data)
    {
        QBuffer buffer;
        buffer.setData(data);
        buffer.open(QIODevice::ReadOnly);
        return writeFrom(path, buffer);
    }

    /// Reads a file into an open device a part at a time, so a large file never sits in memory
    /// whole.
    ///
    /// @param path The file.
    /// @param target The open device the contents are written to.
    /// @return Nothing, NotFound, or the error of the storage or of writing @p target (Io).
    virtual Result<void> readTo(const QString &path, QIODevice &target) = 0;

    /// Writes a file so that readers see the old or the new contents only: the data goes to a
    /// file `.jotter-tmp-<random>` in the same folder, which is then renamed over the target.
    /// The contents are read from an open device a part at a time. The folder must exist.
    ///
    /// @param path The file.
    /// @param source The open device holding the new contents, read to its end.
    /// @return The written file as a listing shows it.
    virtual Result<RemoteEntry> writeFrom(const QString &path, QIODevice &source) = 0;

    /// Creates a file only when it does not exist yet, as the lock needs.
    ///
    /// @param path The file.
    /// @param data The contents.
    /// @return True when the file was created, false when it already existed.
    virtual Result<bool> create(const QString &path, const QByteArray &data) = 0;

    /// Renames a file, replacing an existing target.
    ///
    /// @return Nothing, or NotFound when @p from is missing.
    virtual Result<void> rename(const QString &from, const QString &to) = 0;

    /// Removes a file; a missing file is not an error.
    virtual Result<void> remove(const QString &path) = 0;

    /// Removes a folder with the files in it; a missing folder is not an error.
    virtual Result<void> removeFolder(const QString &path) = 0;

    /// Creates a folder and its missing parents; an existing folder is not an error.
    virtual Result<void> makeFolder(const QString &path) = 0;

private:
    std::function<Result<void>()> m_checkpoint;
    bool m_checking = false;
};

} // namespace core
