#pragma once

#include "core/Error.h"

#include <QStringList>

#include <functional>

namespace core {

class RemoteStorage;
class SyncState;

/// What a sync run did.
struct SyncReport
{
    /// Files copied to the sync storage.
    int uploaded = 0;
    /// Files copied into the library.
    int downloaded = 0;
    /// Files changed on both sides and merged.
    int merged = 0;
    /// Files deleted on either side.
    int deleted = 0;
    /// Identifiers of the conflict copies of notes this run made.
    QStringList noteConflicts;
    /// Conflict copies of attachments this run made, relative to the library.
    QStringList fileConflicts;
    /// Files left for the next run, relative to the library: changed in the library while the
    /// run was about to write them, or unreadable for a merge.
    QStringList skipped;
};

/// Options of a sync run.
struct SyncOptions
{
    /// Lists every remote attachment folder, not only those whose time changed (Sync now).
    bool fullListing = false;
    /// Called on the worker thread with the number of files done and to do.
    std::function<void(int done, int total)> progress;
    /// Optional monotonic elapsed-time source for deterministic lease and slow-storage tests.
    /// Production uses QElapsedTimer when this callback is absent.
    std::function<qint64()> elapsedMilliseconds;
    /// Called on the worker thread between requests; when it returns true, the run stops with
    /// Canceled and releases the lock.
    std::function<bool()> canceled;
};

/// Minutes a sync run's lock lasts unless renewed.
inline constexpr int SyncLockMinutes = 10;

/// Runs one synchronisation of a library folder with a sync storage: takes
/// the remote lock, compares both sides with the sync state, copies, merges and deletes files,
/// records the new state and releases the lock.
///
/// A folder without a library that is empty or missing receives the library of the storage. A
/// storage without a library receives the library, as long as it holds
/// no other files. Runs on a worker thread; the library may change meanwhile, and files changed
/// while the run was about to write them are left for the next run.
///
/// @param root The library folder.
/// @param storage The sync storage.
/// @param state The library's sync state on this device.
/// @param options Options of the run.
/// @return What the run did; or Locked while another device holds the lock, OtherLibrary or
///         NotEmpty for a storage holding something else, NotEmpty for a download into a folder
///         with files, NewerFormat for a library of a newer Jotter, or the error of a file or
///         of the storage.
Result<SyncReport> synchronise(const QString &root, RemoteStorage &storage, SyncState &state,
                               const SyncOptions &options = {});

/// Reads the identifier of the library a sync storage holds, to choose the sync state before
/// downloading it.
///
/// @param storage The sync storage.
/// @return The identifier, or NotFound when the storage holds no library, NewerFormat,
///         Malformed or the error of the storage.
Result<QString> remoteLibraryId(RemoteStorage &storage);

/// Downloads the library a sync storage holds into a missing or empty folder. The files and a sync
/// state of their own go to a staging folder beside the target; only a complete download is
/// published into the target, and its sync state becomes the library's, so the first run after it
/// transfers nothing again. A failed attempt removes only its staging folder.
///
/// @param root The folder to download into; missing parent folders are created.
/// @param storage The sync storage, signed in.
/// @param options The progress callback and cancellation of the run.
/// @return The identifier of the downloaded library, or NotFound when the storage holds no
///         library, NotEmpty when the folder holds files, or the error of the run.
Result<QString> downloadLibrary(const QString &root, RemoteStorage &storage,
                                const SyncOptions &options = {});

/// Checks that a sync storage may be connected to a library: it holds the
/// same library, or nothing but Jotter's own temporary and lock files.
///
/// @param storage The sync storage.
/// @param libraryId The library to connect.
/// @return Nothing, or OtherLibrary / NotEmpty, NewerFormat, Malformed or the error of the
///         storage.
Result<void> checkStorageFor(RemoteStorage &storage, const QString &libraryId);

} // namespace core
