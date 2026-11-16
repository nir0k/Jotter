#include "LocalStorage.h"
#include "core/Attachments.h"
#include "core/Credentials.h"
#include "core/Files.h"
#include "core/Library.h"
#include "core/SftpStorage.h"
#include "core/SyncEngine.h"
#include "core/SyncState.h"

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QtConcurrentRun>

using namespace Qt::StringLiterals;

namespace {

/// A device in a scenario: its library folder and its sync state.
struct Device
{
    QString root;
    QString statePath;
    std::optional<core::SyncState> state;
};

/// Returns the note of a library, or an empty note when it cannot be read.
core::Note readNote(const Device &device, const QString &id)
{
    auto library = core::Library::open(device.root);
    if (!library) {
        return {};
    }
    return library->readNote(id).value_or(core::Note());
}

/// Writes a note into a library, as the editor saves it.
bool writeNote(const Device &device, core::Note note)
{
    auto library = core::Library::open(device.root);
    note.meta.updated = core::now();
    return library && library->writeNote(note);
}

/// Lists the identifiers of the notes in a library.
QStringList noteIds(const Device &device)
{
    QStringList ids = QDir(device.root + u"/notes"_s).entryList(QDir::Files);
    for (QString &id : ids) {
        id.chop(5);
    }
    ids.sort();
    return ids;
}

/// Reads a file of a library.
QByteArray readLocal(const Device &device, const QString &path)
{
    return core::readFile(device.root + u'/' + path).value_or(QByteArray());
}

/// Why a check of the SFTP connection is skipped against a local folder.
constexpr const char *NeedsSftp = "Runs against an SFTP server: set JOTTER_TEST_SFTP";

} // namespace

/// Checks synchronisation with two devices sharing one sync storage: copies,
/// merges and conflicts, deletions and tombstones, the first connection, the lock and the
/// handling of files the storage holds.
///
/// The storage is a local folder. With `JOTTER_TEST_SFTP` set to an address such as
/// `sftp://jotter:jotter@127.0.0.1:2222/library` (the folder relative to the user's home), the
/// same scenarios run against that SFTP server, each in a folder of its own, together with the
/// checks of the SFTP connection; `JOTTER_TEST_SFTP_KEY` names a private key the server accepts,
/// for the key sign-in. tests/sync-check.py runs them against the test stand.
class SyncTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // Credentials and known hosts go to the test locations, never the person's own.
        QStandardPaths::setTestModeEnabled(true);
        const QUrl address(qEnvironmentVariable("JOTTER_TEST_SFTP"));
        if (address.isEmpty()) {
            return;
        }
        // Jotter's known hosts file starts empty.
        QFile::remove(core::SftpStorage::knownHostsPath());
        m_sftp = core::SftpConnection{.host = address.host(),
                                      .port = address.port(22),
                                      .user = address.userName(),
                                      .folder = address.path().sliced(1),
                                      .signIn = core::SftpConnection::SignIn::Password};
        m_password = {.secret = address.password(), .key = {}};
        qInfo("Sync storage: SFTP server %s:%d, folder %s", qPrintable(m_sftp->host), m_sftp->port,
              qPrintable(m_sftp->folder));
    }

    void init()
    {
        m_dir.emplace();
        m_folder = u"run-"_s + core::newId();
        auto base = openStorage(QString(), true);
        QVERIFY(base);
        QVERIFY(base->makeFolder(m_folder));
        m_storage = openStorage(QString());
        QVERIFY(m_storage);
        for (const auto &[device, name] : {std::pair{&m_a, u"a"_s}, std::pair{&m_b, u"b"_s}}) {
            *device = makeDevice(name);
            QVERIFY(device->state);
        }
    }

    void cleanup()
    {
        m_a.state.reset();
        m_b.state.reset();
        m_storage.reset();
        if (auto base = openStorage(QString(), true)) {
            QVERIFY(base->removeFolder(m_folder));
        }
        m_dir.reset();
    }

    /// A library uploaded to an empty storage downloads into an empty folder on another device
    /// with its notes and attachments, file names with any characters included; runs after that
    /// find nothing to do.
    void uploadAndDownload()
    {
        const core::Note note = share({u"<p>Hello</p>"_s});
        const QString attachment = u"attachments/%1/Отчёт #1 (final).pdf"_s.arg(note.meta.id);
        QVERIFY(core::writeFileAtomic(m_a.root + u'/' + attachment, "%PDF-1.7"));
        QVERIFY(sync(m_a));
        QVERIFY(sync(m_b));

        QCOMPARE(readNote(m_b, note.meta.id), note);
        QCOMPARE(readLocal(m_b, attachment), QByteArray("%PDF-1.7"));
        QCOMPARE(readLocal(m_b, u"library.json"_s), readLocal(m_a, u"library.json"_s));
        for (Device *device : {&m_a, &m_b}) {
            const auto report = sync(*device);
            QVERIFY(report);
            QCOMPARE(report->uploaded + report->downloaded + report->merged + report->deleted, 0);
        }
    }

    /// A remote file name that names another place, which only a broken or hostile server
    /// sends, is skipped instead of being written outside the library.
    void remoteNamesStayInTheLibrary()
    {
        /// Lists one more attachment, whose name leads out of its folder.
        class HostileStorage : public LocalStorage
        {
        public:
            using LocalStorage::LocalStorage;

            core::Result<QList<core::RemoteEntry>> list(const QString &path) override
            {
                auto entries = LocalStorage::list(path);
                if (entries && path.startsWith(u"attachments/"_s)) {
                    entries->append({.name = u"x/../../../../escaped"_s,
                                     .folder = false,
                                     .size = 4,
                                     .modified = 1});
                }
                return entries;
            }

            core::Result<void> readTo(const QString &path, QIODevice &target) override
            {
                if (path.endsWith(u"escaped"_s)) {
                    target.write("data");
                    return {};
                }
                return LocalStorage::readTo(path, target);
            }
        };
        HostileStorage storage(m_dir->filePath(u"hostile"_s));
        auto library = core::Library::create(m_a.root);
        QVERIFY(library);
        core::Note note = core::makeNote();
        QVERIFY(library->writeNote(note));
        QVERIFY(core::writeFileAtomic(library->attachmentsDir(note.meta.id) + u"/a.txt"_s, "a"));
        QVERIFY(core::synchronise(m_a.root, storage, *m_a.state));
        core::SyncOptions options;
        options.fullListing = true;
        const auto report = core::synchronise(m_a.root, storage, *m_a.state, options);
        QVERIFY(report);
        QCOMPARE(report->downloaded, 0);
        QVERIFY(!QFileInfo::exists(QFileInfo(m_a.root).absolutePath() + u"/escaped"_s));
    }

    /// A downloaded library keeps the sync state of its download, so the first run after it
    /// transfers nothing again; the download creates missing parent folders.
    void downloadKeepsItsState()
    {
        const core::Note note = share({u"<p>Hello</p>"_s});
        const QString attachment = u"attachments/%1/file.pdf"_s.arg(note.meta.id);
        QVERIFY(core::writeFileAtomic(m_a.root + u'/' + attachment, "%PDF-1.7"));
        QVERIFY(sync(m_a));
        const QString root = m_dir->filePath(u"new/folder/Jotter"_s);
        const auto id = core::downloadLibrary(root, *m_storage);
        QVERIFY(id);
        QCOMPARE(readLocal({.root = root, .statePath = {}, .state = {}}, attachment),
                 QByteArray("%PDF-1.7"));
        /// Passes every call to the test's storage and counts the notes and attachments read.
        class CountingStorage : public core::RemoteStorage
        {
        public:
            explicit CountingStorage(core::RemoteStorage &inner)
                : m_inner(inner)
            {}
            int reads = 0;
            QString location() const override { return m_inner.location(); }
            int timePrecisionMs() const override { return m_inner.timePrecisionMs(); }
            core::Result<QList<core::RemoteEntry>> list(const QString &path) override
            {
                return m_inner.list(path);
            }
            core::Result<void> readTo(const QString &path, QIODevice &target) override
            {
                if (path.startsWith(u"notes/"_s) || path.startsWith(u"attachments/"_s)) {
                    ++reads;
                }
                return m_inner.readTo(path, target);
            }
            core::Result<core::RemoteEntry> writeFrom(const QString &path,
                                                      QIODevice &source) override
            {
                return m_inner.writeFrom(path, source);
            }
            core::Result<bool> create(const QString &path, const QByteArray &data) override
            {
                return m_inner.create(path, data);
            }
            core::Result<void> rename(const QString &from, const QString &to) override
            {
                return m_inner.rename(from, to);
            }
            core::Result<void> remove(const QString &path) override { return m_inner.remove(path); }
            core::Result<void> removeFolder(const QString &path) override
            {
                return m_inner.removeFolder(path);
            }
            core::Result<void> makeFolder(const QString &path) override
            {
                return m_inner.makeFolder(path);
            }

        private:
            core::RemoteStorage &m_inner;
        };
        {
            auto state = core::SyncState::open(core::SyncState::defaultPath(*id));
            QVERIFY(state);
            CountingStorage counting(*m_storage);
            const auto report = core::synchronise(root, counting, *state);
            QVERIFY(report);
            QCOMPARE(report->uploaded + report->downloaded + report->merged + report->deleted, 0);
            QCOMPARE(counting.reads, 0);
        }
        QVERIFY(
            QDir(QFileInfo(core::SyncState::defaultPath(*id)).absolutePath()).removeRecursively());
    }

    /// A run asked to stop ends with Canceled at its next request and releases the lock, so
    /// closing or disconnecting a library does not wait for the transfer.
    void cancelStopsTheRun()
    {
        share({u"<p>One</p>"_s});
        for (const QString &body : {u"<p>Two</p>"_s, u"<p>Three</p>"_s}) {
            core::Note note = core::makeNote();
            note.body = body;
            QVERIFY(writeNote(m_a, note));
        }
        int checks = 0;
        core::SyncOptions options;
        options.canceled = [&checks] { return ++checks > 2; };
        const auto report = core::synchronise(m_a.root, *m_storage, *m_a.state, options);
        QVERIFY(!report);
        QCOMPARE(report.error().code, core::ErrorCode::Canceled);
        QCOMPARE(m_storage->read(u".jotter-lock"_s).error().code, core::ErrorCode::NotFound);
        QVERIFY(sync(m_a));
    }

    /// A download goes only into an empty folder, and only from a storage holding a library.
    void downloadNeedsEmptyFolderAndLibrary()
    {
        QCOMPARE(syncError(m_b), core::ErrorCode::NotFound);
        QVERIFY(core::Library::create(m_a.root));
        QVERIFY(sync(m_a));
        QVERIFY(core::writeFileAtomic(m_b.root + u"/notes.txt"_s, "mine"));
        QCOMPARE(syncError(m_b), core::ErrorCode::NotEmpty);
    }

    /// Changes made on one device reach the other.
    void oneSidedChanges()
    {
        core::Note note = share({u"<p>A</p>"_s});
        note.body = u"<p>A edited</p>"_s;
        QVERIFY(writeNote(m_a, note));
        QVERIFY(sync(m_a));
        const auto report = sync(m_b);
        QVERIFY(report);
        QCOMPARE(report->downloaded, 1);
        QCOMPARE(readNote(m_b, note.meta.id).body, note.body);
    }

    /// Edits of different blocks on two devices combine without a conflict.
    void differentBlocksMerge()
    {
        core::Note note = share({u"<p>A</p>\n<p>B</p>\n<p>C</p>"_s});
        core::Note onA = note;
        onA.body = u"<p>A from a</p>\n<p>B</p>\n<p>C</p>"_s;
        core::Note onB = note;
        onB.body = u"<p>A</p>\n<p>B</p>\n<p>C from b</p>"_s;
        QVERIFY(writeNote(m_a, onA));
        QVERIFY(writeNote(m_b, onB));
        QVERIFY(sync(m_a));
        const auto report = sync(m_b);
        QVERIFY(report);
        QCOMPARE(report->merged, 1);
        QVERIFY(report->noteConflicts.isEmpty());
        QVERIFY(sync(m_a));
        const QString merged = u"<p>A from a</p>\n<p>B</p>\n<p>C from b</p>"_s;
        QCOMPARE(readNote(m_a, note.meta.id).body, merged);
        QCOMPARE(readNote(m_b, note.meta.id).body, merged);
    }

    /// The same block edited on two devices keeps the version synchronised first; the other
    /// becomes a conflict copy in the same collections, which reaches both devices.
    void sameBlockMakesConflictCopy()
    {
        core::Note note = share({u"<p>Shared</p>"_s});
        note.meta.collections = {core::newId()};
        QVERIFY(writeNote(m_a, note));
        QVERIFY(sync(m_a));
        QVERIFY(sync(m_b));
        core::Note onA = readNote(m_a, note.meta.id);
        onA.body = u"<p>From a</p>"_s;
        core::Note onB = readNote(m_b, note.meta.id);
        onB.body = u"<p>From b</p>"_s;
        QVERIFY(writeNote(m_a, onA));
        QVERIFY(writeNote(m_b, onB));
        QVERIFY(sync(m_a));
        const auto report = sync(m_b);
        QVERIFY(report);
        QCOMPARE(report->noteConflicts.size(), 1);
        QCOMPARE(readNote(m_b, note.meta.id).body, onA.body);
        const core::Note copy = readNote(m_b, report->noteConflicts.first());
        QCOMPARE(copy.body, onB.body);
        QCOMPARE(copy.meta.collections, note.meta.collections);
        QVERIFY(copy.meta.title.contains(u"conflict"_s));
        QVERIFY(sync(m_a));
        QCOMPARE(noteIds(m_a), noteIds(m_b));
    }

    /// A conflicting note whose local version refers to a missing attachment, as after a run
    /// interrupted before the file arrived, still gets its conflict copy instead of stopping
    /// every later run.
    void conflictCopyWithMissingAttachment()
    {
        const core::Note note = share({u"<p>Shared</p>"_s});
        QVERIFY(sync(m_a));
        QVERIFY(sync(m_b));
        core::Note onA = readNote(m_a, note.meta.id);
        onA.body = u"<p>From a</p>"_s;
        core::Note onB = readNote(m_b, note.meta.id);
        onB.body = u"<p>From b <a class=\"attachment\" href=\"%1\">gone.pdf</a></p>"_s.arg(
            core::attachmentSource(note.meta.id, u"gone.pdf"_s));
        QVERIFY(writeNote(m_a, onA));
        QVERIFY(writeNote(m_b, onB));
        QVERIFY(sync(m_a));
        const auto report = sync(m_b);
        QVERIFY2(report, report ? "" : qPrintable(report.error().path));
        QCOMPARE(report->noteConflicts.size(), 1);
        QCOMPARE(readNote(m_b, note.meta.id).body, onA.body);
        const core::Note copy = readNote(m_b, report->noteConflicts.first());
        QVERIFY(copy.body.contains(core::attachmentSource(copy.meta.id, u"gone.pdf"_s)));
        QVERIFY(sync(m_a));
        QCOMPARE(noteIds(m_a), noteIds(m_b));
    }

    /// An attachment changed on two devices keeps the remote file; the local one stays beside
    /// it as a conflict copy on both devices.
    void attachmentConflict()
    {
        const core::Note note = share({u"<p>Photo</p>"_s});
        const QString path = u"attachments/%1/photo.jpg"_s.arg(note.meta.id);
        QVERIFY(core::writeFileAtomic(m_a.root + u'/' + path, "original"));
        QVERIFY(sync(m_a));
        QVERIFY(sync(m_b));
        QVERIFY(core::writeFileAtomic(m_a.root + u'/' + path, "from a"));
        QVERIFY(core::writeFileAtomic(m_b.root + u'/' + path, "from b!"));
        QVERIFY(sync(m_a));
        const auto report = sync(m_b);
        QVERIFY(report);
        const QString copy = u"attachments/%1/photo (conflict).jpg"_s.arg(note.meta.id);
        QCOMPARE(report->fileConflicts, QStringList{copy});
        QCOMPARE(readLocal(m_b, path), QByteArray("from a"));
        QCOMPARE(readLocal(m_b, copy), QByteArray("from b!"));
        QVERIFY(sync(m_a));
        QCOMPARE(readLocal(m_a, copy), QByteArray("from b!"));
    }

    /// A note deleted permanently on one device is deleted on the other with its attachments,
    /// even when that device edited it meanwhile; it does not come back.
    void tombstoneDeletesEverywhere()
    {
        core::Note note = share({u"<p>Doomed</p>"_s});
        const QString attachment = u"attachments/%1/file.txt"_s.arg(note.meta.id);
        QVERIFY(core::writeFileAtomic(m_a.root + u'/' + attachment, "data"));
        QVERIFY(sync(m_a));
        QVERIFY(sync(m_b));

        deletePermanently(m_a, note.meta.id);
        note.body = u"<p>Still editing</p>"_s;
        QVERIFY(writeNote(m_b, note));
        QVERIFY(sync(m_a));
        QVERIFY(sync(m_b));
        QVERIFY(sync(m_a));
        for (const Device *device : {&m_a, &m_b}) {
            QVERIFY(!noteIds(*device).contains(note.meta.id));
            QVERIFY(!QFileInfo::exists(device->root + u'/' + attachment));
        }
        QVERIFY(!m_storage->read(u"notes/"_s + note.meta.id + u".html"_s));
    }

    /// A note file removed outside Jotter is removed on the other device too, unless that
    /// device changed it: then it is copied back, so no change is lost.
    void removedOutside()
    {
        core::Note kept = share({u"<p>Kept</p>"_s, u"<p>Gone</p>"_s});
        const QStringList ids = noteIds(m_a);
        const QString gone = ids.at(0) == kept.meta.id ? ids.at(1) : ids.at(0);
        QVERIFY(QFile::remove(m_a.root + u"/notes/"_s + gone + u".html"_s));
        QVERIFY(QFile::remove(m_a.root + u"/notes/"_s + kept.meta.id + u".html"_s));
        kept.body = u"<p>Kept and edited</p>"_s;
        QVERIFY(writeNote(m_b, kept));
        QVERIFY(sync(m_a));
        QVERIFY(sync(m_b));
        QVERIFY(sync(m_a));
        QCOMPARE(noteIds(m_a), QStringList{kept.meta.id});
        QCOMPARE(noteIds(m_b), QStringList{kept.meta.id});
        QCOMPARE(readNote(m_a, kept.meta.id).body, kept.body);
    }

    /// Connecting a copy of the same library: equal notes match, and a note that differs keeps
    /// the remote version with the local one as a conflict copy.
    void firstConnectionToSameLibrary()
    {
        const core::Note same = share({u"<p>Same</p>"_s});
        core::Note other = core::makeNote();
        other.body = u"<p>Remote</p>"_s;
        QVERIFY(writeNote(m_a, other));
        QVERIFY(sync(m_a));
        // Device b holds a copy of the library made before, without sync state.
        const QString copyRoot = m_dir->filePath(u"copy"_s);
        QVERIFY(QDir().mkpath(copyRoot + u"/notes"_s));
        for (const QString &file : {u"library.json"_s, u"collections.json"_s, u"rules.json"_s,
                                    u"notes/"_s + same.meta.id + u".html"_s}) {
            QVERIFY(QFile::copy(m_a.root + u'/' + file, copyRoot + u'/' + file));
        }
        Device copy = makeDevice(u"copy"_s);
        other.body = u"<p>Local</p>"_s;
        QVERIFY(writeNote(copy, other));
        const auto report = sync(copy);
        QVERIFY(report);
        QCOMPARE(report->noteConflicts.size(), 1);
        QCOMPARE(readNote(copy, other.meta.id).body, u"<p>Remote</p>"_s);
        QCOMPARE(readNote(copy, report->noteConflicts.first()).body, u"<p>Local</p>"_s);
    }

    /// A storage holding another library, other files or a library of a newer Jotter is
    /// refused.
    void refusesOtherContents()
    {
        share({});
        Device other = makeDevice(u"other"_s);
        QVERIFY(core::Library::create(other.root));
        QCOMPARE(syncError(other), core::ErrorCode::OtherLibrary);

        auto info = QJsonDocument::fromJson(readLocal(m_a, u"library.json"_s)).object();
        info.insert(u"format"_s, 99);
        QVERIFY(m_storage->write(u"library.json"_s, QJsonDocument(info).toJson()));
        QCOMPARE(syncError(m_a), core::ErrorCode::NewerFormat);

        QVERIFY(m_storage->makeFolder(u"foreign"_s));
        const auto storage = openStorage(u"foreign"_s);
        QVERIFY(storage);
        QVERIFY(storage->write(u"notes.txt"_s, "someone else's"));
        auto run = core::synchronise(m_a.root, *storage, *m_a.state);
        QVERIFY(!run);
        QCOMPARE(run.error().code, core::ErrorCode::NotEmpty);
    }

    /// A lock another device holds stops the run; an expired one is taken over, and the run
    /// removes its own lock when it ends.
    void lock()
    {
        share({});
        const auto lockFor = [](const QDateTime &expires) {
            return QJsonDocument(QJsonObject{{u"device"_s, core::newId()},
                                             {u"expires"_s, core::formatTime(expires)}})
                .toJson();
        };
        QVERIFY(m_storage->write(u".jotter-lock"_s, lockFor(core::now().addSecs(300))));
        QCOMPARE(syncError(m_a), core::ErrorCode::Locked);
        QVERIFY(m_storage->write(u".jotter-lock"_s, lockFor(core::now().addSecs(-1))));
        QVERIFY(sync(m_a));
        QVERIFY(!m_storage->read(u".jotter-lock"_s));
    }

    /// Two devices syncing at once take turns through the lock, and both end with both
    /// changes.
    void concurrentRuns()
    {
        share({});
        core::Note fromA = core::makeNote();
        core::Note fromB = core::makeNote();
        QVERIFY(writeNote(m_a, fromA));
        QVERIFY(writeNote(m_b, fromB));
        // Each device has its own storage connection, as two computers would, and opens its
        // state on the thread that uses it.
        const auto storageA = openStorage(QString());
        const auto storageB = openStorage(QString());
        QVERIFY(storageA && storageB);
        const auto runUntilDone = [](const Device &device, core::RemoteStorage &storage) {
            auto state = core::SyncState::open(device.statePath);
            if (!state) {
                return false;
            }
            for (int attempt = 0; attempt < 100; ++attempt) {
                auto report = core::synchronise(device.root, storage, *state);
                if (report || report.error().code != core::ErrorCode::Locked) {
                    return report.has_value();
                }
                QThread::msleep(10);
            }
            return false;
        };
        auto a = QtConcurrent::run([&] { return runUntilDone(m_a, *storageA); });
        auto b = QtConcurrent::run([&] { return runUntilDone(m_b, *storageB); });
        QVERIFY(a.result());
        QVERIFY(b.result());
        // Whichever device ran first has not seen the other's change yet.
        QVERIFY(sync(m_a));
        QVERIFY(sync(m_b));
        QCOMPARE(noteIds(m_a), noteIds(m_b));
        QCOMPARE(noteIds(m_a).size(), 2);
    }

    /// A remote file rewritten with the same contents is not a change, and temporary files of
    /// interrupted uploads are removed after a day.
    void remoteHousekeeping()
    {
        const core::Note note = share({u"<p>Text</p>"_s});
        const QString path = u"notes/"_s + note.meta.id + u".html"_s;
        QVERIFY(m_storage->write(path, m_storage->read(path).value()));
        auto report = sync(m_b);
        QVERIFY(report);
        QCOMPARE(report->downloaded + report->merged, 0);

        auto *local = dynamic_cast<LocalStorage *>(m_storage.get());
        if (!local) {
            QSKIP("File times cannot be set back on an SFTP server; checked with a local folder");
        }
        QVERIFY(m_storage->write(u".jotter-tmp-old"_s, "partial"));
        QVERIFY(m_storage->write(u".jotter-tmp-new"_s, "partial"));
        local->age(u".jotter-tmp-old"_s, 2);
        QVERIFY(sync(m_a));
        QVERIFY(!m_storage->read(u".jotter-tmp-old"_s));
        QVERIFY(m_storage->read(u".jotter-tmp-new"_s));
    }

    /// Collections and rules added on two devices are all kept.
    void libraryFilesMerge()
    {
        share({});
        for (const auto &[device, name] :
             {std::pair{&m_a, u"From a"_s}, std::pair{&m_b, u"From b"_s}}) {
            auto library = core::Library::open(device->root);
            QVERIFY(library);
            QList<core::Collection> collections = library->readCollections().value();
            core::Collection collection;
            collection.id = core::newId();
            collection.name = name;
            collection.created = collection.updated = core::now();
            collections.append(collection);
            QVERIFY(library->writeCollections(collections));
        }
        QVERIFY(sync(m_a));
        QVERIFY(sync(m_b));
        QVERIFY(sync(m_a));
        QCOMPARE(readLocal(m_a, u"collections.json"_s), readLocal(m_b, u"collections.json"_s));
        QCOMPARE(core::Library::open(m_a.root)->readCollections()->size(), 2);
    }

    /// Many notes upload and download, and a run after that touches nothing; the times are
    /// reported for the sync check.
    void manyNotes()
    {
        share({});
        // An SFTP server is checked with a library of a realistic size.
        const int Count = m_sftp ? 2000 : 300;
        for (int i = 0; i < Count; ++i) {
            core::Note note = core::makeNote();
            note.body = u"<p>Note %1</p>"_s.arg(i);
            QVERIFY(writeNote(m_a, note));
        }
        QElapsedTimer clock;
        clock.start();
        QVERIFY(sync(m_a));
        qInfo("Uploading %d notes took %lld ms", Count, clock.restart());
        QVERIFY(sync(m_b));
        qInfo("Downloading %d notes took %lld ms", Count, clock.restart());
        const auto report = sync(m_b);
        QVERIFY(report);
        qInfo("A run without changes took %lld ms", clock.elapsed());
        QCOMPARE(report->downloaded + report->uploaded, 0);
        QCOMPARE(noteIds(m_b).size(), Count);
    }

    /// Credentials are kept per library in a file only its owner may read, in a folder only
    /// its owner may enter; empty credentials remove the library's entry.
    void credentialsArePrivate()
    {
        const QString library = core::newId();
        const QString other = core::newId();
        const core::Credentials password{.secret = u"pässword"_s, .key = {}};
        const core::Credentials key{.secret = u"passphrase"_s,
                                    .key = u"-----BEGIN OPENSSH PRIVATE KEY-----\n…"_s};
        QVERIFY(core::writeCredentials(library, password));
        QVERIFY(core::writeCredentials(other, key));
        QCOMPARE(core::readCredentials(library).value(), password);
        constexpr auto Others = QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                QFileDevice::WriteOther | QFileDevice::ExeOther;
        const QFileInfo file(core::credentialsPath());
        QCOMPARE(file.permissions() & Others, QFileDevice::Permissions());
        QCOMPARE(QFileInfo(file.absolutePath()).permissions() & Others, QFileDevice::Permissions());
        QVERIFY(core::writeCredentials(library, {}));
        QCOMPARE(core::readCredentials(library).value(), core::Credentials());
        QCOMPARE(core::readCredentials(other).value(), key);
        QFile::remove(core::credentialsPath());
    }

    /// A file written again replaces the old one in one step, leaving no temporary file, and an
    /// existing file is not created again.
    void writeReplaces()
    {
        QVERIFY(m_storage->write(u"file.txt"_s, "first"));
        QVERIFY(m_storage->write(u"file.txt"_s, "second"));
        QCOMPARE(m_storage->read(u"file.txt"_s).value(), QByteArray("second"));
        const auto entries = m_storage->list(QString());
        QVERIFY(entries);
        QCOMPARE(entries->size(), 1);
        QVERIFY(!m_storage->create(u"file.txt"_s, "third").value());
    }

    /// An attachment of the largest size Jotter accepts goes both ways unchanged.
    void largeAttachment()
    {
        const core::Note note = share({u"<p>Video</p>"_s});
        const QString path = u"attachments/%1/video.mp4"_s.arg(note.meta.id);
        QByteArray data(qsizetype{50} * 1024 * 1024, Qt::Uninitialized);
        for (qsizetype i = 0; i < data.size(); ++i) {
            data[i] = static_cast<char>((i * 7919) >> 8);
        }
        QVERIFY(core::writeFileAtomic(m_a.root + u'/' + path, data));
        QElapsedTimer clock;
        clock.start();
        QVERIFY(sync(m_a));
        qInfo("Uploading 50 MB took %lld ms", clock.restart());
        QVERIFY(sync(m_b));
        qInfo("Downloading 50 MB took %lld ms", clock.elapsed());
        QVERIFY(readLocal(m_b, path) == data);
    }

    /// An unknown host key is reported with its fingerprint and refused for sign-in until it
    /// is trusted; then it is known.
    void hostKeyUnknown()
    {
        if (!m_sftp) {
            QSKIP(NeedsSftp);
        }
        QFile::remove(core::SftpStorage::knownHostsPath());
        auto storage = core::SftpStorage::connect(*m_sftp);
        QVERIFY(storage);
        const core::HostKey key = (*storage)->hostKey();
        QCOMPARE(key.state, core::HostKey::State::Unknown);
        QVERIFY(!key.type.isEmpty());
        QVERIFY(key.fingerprint.startsWith(u"SHA256:"_s));
        QCOMPARE((*storage)->signIn(m_password).error().code, core::ErrorCode::HostKey);
        QVERIFY((*storage)->trustHostKey());
        auto again = core::SftpStorage::connect(*m_sftp);
        QVERIFY(again);
        QCOMPARE((*again)->hostKey().state, core::HostKey::State::Known);
        QCOMPARE((*again)->hostKey().fingerprint, key.fingerprint);
    }

    /// A changed host key shows the old and the new fingerprint and stops sync runs; trusting
    /// it replaces the old key.
    void hostKeyChanged()
    {
        if (!m_sftp) {
            QSKIP(NeedsSftp);
        }
        const QString host =
            m_sftp->port == 22 ? m_sftp->host : u"[%1]:%2"_s.arg(m_sftp->host).arg(m_sftp->port);
        // A key the server does not have, recorded for its address.
        QVERIFY(core::writePrivateFile(
            core::SftpStorage::knownHostsPath(),
            host.toUtf8() + " ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIMAG7ofcYv3Vv3uWI60JcdpWwPvj"
                            "CNXf7TXqWwWA0B6U\n"));
        auto storage = core::SftpStorage::connect(*m_sftp);
        QVERIFY(storage);
        const core::HostKey key = (*storage)->hostKey();
        QCOMPARE(key.state, core::HostKey::State::Changed);
        QVERIFY(key.knownFingerprint.startsWith(u"SHA256:"_s));
        QVERIFY(key.knownFingerprint != key.fingerprint);
        auto refused = core::SftpStorage::open(*m_sftp, m_password);
        QVERIFY(!refused);
        QCOMPARE(refused.error().code, core::ErrorCode::HostKey);
        QVERIFY((*storage)->trustHostKey());
        const QByteArray known = core::readFile(core::SftpStorage::knownHostsPath()).value();
        QCOMPARE(known.count('\n'), 1);
        auto again = core::SftpStorage::connect(*m_sftp);
        QVERIFY(again);
        QCOMPARE((*again)->hostKey().state, core::HostKey::State::Known);
    }

    /// A wrong password is a sign-in error and a closed port a network error, which the
    /// application retries; the probe of the connection check names a missing folder.
    void connectionErrors()
    {
        if (!m_sftp) {
            QSKIP(NeedsSftp);
        }
        auto wrong = core::SftpStorage::open(*m_sftp, {.secret = u"not the password"_s, .key = {}});
        QVERIFY(!wrong);
        QCOMPARE(wrong.error().code, core::ErrorCode::SignIn);

        core::SftpConnection closed = *m_sftp;
        closed.port = 1;
        auto unreachable = core::SftpStorage::connect(closed);
        QVERIFY(!unreachable);
        QCOMPARE(unreachable.error().code, core::ErrorCode::Network);

        const auto probe = openStorage(QString());
        QVERIFY(probe);
        QVERIFY(static_cast<core::SftpStorage *>(probe.get())->probe());
        const auto missing = openStorage(u"missing"_s);
        QVERIFY(missing);
        QCOMPARE(static_cast<core::SftpStorage *>(missing.get())->probe().error().code,
                 core::ErrorCode::NotFound);
    }

    /// Choosing a folder: the home folder and a folder relative to it resolve to absolute
    /// paths, and a folder made through a connection to the root is the one a connection to its
    /// path uses.
    void chooseFolder()
    {
        if (!m_sftp) {
            QSKIP(NeedsSftp);
        }
        core::SftpConnection root = *m_sftp;
        root.folder = u"/"_s;
        auto storage = core::SftpStorage::open(root, m_password);
        QVERIFY(storage);
        const auto home = (*storage)->resolveFolder({});
        QVERIFY(home && home->startsWith(u'/'));
        const auto folder = (*storage)->resolveFolder(m_sftp->folder + u'/' + m_folder);
        QVERIFY(folder);
        QCOMPARE(*folder, *home + u'/' + m_sftp->folder + u'/' + m_folder);
        QVERIFY((*storage)->makeFolder(folder->sliced(1) + u"/chosen"_s));
        QVERIFY(m_storage->list(u"chosen"_s));
    }

    /// A key the server accepts signs in from its imported text, and a key method without a
    /// key, as a connection of an earlier version has, is refused; the key file is given in
    /// JOTTER_TEST_SFTP_KEY.
    void keySignIn()
    {
        if (!m_sftp) {
            QSKIP(NeedsSftp);
        }
        const QString keyFile = qEnvironmentVariable("JOTTER_TEST_SFTP_KEY");
        if (keyFile.isEmpty()) {
            QSKIP("JOTTER_TEST_SFTP_KEY is not set");
        }
        auto text = core::readFile(keyFile);
        QVERIFY(text);
        core::SftpConnection connection = *m_sftp;
        connection.signIn = core::SftpConnection::SignIn::Key;
        auto storage =
            core::SftpStorage::open(connection, {.secret = {}, .key = QString::fromUtf8(*text)});
        if (!storage) {
            QFAIL(qPrintable(storage.error().detail));
        }
        auto missing = core::SftpStorage::open(connection, {});
        QVERIFY(!missing);
        QCOMPARE(missing.error().code, core::ErrorCode::SignIn);
    }

    /// Replacing a local note with a symlink must not delete its valid remote version.
    void linkedNoteDoesNotPropagateDeletion()
    {
        const core::Note note = share({u"<p>remote original</p>"_s});
        const QString path = u"notes/"_s + note.meta.id + u".html"_s;
        const QByteArray original = m_storage->read(path).value();
        const QString outside = m_dir->filePath(u"outside.html"_s);
        QVERIFY(core::writeFileAtomic(outside, "private outside bytes"));
        QVERIFY(QFile::remove(m_a.root + u'/' + path));
        QVERIFY(QFile::link(outside, m_a.root + u'/' + path));
        const auto report = sync(m_a);
        QVERIFY(!report);
        QCOMPARE(m_storage->read(path).value(), original);
        QCOMPARE(core::readFile(outside).value(), QByteArray("private outside bytes"));
    }

    /// A malformed merge input reports a failure instead of a successful skipped-file run.
    void malformedMergeIsAnError()
    {
        core::Note note = share({u"<p>base</p>"_s});
        note.body = u"<p>local change</p>"_s;
        QVERIFY(writeNote(m_a, note));
        const QString path = u"notes/"_s + note.meta.id + u".html"_s;
        QVERIFY(m_storage->write(path, "damaged remote note"));
        const auto report = sync(m_a);
        QVERIFY(!report);
        QCOMPARE(report.error().code, core::ErrorCode::Malformed);
        QCOMPARE(readNote(m_a, note.meta.id).body, note.body);
    }

    /// A single listing and a single transfer exceeding the lease lifetime both renew it.
    void leaseCheckpointsDuringLongOperations()
    {
        class SlowStorage : public LocalStorage
        {
        public:
            /// Simulates bounded network batches while retaining the real lock-file behaviour.
            explicit SlowStorage(const QString &root)
                : LocalStorage(root)
            {}
            qint64 elapsed = 0;
            int renewals = 0;
            QDateTime lastExpiry;
            /// Advances a long listing through cooperative checkpoints before returning it.
            core::Result<QList<core::RemoteEntry>> list(const QString &path) override
            {
                for (int i = 0; i < 4; ++i) {
                    elapsed += qint64{4} * 60 * 1000;
                    if (auto checked = checkpoint(); !checked) {
                        return std::unexpected(checked.error());
                    }
                }
                return LocalStorage::list(path);
            }
            /// Treats one file as a long transfer, while lock I/O remains non-recursive.
            core::Result<core::RemoteEntry> writeFrom(const QString &path,
                                                      QIODevice &source) override
            {
                const QByteArray bytes = source.readAll();
                if (path == u".jotter-lock") {
                    ++renewals;
                    const auto object = QJsonDocument::fromJson(bytes).object();
                    lastExpiry = *core::parseTime(object.value(u"expires"_s).toString());
                } else {
                    for (int i = 0; i < 3; ++i) {
                        elapsed += qint64{4} * 60 * 1000;
                        if (auto checked = checkpoint(); !checked) {
                            return std::unexpected(checked.error());
                        }
                    }
                }
                QBuffer buffer;
                buffer.setData(bytes);
                buffer.open(QIODevice::ReadOnly);
                return LocalStorage::writeFrom(path, buffer);
            }
        };
        SlowStorage storage(m_dir->filePath(u"lease-remote"_s));
        QVERIFY(core::Library::create(m_a.root));
        const core::Note note = core::makeNote();
        QVERIFY(writeNote(m_a, note));
        core::SyncOptions options;
        options.elapsedMilliseconds = [&storage] { return storage.elapsed; };
        auto report = core::synchronise(m_a.root, storage, *m_a.state, options);
        QVERIFY2(report, report ? "" : qPrintable(report.error().detail));
        QVERIFY(storage.renewals >= 3);
        QVERIFY(storage.lastExpiry > core::now().addSecs(qint64{10} * 60));
        QVERIFY(!QFile::exists(m_dir->filePath(u"lease-remote/.jotter-lock"_s)));
    }

private:
    /// Opens the sync storage of the running test, or a folder inside it, trusting the
    /// server's host key on first use.
    ///
    /// @param folder The folder inside the test's folder; empty for the test's folder.
    /// @param base Whether to open the folder all tests share instead, to create and remove
    ///        the test's folder.
    /// @return The storage, or null when it cannot be opened.
    std::unique_ptr<core::RemoteStorage> openStorage(const QString &folder, bool base = false)
    {
        QStringList parts;
        if (!base) {
            parts.append(m_folder);
        }
        if (!folder.isEmpty()) {
            parts.append(folder);
        }
        if (!m_sftp) {
            parts.prepend(m_dir->filePath(u"remote"_s));
            return std::make_unique<LocalStorage>(parts.join(u'/'));
        }
        core::SftpConnection connection = *m_sftp;
        parts.prepend(connection.folder);
        connection.folder = parts.join(u'/');
        auto storage = core::SftpStorage::connect(connection);
        if (storage && (*storage)->hostKey().state != core::HostKey::State::Known) {
            if (auto trusted = (*storage)->trustHostKey(); !trusted) {
                storage = std::unexpected(trusted.error());
            }
        }
        if (storage) {
            if (auto signedIn = (*storage)->signIn(m_password); !signedIn) {
                storage = std::unexpected(signedIn.error());
            }
        }
        if (!storage) {
            qWarning("Cannot open %s: %s", qPrintable(storage.error().path),
                     qPrintable(storage.error().detail));
            return nullptr;
        }
        return std::move(*storage);
    }

    /// Returns a device with a library folder and a sync state named after @p name in the test
    /// folder; the folder itself is not created.
    Device makeDevice(const QString &name)
    {
        Device device{.root = m_dir->filePath(name),
                      .statePath = m_dir->filePath(name + u"-state.sqlite"_s),
                      .state = {}};
        if (auto state = core::SyncState::open(device.statePath)) {
            device.state = std::move(*state);
        }
        return device;
    }

    /// Creates the library on device a with notes of the given bodies, uploads it and
    /// downloads it on device b.
    ///
    /// @return The first note, or an empty note when no bodies were given.
    core::Note share(const QStringList &bodies)
    {
        core::Note first;
        if (!core::Library::create(m_a.root)) {
            return first;
        }
        for (const QString &body : bodies) {
            core::Note note = core::makeNote();
            note.body = body;
            writeNote(m_a, note);
            if (first.meta.id.isEmpty()) {
                first = readNote(m_a, note.meta.id);
            }
        }
        if (!sync(m_a) || !sync(m_b)) {
            return {};
        }
        return first;
    }

    core::Result<core::SyncReport> sync(Device &device)
    {
        auto report = core::synchronise(device.root, *m_storage, *device.state);
        if (!report) {
            qWarning("Sync of %s failed: %s", qPrintable(device.root),
                     qPrintable(report.error().path + u": "_s + report.error().detail));
        }
        return report;
    }

    /// Runs a sync that must fail and returns its error code.
    core::ErrorCode syncError(Device &device)
    {
        auto report = core::synchronise(device.root, *m_storage, *device.state);
        return report ? core::ErrorCode::Io : report.error().code;
    }

    /// Deletes a note as Jotter does: its file and attachments go, and a tombstone is recorded.
    void deletePermanently(Device &device, const QString &id)
    {
        auto library = core::Library::open(device.root);
        QVERIFY(library);
        QVERIFY(QFile::remove(library->notePath(id)));
        QDir(library->attachmentsDir(id)).removeRecursively();
        core::LibraryInfo info = library->info();
        info.tombstones.append({.id = id, .deleted = core::now()});
        QVERIFY(library->writeInfo(info));
    }

    /// The SFTP server the tests run against, when one is set.
    std::optional<core::SftpConnection> m_sftp;
    core::Credentials m_password;
    std::optional<QTemporaryDir> m_dir;
    /// The folder of the running test inside the shared one.
    QString m_folder;
    std::unique_ptr<core::RemoteStorage> m_storage;
    Device m_a;
    Device m_b;
};

QTEST_GUILESS_MAIN(SyncTest)
#include "tst_sync.moc"
