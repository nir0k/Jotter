#include "core/SyncEngine.h"

#include "core/Attachments.h"
#include "core/Directory.h"
#include "core/FileGuard.h"
#include "core/Files.h"
#include "core/Library.h"
#include "core/Merge.h"
#include "core/RemoteStorage.h"
#include "core/SyncState.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonObject>
#include <QLocale>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>

namespace core {

namespace {

constexpr QLatin1StringView InfoFile("library.json");
constexpr QLatin1StringView CollectionsFile("collections.json");
constexpr QLatin1StringView RulesFile("rules.json");
constexpr QLatin1StringView ContentsFile("index.html");
constexpr QLatin1StringView NotesDir("notes");
constexpr QLatin1StringView AttachmentsDir("attachments");
constexpr QLatin1StringView LockFile(".jotter-lock");
constexpr QLatin1StringView TempPrefix(".jotter-tmp-");

constexpr qint64 LockLifetimeMs = qint64{SyncLockMinutes} * 60 * 1000;
/// A long run renews its lock when half of its lifetime has passed.
constexpr qint64 LockRenewMs = LockLifetimeMs / 2;
/// Temporary files of interrupted uploads are removed after a day.
constexpr qint64 TempMaxAgeMs = qint64{24} * 60 * 60 * 1000;
/// A library file modified this recently may change again without its time or size changing,
/// as file system times are coarse; its time is not recorded, so the next run reads it again.
constexpr qint64 RacyMs = 2000;
/// The sync state is written in batches of this many files, so an interrupted run keeps what
/// it did without a disk flush per file.
constexpr int CommitInterval = 200;

/// The kinds of synchronised files, which differ in how they merge.
enum class FileKind { Info, Collections, Rules, Note, Attachment, Contents };

/// The size and modification time of a file on one side.
struct Attributes
{
    qint64 size = 0;
    qint64 modified = 0;

    bool operator==(const Attributes &) const = default;
};

/// One side's version of a file during a run.
struct Version
{
    bool exists = false;
    /// Whether it differs from the synchronised contents, or is new, or was deleted.
    bool changed = false;
    /// Whether the contents were read; they are only when needed.
    bool read = false;
    /// The contents of a note or JSON file, which are merged and so read into memory.
    QByteArray data;
    /// The local file holding the contents of an attachment, which is only copied, so a large
    /// one never sits in memory: the library's file, or a downloaded temporary file beside it.
    QString file;
    /// Whether file is a temporary download, which the run moves into place or removes.
    bool temporary = false;
    QByteArray hash;

    /// Opens the contents for reading.
    ///
    /// @return The open device, or the error of opening the file.
    Result<std::unique_ptr<QIODevice>> open() const
    {
        if (file.isEmpty()) {
            auto buffer = std::make_unique<QBuffer>();
            buffer->setData(data);
            buffer->open(QIODevice::ReadOnly);
            return buffer;
        }
        auto opened = openFile(file, QIODevice::ReadOnly);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        return std::move(*opened);
    }
};

QByteArray hashOf(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

/// Hashes a local file a part at a time.
///
/// @param path The file.
/// @return The SHA-256 hash, or the error of reading it.
Result<QByteArray> hashOfFile(const QString &path)
{
    auto file = openFile(path, QIODevice::ReadOnly);
    if (!file) {
        return std::unexpected(file.error());
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(file->get())) {
        return fail(ErrorCode::Io, path, (*file)->errorString());
    }
    return hash.result();
}

/// Tells whether a file name is a note file, `<uuid>.html`.
bool isNoteFileName(const QString &name)
{
    return name.endsWith(QLatin1StringView(".html")) && isValidId(name.chopped(5));
}

/// Returns the note a note file or attachment belongs to, or an empty string for other files.
QString noteIdOf(const QString &path)
{
    if (path.startsWith(NotesDir + u'/')) {
        return path.sliced(NotesDir.size() + 1).chopped(5);
    }
    if (path.startsWith(AttachmentsDir + u'/')) {
        return path.sliced(AttachmentsDir.size() + 1).section(u'/', 0, 0);
    }
    return {};
}

FileKind kindOf(const QString &path)
{
    if (path == InfoFile) {
        return FileKind::Info;
    }
    if (path == CollectionsFile) {
        return FileKind::Collections;
    }
    if (path == RulesFile) {
        return FileKind::Rules;
    }
    if (path == ContentsFile) {
        return FileKind::Contents;
    }
    return path.startsWith(NotesDir + u'/') ? FileKind::Note : FileKind::Attachment;
}

/// Returns the folder of a path relative to the library, empty for the root.
QString folderOf(const QString &path)
{
    const qsizetype slash = path.lastIndexOf(u'/');
    return slash < 0 ? QString() : path.first(slash);
}

/// Returns the size and modification time of a local file, or no value for a missing file or
/// one that is not a regular file.
std::optional<Attributes> localAttributes(const QString &file)
{
    const QFileInfo info(file);
    if (!info.isFile() || info.isSymLink()) {
        return std::nullopt;
    }
    return Attributes{.size = info.size(), .modified = info.lastModified().toMSecsSinceEpoch()};
}

/// One sync run; see synchronise().
class Run
{
public:
    Run(QString root, RemoteStorage &storage, SyncState &state, const SyncOptions &options)
        : m_root(std::move(root))
        , m_storage(storage)
        , m_state(state)
        , m_options(options)
    {}

    Result<SyncReport> exec()
    {
        auto device = m_state.deviceId();
        if (!device) {
            return std::unexpected(device.error());
        }
        m_device = *device;
        if (isCanceled()) {
            return fail(ErrorCode::Canceled, m_storage.location());
        }
        m_clock.start();
        m_startedUtc = now();
        if (auto locked = lock(); !locked) {
            return std::unexpected(locked.error());
        }
        m_storage.setCheckpoint([this] { return renewLock(); });
        const auto resetCheckpoint = qScopeGuard([this] { m_storage.setCheckpoint({}); });
        Result<void> result = m_state.begin();
        if (result) {
            result = syncAll();
            // What was recorded stays valid when the run fails half way.
            if (auto committed = m_state.commit(); !committed && result) {
                result = committed;
            }
        }
        // Changes made by others after the lock is released must carry later times than any
        // this run recorded, or the next run could not tell them apart: the run waits until
        // the server's clock has passed the precision of the latest recorded time. The
        // server's time is estimated from the lock's modification time, which is never later
        // than the real one.
        if (const int precision = m_storage.timePrecisionMs();
            precision > 0 && m_latest > 0 && !isCanceled()) {
            const qint64 serverNow = m_lockServerTime + elapsed() - m_lockedAt;
            const qint64 wait = std::min<qint64>(m_latest + precision + precision / 10 - serverNow,
                                                 qint64{2} * precision);
            if (wait > 0) {
                QThread::msleep(static_cast<unsigned long>(wait));
            }
        }
        m_storage.setCheckpoint({});
        unlock();
        if (!result) {
            return std::unexpected(result.error());
        }
        return m_report;
    }

private:
    /// Uses one monotonic time source for lease renewal and its wall-clock expiry.
    qint64 elapsed() const
    {
        return m_options.elapsedMilliseconds ? m_options.elapsedMilliseconds() : m_clock.elapsed();
    }

    /// Tells whether the caller asked the run to stop.
    bool isCanceled() const { return m_options.canceled && m_options.canceled(); }

    /// Returns the contents of the lock this device writes, expiring a lock lifetime from now.
    QByteArray lockContents() const
    {
        return toJsonText(
            QJsonObject{{QStringLiteral("device"), m_device},
                        {QStringLiteral("expires"),
                         formatTime(m_startedUtc.addMSecs(elapsed() + LockLifetimeMs))}});
    }

    /// Takes the remote lock. An expired lock, or one this device left behind, is taken over:
    /// it is first moved aside, so that of two devices taking it over at once only one wins.
    Result<void> lock()
    {
        const auto locked = [&] { return fail(ErrorCode::Locked, m_storage.location()); };
        m_lock = lockContents();
        auto created = m_storage.create(LockFile, m_lock);
        if (!created) {
            return std::unexpected(created.error());
        }
        if (!*created) {
            auto held = m_storage.read(LockFile);
            if (!held) {
                return held.error().code == ErrorCode::NotFound ? locked()
                                                                : std::unexpected(held.error());
            }
            const auto object = parseJsonObject(*held, LockFile);
            const auto expires =
                object ? parseTime(object->value(QStringLiteral("expires")).toString())
                       : std::nullopt;
            const bool mine =
                object && object->value(QStringLiteral("device")).toString() == m_device;
            if (!mine && expires && *expires > now()) {
                return locked();
            }
            // A lock without a readable expiry is being written by another device, unless it
            // is far older than a lock lasts by this device's clock.
            if (!expires) {
                auto entries = m_storage.list(QString());
                if (!entries) {
                    return std::unexpected(entries.error());
                }
                const auto entry =
                    std::ranges::find(*entries, QString(LockFile), &RemoteEntry::name);
                if (entry != entries->cend() &&
                    now().toMSecsSinceEpoch() - entry->modified < 2 * LockLifetimeMs) {
                    return locked();
                }
            }
            const QString aside = TempPrefix + newId();
            if (auto moved = m_storage.rename(LockFile, aside); !moved) {
                return moved.error().code == ErrorCode::NotFound ? locked()
                                                                 : std::unexpected(moved.error());
            }
            if (auto movedData = m_storage.read(aside); !movedData || *movedData != *held) {
                // Another device took the lock over in between; its lock goes back.
                if (auto restored = m_storage.rename(aside, LockFile); !restored) {
                    qWarning("Cannot restore the sync lock of another device: %s",
                             qPrintable(restored.error().detail));
                }
                return locked();
            }
            // A leftover is a temporary file, removed by a later run.
            if (auto removed = m_storage.remove(aside); !removed) {
                qWarning("Cannot remove an expired sync lock: %s",
                         qPrintable(removed.error().detail));
            }
            created = m_storage.create(LockFile, m_lock);
            if (!created) {
                return std::unexpected(created.error());
            }
            if (!*created) {
                return locked();
            }
        }
        m_lockRenewed = m_lockedAt = elapsed();
        return {};
    }

    /// Renews the lock when half of its lifetime has passed; a lock another device took over
    /// meanwhile stops the run.
    Result<void> renewLock()
    {
        if (isCanceled()) {
            return fail(ErrorCode::Canceled, m_storage.location());
        }
        if (elapsed() - m_lockRenewed < LockRenewMs) {
            return {};
        }
        auto held = m_storage.read(LockFile);
        if (!held || *held != m_lock) {
            return fail(ErrorCode::Locked, m_storage.location());
        }
        m_lock = lockContents();
        if (auto written = m_storage.write(LockFile, m_lock); !written) {
            return std::unexpected(written.error());
        }
        m_lockRenewed = elapsed();
        return {};
    }

    /// Removes the lock when it is still this run's.
    void unlock()
    {
        if (auto held = m_storage.read(LockFile); held && *held == m_lock) {
            if (auto removed = m_storage.remove(LockFile); !removed) {
                // The lock expires by itself.
                qWarning("Cannot release the sync lock: %s", qPrintable(removed.error().detail));
            }
        }
    }

    /// Checks what both sides hold, then synchronises every file: the JSON files first, so the
    /// tombstones are known, then notes, attachments and the table of contents.
    Result<void> syncAll()
    {
        auto rootEntries = m_storage.list(QString());
        if (!rootEntries) {
            return std::unexpected(rootEntries.error());
        }
        if (auto prepared = prepare(*rootEntries); !prepared) {
            return prepared;
        }
        if (auto listed = listRemote(*rootEntries); !listed) {
            return listed;
        }
        if (auto listed = listLocal(); !listed) {
            return listed;
        }

        QStringList paths;
        for (const QHash<QString, Attributes> *side : {&m_local, &m_remote}) {
            for (auto it = side->cbegin(); it != side->cend(); ++it) {
                paths.append(it.key());
            }
        }
        for (auto it = m_files.cbegin(); it != m_files.cend(); ++it) {
            paths.append(it.key());
        }
        std::ranges::sort(paths, [](const QString &a, const QString &b) {
            return std::pair(kindOf(a), a) < std::pair(kindOf(b), b);
        });
        paths.erase(std::ranges::unique(paths).begin(), paths.end());
        // Only files whose attributes differ from the state can need work.
        paths.removeIf([&](const QString &path) { return !candidate(path); });
        const int total = static_cast<int>(paths.size());
        int doneCount = 0;
        bool tombstonesRead = false;
        for (const QString &path : std::as_const(paths)) {
            const FileKind kind = kindOf(path);
            if (!tombstonesRead && kind != FileKind::Info && kind != FileKind::Collections &&
                kind != FileKind::Rules) {
                if (auto deleted = deleteTombstoned(); !deleted) {
                    return deleted;
                }
                tombstonesRead = true;
            }
            if (!m_tombstones.contains(noteIdOf(path)) || kind == FileKind::Contents) {
                if (auto synced = syncFile(path); !synced) {
                    return synced;
                }
            }
            if (m_options.progress) {
                m_options.progress(++doneCount, total);
            }
        }
        if (!tombstonesRead) {
            if (auto deleted = deleteTombstoned(); !deleted) {
                return deleted;
            }
        }
        return recordFolders();
    }

    /// Checks the library on both sides and removes stale temporary files
    /// from the root of the storage.
    Result<void> prepare(const QList<RemoteEntry> &rootEntries)
    {
        for (const RemoteEntry &entry : rootEntries) {
            if (entry.name == LockFile) {
                m_lockServerTime = entry.modified;
            }
        }
        m_staleBefore = m_lockServerTime - TempMaxAgeMs;
        bool remoteLibrary = false;
        bool foreign = false;
        for (const RemoteEntry &entry : rootEntries) {
            if (entry.name == InfoFile) {
                remoteLibrary = true;
            } else if (entry.name.startsWith(TempPrefix)) {
                if (auto removed = removeStale(QString(), entry); !removed) {
                    return removed;
                }
            } else if (entry.name != LockFile) {
                foreign = true;
            }
        }
        const bool localLibrary = QFileInfo::exists(m_root + u'/' + InfoFile);
        if (!localLibrary) {
            const QDir dir(m_root);
            if (dir.exists() &&
                !dir.isEmpty(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)) {
                return fail(ErrorCode::NotEmpty, m_root);
            }
            if (!remoteLibrary) {
                return fail(ErrorCode::NotFound, m_storage.location(),
                            QStringLiteral("the storage holds no library"));
            }
            for (const QLatin1StringView sub : {NotesDir, AttachmentsDir}) {
                if (!makeDirectory(m_root + u'/' + sub)) {
                    return fail(ErrorCode::Io, m_root + u'/' + sub);
                }
            }
        }
        if (remoteLibrary) {
            auto data = m_storage.read(InfoFile);
            if (!data) {
                return std::unexpected(data.error());
            }
            auto remote = parseLibraryInfo(*data, m_storage.location() + u'/' + InfoFile);
            if (!remote) {
                return std::unexpected(remote.error());
            }
            if (localLibrary) {
                const QString path = m_root + u'/' + InfoFile;
                auto localData = readFile(path);
                if (!localData) {
                    return std::unexpected(localData.error());
                }
                auto local = parseLibraryInfo(*localData, path);
                if (!local) {
                    return std::unexpected(local.error());
                }
                if (local->id != remote->id) {
                    return fail(ErrorCode::OtherLibrary, m_storage.location());
                }
            } else if (auto cleared = m_state.clear(); !cleared) {
                return cleared;
            }
        } else {
            if (foreign) {
                return fail(ErrorCode::NotEmpty, m_storage.location());
            }
            // An empty storage receives the whole library, whatever the state remembers of an
            // earlier one; without this, files missing there would count as deleted.
            if (auto cleared = m_state.clear(); !cleared) {
                return cleared;
            }
            for (const QLatin1StringView sub : {NotesDir, AttachmentsDir}) {
                if (auto made = m_storage.makeFolder(sub); !made) {
                    return made;
                }
            }
        }
        auto files = m_state.files();
        if (!files) {
            return std::unexpected(files.error());
        }
        m_files = std::move(*files);
        auto folders = m_state.folders();
        if (!folders) {
            return std::unexpected(folders.error());
        }
        m_folders = std::move(*folders);
        return {};
    }

    /// Removes a temporary file of an interrupted upload older than a day.
    Result<void> removeStale(const QString &folder, const RemoteEntry &entry)
    {
        if (entry.folder || entry.modified >= m_staleBefore) {
            return {};
        }
        return m_storage.remove(folder.isEmpty() ? entry.name : folder + u'/' + entry.name);
    }

    /// Lists the synchronised files of the storage. An attachment folder whose modification
    /// time is the one recorded is not listed again; its files are taken from the state.
    Result<void> listRemote(const QList<RemoteEntry> &rootEntries)
    {
        for (const RemoteEntry &entry : rootEntries) {
            if (!entry.folder && (entry.name == InfoFile || entry.name == CollectionsFile ||
                                  entry.name == RulesFile)) {
                m_remote.insert(entry.name, {.size = entry.size, .modified = entry.modified});
            }
        }
        auto notes = listFolder(NotesDir);
        if (!notes) {
            return std::unexpected(notes.error());
        }
        for (const RemoteEntry &entry : *notes) {
            if (!entry.folder && isNoteFileName(entry.name)) {
                m_remote.insert(NotesDir + u'/' + entry.name,
                                {.size = entry.size, .modified = entry.modified});
            }
        }
        auto folders = listFolder(AttachmentsDir);
        if (!folders) {
            return std::unexpected(folders.error());
        }
        QHash<QString, QStringList> recorded;
        for (auto it = m_files.cbegin(); it != m_files.cend(); ++it) {
            if (kindOf(it.key()) == FileKind::Attachment) {
                recorded[folderOf(it.key())].append(it.key());
            }
        }
        for (const RemoteEntry &folder : *folders) {
            if (!folder.folder || !isValidId(folder.name)) {
                continue;
            }
            const QString path = AttachmentsDir + u'/' + folder.name;
            m_remoteFolders.insert(path, folder.modified);
            if (!m_options.fullListing && m_folders.value(path, -1) == folder.modified) {
                for (const QString &file : recorded.value(path)) {
                    const SyncedFile &known = m_files[file];
                    m_remote.insert(file,
                                    {.size = known.remoteSize, .modified = known.remoteModified});
                }
                continue;
            }
            auto files = listFolder(path);
            if (!files) {
                return std::unexpected(files.error());
            }
            for (const RemoteEntry &entry : *files) {
                if (entry.folder) {
                    continue;
                }
                if (entry.name.startsWith(TempPrefix)) {
                    if (auto removed = removeStale(path, entry); !removed) {
                        return removed;
                    }
                    continue;
                }
                // A name is one entry of the folder; one that names another place, which only a
                // broken or hostile server sends, would be written outside the library.
                if (!isEntryName(entry.name)) {
                    qWarning("Skipping a remote attachment with an invalid name in %s",
                             qPrintable(path));
                    continue;
                }
                m_remote.insert(path + u'/' + entry.name,
                                {.size = entry.size, .modified = entry.modified});
            }
        }
        return {};
    }

    /// Lists a remote folder; a missing folder is empty.
    Result<QList<RemoteEntry>> listFolder(const QString &path)
    {
        auto entries = m_storage.list(path);
        if (!entries && entries.error().code == ErrorCode::NotFound) {
            return QList<RemoteEntry>();
        }
        return entries;
    }

    /// Lists the synchronised files of the library.
    Result<void> listLocal()
    {
        for (const QLatin1StringView name : {InfoFile, CollectionsFile, RulesFile, ContentsFile}) {
            if (const auto attributes = localAttributes(m_root + u'/' + name)) {
                m_local.insert(name, *attributes);
                m_revisions.insert(name, fileRevision(m_root + u'/' + name));
            }
        }
        const QDir notes(m_root + u'/' + NotesDir);
        auto noteNames = listDirectory(notes.path());
        if (!noteNames) {
            return std::unexpected(noteNames.error());
        }
        for (const QString &name : *noteNames) {
            if (!isNoteFileName(name)) {
                continue;
            }
            const QString path = NotesDir + u'/' + name;
            const QFileInfo entry(m_root + u'/' + path);
            if (entry.isSymLink() || !entry.isFile()) {
                return fail(ErrorCode::Io, entry.filePath(),
                            QStringLiteral("not a regular library file"));
            }
            if (const auto attributes = localAttributes(m_root + u'/' + path)) {
                m_local.insert(path, *attributes);
                m_revisions.insert(path, fileRevision(m_root + u'/' + path));
            }
        }
        const QDir attachments(m_root + u'/' + AttachmentsDir);
        auto folders = listDirectory(attachments.path());
        if (!folders) {
            return std::unexpected(folders.error());
        }
        for (const QString &folder : *folders) {
            if (!isValidId(folder)) {
                continue;
            }
            const QDir dir(attachments.filePath(folder));
            auto names = listDirectory(dir.path());
            if (!names) {
                return std::unexpected(names.error());
            }
            for (const QString &name : *names) {
                // Jotter's own temporary files, such as an interrupted download, are not
                // attachments.
                if (name.startsWith(QLatin1StringView(".jotter-"))) {
                    continue;
                }
                const QString path = AttachmentsDir + u'/' + folder + u'/' + name;
                const QFileInfo entry(m_root + u'/' + path);
                if (entry.isSymLink() || !entry.isFile()) {
                    return fail(ErrorCode::Io, entry.filePath(),
                                QStringLiteral("not a regular library file"));
                }
                if (const auto attributes = localAttributes(m_root + u'/' + path)) {
                    m_local.insert(path, *attributes);
                    m_revisions.insert(path, fileRevision(m_root + u'/' + path));
                }
            }
        }
        return {};
    }

    /// Tells whether a file may need work: it is new, gone on a side, or its attributes differ
    /// from those recorded.
    bool candidate(const QString &path) const
    {
        const auto known = m_files.constFind(path);
        const auto local = m_local.constFind(path);
        const auto remote = m_remote.constFind(path);
        if (known == m_files.constEnd()) {
            return true;
        }
        if (local == m_local.constEnd() ||
            *local != Attributes{.size = known->localSize, .modified = known->localModified}) {
            return true;
        }
        // The table of contents is never downloaded, so its remote side does not matter.
        return kindOf(path) != FileKind::Contents &&
               (remote == m_remote.constEnd() ||
                *remote !=
                    Attributes{.size = known->remoteSize, .modified = known->remoteModified});
    }

    /// Reads the library's version of a file as far as needed to tell whether it changed.
    Result<Version> localVersion(const QString &path, const SyncedFile *known)
    {
        Version version;
        const auto attributes = m_local.constFind(path);
        if (attributes == m_local.constEnd()) {
            version.changed = known != nullptr;
            return version;
        }
        version.exists = true;
        if (known && attributes->size == known->localSize &&
            attributes->modified == known->localModified) {
            version.hash = known->hash;
            return version;
        }
        if (kindOf(path) == FileKind::Attachment) {
            version.file = m_root + u'/' + path;
            auto hash = hashOfFile(version.file);
            if (!hash) {
                return std::unexpected(hash.error());
            }
            version.hash = *hash;
            version.changed = !known || version.hash != known->hash;
            version.read = true;
            return version;
        }
        auto data = readFile(m_root + u'/' + path);
        if (!data) {
            return std::unexpected(data.error());
        }
        version.hash = hashOf(*data);
        version.changed = !known || version.hash != known->hash;
        version.read = true;
        version.data = std::move(*data);
        return version;
    }

    /// Reads the storage's version of a file as far as needed to tell whether it changed: a
    /// file whose time changed is downloaded and compared by its hash.
    Result<Version> remoteVersion(const QString &path, const SyncedFile *known)
    {
        Version version;
        const auto attributes = m_remote.constFind(path);
        if (attributes == m_remote.constEnd()) {
            version.changed = known != nullptr;
            return version;
        }
        version.exists = true;
        if (known && attributes->size == known->remoteSize &&
            attributes->modified == known->remoteModified) {
            version.hash = known->hash;
            return version;
        }
        if (kindOf(path) == FileKind::Attachment) {
            auto file = fetch(path);
            if (!file) {
                return std::unexpected(file.error());
            }
            version.file = *file;
            version.temporary = true;
            auto hash = hashOfFile(version.file);
            if (!hash) {
                return std::unexpected(hash.error());
            }
            version.hash = *hash;
            version.changed = !known || version.hash != known->hash;
            version.read = true;
            return version;
        }
        auto data = m_storage.read(path);
        if (!data) {
            return std::unexpected(data.error());
        }
        version.hash = hashOf(*data);
        version.changed = !known || version.hash != known->hash;
        version.read = true;
        version.data = std::move(*data);
        return version;
    }

    /// Downloads a remote attachment into a temporary file beside its place in the library, on
    /// the same file system, so it can be moved into place by a rename.
    ///
    /// @param path The attachment, relative to the library.
    /// @return The temporary file, or the error of the storage or the library.
    Result<QString> fetch(const QString &path)
    {
        const QString folder = m_root + u'/' + folderOf(path);
        if (auto made = makeDirectory(folder); !made) {
            return std::unexpected(made.error());
        }
        QString temporary = folder + u'/' + TempPrefix + newId();
        auto file = openFile(temporary, QIODevice::WriteOnly | QIODevice::NewOnly);
        if (!file) {
            return std::unexpected(file.error());
        }
        auto read = m_storage.readTo(path, **file);
        if (read && !(*file)->flush()) {
            read = fail(ErrorCode::Io, temporary, (*file)->errorString());
        }
        file->reset();
        if (!read) {
            const auto removed = removePath(temporary);
            Q_UNUSED(removed)
            return std::unexpected(read.error());
        }
        return temporary;
    }

    /// Synchronises one file.
    Result<void> syncFile(const QString &path)
    {
        if (auto renewed = renewLock(); !renewed) {
            return renewed;
        }
        const auto knownIt = m_files.constFind(path);
        const std::optional<SyncedFile> known =
            knownIt == m_files.constEnd() ? std::nullopt : std::optional(*knownIt);
        const SyncedFile *knownPtr = known ? &*known : nullptr;
        auto local = localVersion(path, knownPtr);
        if (!local) {
            return std::unexpected(local.error());
        }
        if (kindOf(path) == FileKind::Contents) {
            // Every device generates its own table of contents; it is only uploaded.
            return local->changed && local->exists ? upload(path, *local) : Result<void>();
        }
        auto remote = remoteVersion(path, knownPtr);
        if (!remote) {
            return std::unexpected(remote.error());
        }
        // A downloaded attachment not moved into place is not kept.
        const auto dropDownload = qScopeGuard([&] {
            if (remote->temporary) {
                const auto removed = removePath(remote->file);
                Q_UNUSED(removed)
            }
        });
        if (!local->exists && !remote->exists) {
            m_files.remove(path);
            return countChange(m_state.removeFile(path));
        }
        if (local->exists && remote->exists && local->hash == remote->hash) {
            // Unchanged, or changed alike on both sides: only the record changes.
            const Attributes inLibrary = settled(m_local[path]);
            const SyncedFile record{.hash = local->hash,
                                    .remoteSize = m_remote[path].size,
                                    .remoteModified = m_remote[path].modified,
                                    .localSize = inLibrary.size,
                                    .localModified = inLibrary.modified};
            if (known && *known == record) {
                return {};
            }
            m_files.insert(path, record);
            m_latest = std::max(m_latest, record.remoteModified);
            if (known && known->hash == record.hash) {
                return countChange(m_state.touchFile(path, record));
            }
            const QByteArray &data = local->read ? local->data : remote->data;
            return countChange(m_state.putFile(path, record, baseOf(path, data)));
        }
        if (!remote->changed) {
            return local->exists ? upload(path, *local) : deleteRemote(path);
        }
        if (!local->changed) {
            return remote->exists ? download(path, *remote) : deleteLocal(path);
        }
        // Changed on both sides. A change wins over a deletion, so no change is lost.
        if (!remote->exists) {
            return upload(path, *local);
        }
        if (!local->exists) {
            return download(path, *remote);
        }
        return merge(path, known.has_value(), *local, *remote);
    }

    /// Returns the merge base to record for a file: its contents for notes and JSON files,
    /// nothing for attachments and the table of contents.
    static std::optional<QByteArray> baseOf(const QString &path, const QByteArray &data)
    {
        const FileKind kind = kindOf(path);
        if (kind == FileKind::Attachment || kind == FileKind::Contents) {
            return std::nullopt;
        }
        return data;
    }

    /// Returns the attributes of a library file to record; see RacyMs.
    static Attributes settled(Attributes local)
    {
        if (QDateTime::currentMSecsSinceEpoch() - local.modified < RacyMs) {
            local.modified = -1;
        }
        return local;
    }

    /// Records a synchronised file.
    ///
    /// @param path The file, relative to the library.
    /// @param hash The hash of its contents.
    /// @param data Its contents for a note or JSON file, the base of later merges; empty for
    ///        other files.
    /// @param remote Its attributes in the storage.
    /// @param local Its attributes in the library.
    Result<void> record(const QString &path, const QByteArray &hash, const QByteArray &data,
                        const Attributes &remote, const Attributes &local)
    {
        const Attributes inLibrary = settled(local);
        const SyncedFile file{.hash = hash,
                              .remoteSize = remote.size,
                              .remoteModified = remote.modified,
                              .localSize = inLibrary.size,
                              .localModified = inLibrary.modified};
        m_files.insert(path, file);
        m_latest = std::max(m_latest, file.remoteModified);
        return countChange(m_state.putFile(path, file, baseOf(path, data)));
    }

    /// Commits the sync state after every CommitInterval changes.
    Result<void> countChange(Result<void> result)
    {
        if (!result || ++m_pending < CommitInterval) {
            return result;
        }
        m_pending = 0;
        if (auto committed = m_state.commit(); !committed) {
            return committed;
        }
        return m_state.begin();
    }

    /// Writes a file to the storage, creating its folder the first time.
    Result<Attributes> put(const QString &path, const Version &version)
    {
        const QString folder = folderOf(path);
        if (!folder.isEmpty() && !m_madeFolders.contains(folder)) {
            if (auto made = m_storage.makeFolder(folder); !made) {
                return std::unexpected(made.error());
            }
            m_madeFolders.insert(folder);
        }
        auto source = version.open();
        if (!source) {
            return std::unexpected(source.error());
        }
        auto entry = m_storage.writeFrom(path, **source);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        m_changedFolders.insert(folder);
        const Attributes attributes{.size = entry->size, .modified = entry->modified};
        m_remote.insert(path, attributes);
        return attributes;
    }

    /// Tells whether a library file is still as it was listed, so the run may replace it.
    bool unchangedLocally(const QString &path) const
    {
        const auto listed = m_local.constFind(path);
        const auto current = localAttributes(m_root + u'/' + path);
        return listed == m_local.constEnd()
                   ? !current
                   : current == *listed &&
                         m_revisions.value(path) == fileRevision(m_root + u'/' + path);
    }

    /// Writes a file into the library unless it changed since it was listed. A downloaded
    /// attachment is moved into place; other contents are written atomically.
    ///
    /// @return Its new attributes, or no value when it was left for the next run.
    Result<std::optional<Attributes>> putLocal(const QString &path, const Version &version)
    {
        const QString file = m_root + u'/' + path;
        const FileGuard guard(file);
        if (!unchangedLocally(path)) {
            m_report.skipped.append(path);
            return std::optional<Attributes>();
        }
        const PathParts parts = splitPath(file);
        auto folder = Directory::open(parts.folder, true);
        if (!folder) {
            return std::unexpected(folder.error());
        }
        Result<void> written;
        if (version.temporary && splitPath(version.file).folder == parts.folder) {
            written = folder->rename(splitPath(version.file).name, parts.name);
        } else if (auto source = version.open(); source) {
            written = folder->write(parts.name, **source);
        } else {
            written = std::unexpected(source.error());
        }
        if (!written) {
            return std::unexpected(written.error());
        }
        const auto attributes = localAttributes(file);
        if (!attributes) {
            return fail(ErrorCode::Io, file);
        }
        m_local.insert(path, *attributes);
        m_revisions.insert(path, fileRevision(m_root + u'/' + path));
        return attributes;
    }

    Result<void> upload(const QString &path, const Version &local)
    {
        auto remote = put(path, local);
        if (!remote) {
            return std::unexpected(remote.error());
        }
        ++m_report.uploaded;
        return record(path, local.hash, local.data, *remote, m_local[path]);
    }

    Result<void> download(const QString &path, const Version &remote)
    {
        auto local = putLocal(path, remote);
        if (!local) {
            return std::unexpected(local.error());
        }
        if (!*local) {
            return {};
        }
        ++m_report.downloaded;
        return record(path, remote.hash, remote.data, m_remote[path], **local);
    }

    Result<void> deleteRemote(const QString &path)
    {
        if (auto removed = m_storage.remove(path); !removed) {
            return removed;
        }
        m_changedFolders.insert(folderOf(path));
        m_remote.remove(path);
        m_files.remove(path);
        ++m_report.deleted;
        return countChange(m_state.removeFile(path));
    }

    Result<void> deleteLocal(const QString &path)
    {
        const FileGuard guard(m_root + u'/' + path);
        if (!unchangedLocally(path)) {
            m_report.skipped.append(path);
            return {};
        }
        const QString file = m_root + u'/' + path;
        if (!removePath(file)) {
            return fail(ErrorCode::Io, file, QStringLiteral("cannot delete the file"));
        }
        if (kindOf(path) == FileKind::Attachment) {
            // An attachment folder left empty goes too; rmdir() keeps one that is not.
            const auto cleaned = removePath(m_root + u'/' + folderOf(path));
            Q_UNUSED(cleaned)
        }
        m_local.remove(path);
        m_files.remove(path);
        ++m_report.deleted;
        return countChange(m_state.removeFile(path));
    }

    /// Merges a file changed on both sides and writes the result to both.
    ///
    /// @param path The file.
    /// @param hasBase Whether the state holds a version both sides started from.
    /// @param local The library's version, read.
    /// @param remote The storage's version, read.
    Result<void> merge(const QString &path, bool hasBase, const Version &local,
                       const Version &remote)
    {
        const FileKind kind = kindOf(path);
        if (kind == FileKind::Attachment) {
            return attachmentConflict(path, local, remote);
        }
        std::optional<QByteArray> base;
        if (hasBase) {
            auto stored = m_state.base(path);
            if (!stored) {
                return std::unexpected(stored.error());
            }
            base = std::move(*stored);
        }
        // Each kind parses the three versions, merges them and serialises the result; a version
        // that cannot be parsed leaves the file for the person to repair.
        const auto mergeWith = [&]<typename T>(const auto &parse, const auto &mergeParsed,
                                               const auto &serialize) -> std::optional<QByteArray> {
            const Result<T> l = parse(local.data, path);
            const Result<T> r = parse(remote.data, path);
            if (!l || !r) {
                return std::nullopt;
            }
            std::optional<T> b;
            if (base) {
                if (Result<T> parsed = parse(*base, path)) {
                    b = std::move(*parsed);
                }
            }
            return serialize(mergeParsed(b, *l, *r));
        };
        std::optional<QByteArray> merged;
        switch (kind) {
        case FileKind::Info:
            merged = mergeWith.template operator()<LibraryInfo>(
                parseLibraryInfo,
                [](const std::optional<LibraryInfo> &b, const LibraryInfo &l,
                   const LibraryInfo &r) { return mergeLibraryInfo(b, l, r, now()); },
                serializeLibraryInfo);
            break;
        case FileKind::Collections:
            merged = mergeWith.template operator()<QList<Collection>>(
                parseCollections,
                [](const std::optional<QList<Collection>> &b, const QList<Collection> &l,
                   const QList<Collection> &r) {
                    return mergeCollections(b.value_or(QList<Collection>()), l, r);
                },
                serializeCollections);
            break;
        case FileKind::Rules:
            merged = mergeWith.template operator()<QList<Rule>>(
                parseRules,
                [](const std::optional<QList<Rule>> &b, const QList<Rule> &l,
                   const QList<Rule> &r) { return mergeRules(b.value_or(QList<Rule>()), l, r); },
                serializeRules);
            break;
        case FileKind::Note:
            merged = mergeWith.template operator()<Note>(
                parseNote,
                [&](const std::optional<Note> &b, const Note &l, const Note &r) {
                    const NoteMerge result = mergeNote(b, l, r);
                    if (result.conflict) {
                        m_conflict = l;
                    }
                    return result.note;
                },
                serializeNote);
            break;
        case FileKind::Contents:
        case FileKind::Attachment:
            break;
        }
        if (!merged) {
            return fail(ErrorCode::Malformed, path,
                        QStringLiteral("cannot merge an unreadable version"));
        }
        if (const std::optional<Note> conflict = std::exchange(m_conflict, std::nullopt)) {
            if (!unchangedLocally(path)) {
                m_report.skipped.append(path);
                return {};
            }
            if (auto copied = conflictCopy(*conflict); !copied) {
                return copied;
            }
        }
        const Version result{.exists = true,
                             .changed = true,
                             .read = true,
                             .data = *merged,
                             .file = {},
                             .temporary = false,
                             .hash = hashOf(*merged)};
        Attributes inLibrary = m_local[path];
        if (*merged != local.data) {
            auto written = putLocal(path, result);
            if (!written) {
                return std::unexpected(written.error());
            }
            if (!*written) {
                return {};
            }
            inLibrary = **written;
        }
        Attributes inStorage = m_remote[path];
        if (*merged != remote.data) {
            auto written = put(path, result);
            if (!written) {
                return std::unexpected(written.error());
            }
            inStorage = *written;
        }
        ++m_report.merged;
        return record(path, result.hash, result.data, inStorage, inLibrary);
    }

    /// Saves the local version of a note whose body conflicts as a new note beside it, in the
    /// same collections, as the editor keeps a note changed outside while it is open.
    Result<void> conflictCopy(const Note &local)
    {
        Note copy = makeNote();
        const QString title = local.meta.title.isEmpty()
                                  ? QCoreApplication::translate("core::Sync", "Untitled")
                                  : local.meta.title;
        copy.meta.title =
            QCoreApplication::translate("core::Sync", "%1 (conflict, %2)")
                .arg(title, QLocale().toString(QDateTime::currentDateTime(), QLocale::ShortFormat));
        copy.meta.collections = local.meta.collections;
        copy.meta.tags = local.meta.tags;
        copy.body = local.body;
        auto attachments = copyNoteAttachments(m_root, copy);
        if (!attachments) {
            return std::unexpected(attachments.error());
        }
        // Publish the independent files before the copy becomes visible remotely.
        for (const QString &name : *attachments) {
            const QString attachment = AttachmentsDir + u'/' + copy.meta.id + u'/' + name;
            Version stored{.exists = true,
                           .changed = true,
                           .read = true,
                           .data = {},
                           .file = m_root + u'/' + attachment,
                           .temporary = false,
                           .hash = {}};
            auto hash = hashOfFile(stored.file);
            if (!hash) {
                return std::unexpected(hash.error());
            }
            stored.hash = *hash;
            auto remote = put(attachment, stored);
            if (!remote) {
                return std::unexpected(remote.error());
            }
            const auto attributes = localAttributes(m_root + u'/' + attachment);
            if (!attributes) {
                return fail(ErrorCode::Io, attachment);
            }
            m_local.insert(attachment, *attributes);
            if (auto recorded = record(attachment, stored.hash, {}, *remote, *attributes);
                !recorded) {
                return recorded;
            }
        }
        const QString path = NotesDir + u'/' + copy.meta.id + QStringLiteral(".html");
        const QByteArray data = serializeNote(copy);
        const Version note{.exists = true,
                           .changed = true,
                           .read = true,
                           .data = data,
                           .file = {},
                           .temporary = false,
                           .hash = hashOf(data)};
        auto inLibrary = putLocal(path, note);
        if (!inLibrary || !*inLibrary) {
            return inLibrary ? Result<void>() : std::unexpected(inLibrary.error());
        }
        auto inStorage = put(path, note);
        if (!inStorage) {
            return std::unexpected(inStorage.error());
        }
        m_report.noteConflicts.append(copy.meta.id);
        return record(path, note.hash, data, *inStorage, **inLibrary);
    }

    /// Resolves an attachment changed on both sides: the remote file wins and the local one is
    /// kept beside it as `<name> (conflict).<ext>` on both sides.
    Result<void> attachmentConflict(const QString &path, const Version &local,
                                    const Version &remote)
    {
        if (!unchangedLocally(path)) {
            m_report.skipped.append(path);
            return {};
        }
        const QString folder = folderOf(path);
        const QFileInfo name(path.sliced(folder.size() + 1));
        const QString suffix = name.suffix().isEmpty() ? QString() : u'.' + name.suffix();
        const QString stem = name.suffix().isEmpty() ? name.fileName() : name.completeBaseName();
        QString copyPath;
        for (int n = 1;; ++n) {
            copyPath =
                folder + u'/' + stem +
                (n == 1 ? QStringLiteral(" (conflict)") : QStringLiteral(" (conflict %1)").arg(n)) +
                suffix;
            if (!QFileInfo::exists(m_root + u'/' + copyPath) && !m_remote.contains(copyPath)) {
                break;
            }
        }
        auto copyLocal = putLocal(copyPath, local);
        if (!copyLocal || !*copyLocal) {
            return copyLocal ? Result<void>() : std::unexpected(copyLocal.error());
        }
        auto copyRemote = put(copyPath, local);
        if (!copyRemote) {
            return std::unexpected(copyRemote.error());
        }
        if (auto recorded = record(copyPath, local.hash, {}, *copyRemote, **copyLocal); !recorded) {
            return recorded;
        }
        m_report.fileConflicts.append(copyPath);
        return download(path, remote);
    }

    /// Reads the tombstones of the synchronised `library.json` and deletes the notes they name
    /// on both sides, with their attachment folders.
    Result<void> deleteTombstoned()
    {
        const QString infoPath = m_root + u'/' + InfoFile;
        auto data = readFile(infoPath);
        if (!data) {
            return std::unexpected(data.error());
        }
        auto info = parseLibraryInfo(*data, infoPath);
        if (!info) {
            return std::unexpected(info.error());
        }
        for (const Tombstone &tombstone : info->tombstones) {
            m_tombstones.insert(tombstone.id);
        }
        if (m_tombstones.isEmpty()) {
            return {};
        }
        QSet<QString> remoteNotes;
        QStringList localPaths;
        for (auto it = m_local.cbegin(); it != m_local.cend(); ++it) {
            if (m_tombstones.contains(noteIdOf(it.key()))) {
                localPaths.append(it.key());
            }
        }
        for (auto it = m_remote.cbegin(); it != m_remote.cend(); ++it) {
            if (kindOf(it.key()) == FileKind::Note && m_tombstones.contains(noteIdOf(it.key()))) {
                remoteNotes.insert(it.key());
            }
        }
        for (const QString &path : std::as_const(localPaths)) {
            if (kindOf(path) == FileKind::Note) {
                if (auto removed = removePath(m_root + u'/' + path); !removed) {
                    return removed;
                }
                ++m_report.deleted;
            }
            m_local.remove(path);
        }
        for (const QString &path : std::as_const(remoteNotes)) {
            if (auto removed = m_storage.remove(path); !removed) {
                return removed;
            }
            ++m_report.deleted;
        }
        for (const QString &id : std::as_const(m_tombstones)) {
            const QString folder = AttachmentsDir + u'/' + id;
            if (auto removed = removePath(m_root + u'/' + folder, true); !removed) {
                return removed;
            }
            if (m_remoteFolders.remove(folder) > 0) {
                if (auto removed = m_storage.removeFolder(folder); !removed) {
                    return removed;
                }
            }
        }
        m_remote.removeIf(
            [&](const auto &entry) { return m_tombstones.contains(noteIdOf(entry.key())); });
        QStringList forgotten;
        for (auto it = m_files.cbegin(); it != m_files.cend(); ++it) {
            if (m_tombstones.contains(noteIdOf(it.key()))) {
                forgotten.append(it.key());
            }
        }
        for (const QString &path : std::as_const(forgotten)) {
            m_files.remove(path);
            if (auto removed = countChange(m_state.removeFile(path)); !removed) {
                return removed;
            }
        }
        return {};
    }

    /// Records the modification times of the remote attachment folders. A folder this run
    /// changed is listed again next time, as its new time is unknown.
    Result<void> recordFolders()
    {
        QSet<QString> paths = m_changedFolders;
        for (auto it = m_folders.cbegin(); it != m_folders.cend(); ++it) {
            paths.insert(it.key());
        }
        for (auto it = m_remoteFolders.cbegin(); it != m_remoteFolders.cend(); ++it) {
            paths.insert(it.key());
        }
        for (const QString &path : std::as_const(paths)) {
            if (!path.startsWith(AttachmentsDir + u'/')) {
                continue;
            }
            Result<void> recorded;
            if (m_changedFolders.contains(path)) {
                recorded = m_state.putFolder(path, -1);
            } else if (const auto listed = m_remoteFolders.constFind(path);
                       listed != m_remoteFolders.constEnd()) {
                if (m_folders.value(path, -1) != *listed) {
                    recorded = m_state.putFolder(path, *listed);
                    m_latest = std::max(m_latest, *listed);
                }
            } else {
                recorded = m_state.removeFolder(path);
            }
            if (!recorded) {
                return recorded;
            }
        }
        return {};
    }

    QString m_root;
    RemoteStorage &m_storage;
    SyncState &m_state;
    const SyncOptions &m_options;
    QString m_device;
    /// The contents of this run's lock, to recognise it.
    QByteArray m_lock;
    QElapsedTimer m_clock;
    QDateTime m_startedUtc;
    qint64 m_lockRenewed = 0;
    /// When the lock was taken, on m_clock and by the server's clock.
    qint64 m_lockedAt = 0;
    qint64 m_lockServerTime = 0;
    /// The latest remote modification time this run recorded.
    qint64 m_latest = 0;
    /// Temporary files modified before this remote time are stale.
    qint64 m_staleBefore = 0;
    QHash<QString, Attributes> m_local;
    QHash<QString, QString> m_revisions;
    QHash<QString, Attributes> m_remote;
    QHash<QString, SyncedFile> m_files;
    /// Recorded and listed modification times of remote attachment folders.
    QHash<QString, qint64> m_folders;
    QHash<QString, qint64> m_remoteFolders;
    QSet<QString> m_madeFolders;
    QSet<QString> m_changedFolders;
    QSet<QString> m_tombstones;
    /// The local note of a merge that conflicted, to be saved as a conflict copy.
    std::optional<Note> m_conflict;
    int m_pending = 0;
    SyncReport m_report;
};

} // namespace

Result<SyncReport> synchronise(const QString &root, RemoteStorage &storage, SyncState &state,
                               const SyncOptions &options)
{
    return Run(resolvedPath(root), storage, state, options).exec();
}

Result<QString> remoteLibraryId(RemoteStorage &storage)
{
    auto data = storage.read(InfoFile);
    if (!data) {
        return std::unexpected(data.error());
    }
    auto info = parseLibraryInfo(*data, storage.location() + u'/' + InfoFile);
    if (!info) {
        return std::unexpected(info.error());
    }
    return info->id;
}

Result<QString> downloadLibrary(const QString &root, RemoteStorage &storage,
                                const SyncOptions &options)
{
    auto id = remoteLibraryId(storage);
    if (!id) {
        return id;
    }
    const QString target = resolvedPath(root);
    const QString parent = splitPath(target).folder;
    if (auto made = makeDirectory(parent); !made) {
        return std::unexpected(made.error());
    }
    // Both the partial library and its sync state belong exclusively to this attempt; the
    // staging folder is on the target's file system, so publishing it is a rename.
    const QTemporaryDir staging(parent + QStringLiteral("/.jotter-download-XXXXXX"));
    if (!staging.isValid()) {
        return fail(ErrorCode::Io, target, staging.errorString());
    }
    const QString stagedRoot = staging.path() + QStringLiteral("/library");
    const QString stagedState = staging.path() + QStringLiteral("/state.sqlite");
    {
        auto state = SyncState::open(stagedState);
        if (!state) {
            return std::unexpected(state.error());
        }
        if (auto report = synchronise(stagedRoot, storage, *state, options); !report) {
            return std::unexpected(report.error());
        }
    }
    if (auto published = publishDirectory(stagedRoot, target); !published) {
        return std::unexpected(published.error());
    }
    // The state was closed above, which folds its write-ahead log into the file. Leftovers of an
    // earlier state of this library would not match it, so they go first.
    const QString statePath = SyncState::defaultPath(*id);
    for (const QString &suffix : {QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        if (auto removed = removePath(statePath + suffix); !removed) {
            qWarning("Cannot remove an old sync state file: %s",
                     qPrintable(removed.error().detail));
        }
    }
    auto moved = makeDirectory(splitPath(statePath).folder);
    if (moved) {
        moved = copyFileAtomic(stagedState, statePath);
    }
    if (!moved) {
        // The library is complete; without its state the first run compares every file again.
        qWarning("Cannot keep the sync state of the download: %s",
                 qPrintable(moved.error().detail));
    }
    return id;
}

Result<void> checkStorageFor(RemoteStorage &storage, const QString &libraryId)
{
    auto remote = remoteLibraryId(storage);
    if (remote) {
        return *remote == libraryId ? Result<void>()
                                    : fail(ErrorCode::OtherLibrary, storage.location());
    }
    if (remote.error().code != ErrorCode::NotFound) {
        return std::unexpected(remote.error());
    }
    auto entries = storage.list(QString());
    if (!entries) {
        return std::unexpected(entries.error());
    }
    // Jotter's own names all start with `.jotter-`: the lock, temporary files and probes.
    const bool foreign = std::ranges::any_of(*entries, [](const RemoteEntry &entry) {
        return !entry.name.startsWith(QLatin1StringView(".jotter-"));
    });
    return foreign ? fail(ErrorCode::NotEmpty, storage.location()) : Result<void>();
}

} // namespace core
