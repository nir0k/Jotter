#pragma once

#include "core/Error.h"

#include <QByteArray>
#include <QHash>
#include <QString>

#include <optional>

namespace core {

/// What the last sync run recorded about one synchronised file.
struct SyncedFile
{
    /// The SHA-256 hash of the contents both sides had.
    QByteArray hash;
    qint64 remoteSize = 0;
    /// The remote modification time, in milliseconds since the epoch.
    qint64 remoteModified = 0;
    qint64 localSize = 0;
    /// The local modification time, in milliseconds since the epoch.
    qint64 localModified = 0;

    bool operator==(const SyncedFile &) const = default;
};

/// The sync state of a library on this device, in
/// `$XDG_DATA_HOME/jotter/sync/<library-id>/state.sqlite`: the device identifier, a record of
/// every synchronised file with the merge base of note and JSON files, and the modification
/// times of remote attachment folders. It lives outside the library, so it never reaches a sync
/// storage, and losing it only makes the next run a first connection.
///
/// One connection per object; each thread opens its own.
class SyncState
{
public:
    /// Opens or creates the sync state at @p path.
    ///
    /// @param path The database file; missing folders are created.
    /// @return The state, or Io.
    static Result<SyncState> open(const QString &path);

    /// Returns the sync state file of a library in the data folder.
    static QString defaultPath(const QString &libraryId);

    SyncState(SyncState &&other) noexcept;
    SyncState &operator=(SyncState &&other) noexcept;
    SyncState(const SyncState &) = delete;
    SyncState &operator=(const SyncState &) = delete;
    ~SyncState();

    /// Returns this device's identifier, creating it on first use.
    ///
    /// @return The identifier, or Io.
    Result<QString> deviceId();

    /// Reads the records of all synchronised files, without their merge bases.
    ///
    /// @return The records by path relative to the library, or Io.
    Result<QHash<QString, SyncedFile>> files() const;

    /// Reads the merge base of a file.
    ///
    /// @return The contents last synchronised, no value for a file without a base, or Io.
    Result<std::optional<QByteArray>> base(const QString &path) const;

    /// Records a synchronised file with its merge base.
    ///
    /// @param path The file, relative to the library.
    /// @param file The record.
    /// @param base The contents to merge against later; no value for attachments.
    /// @return Nothing, or Io.
    Result<void> putFile(const QString &path, const SyncedFile &file,
                         const std::optional<QByteArray> &base);

    /// Changes the record of a file and keeps its merge base, for a file whose times changed
    /// without its contents.
    Result<void> touchFile(const QString &path, const SyncedFile &file);

    /// Forgets a file that is gone on both sides.
    Result<void> removeFile(const QString &path);

    /// Reads the remote modification times of attachment folders by path.
    Result<QHash<QString, qint64>> folders() const;

    /// Records the remote modification time of an attachment folder.
    Result<void> putFolder(const QString &path, qint64 modified);

    /// Forgets an attachment folder.
    Result<void> removeFolder(const QString &path);

    /// Forgets every file and folder, keeping the device identifier; the next run is a first
    /// connection.
    Result<void> clear();

    /// Starts a transaction; changes are written together by commit().
    Result<void> begin();

    /// Writes the changes since begin().
    Result<void> commit();

private:
    explicit SyncState(QString connection);

    QString m_connection;
};

} // namespace core
