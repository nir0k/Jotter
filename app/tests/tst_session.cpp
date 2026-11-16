#include "core/Files.h"
#include "core/LibrarySession.h"
#include "core/Tags.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

using namespace Qt::StringLiterals;

/// Checks the note operations of an open library: saving and tags, the trash and its
/// retention period, permanent deletion, and noticing changes made by other programs.
class SessionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init()
    {
        m_dir.emplace();
        QVERIFY(core::Library::create(m_dir->path()));
        auto session = core::LibrarySession::open(m_dir->path());
        QVERIFY(session);
        m_session = std::move(*session);
        QSignalSpy firstScan(m_session.get(), &core::LibrarySession::notesChanged);
        QVERIFY(firstScan.wait());
    }

    void cleanup()
    {
        m_session.reset();
        m_dir.reset();
    }

    /// A saved note is in the index at once with its tags; `#word` in the text is only text.
    void saveIndexesTags()
    {
        core::Note note = core::makeNote();
        note.meta.tags = {u"home"_s};
        note.body = u"<p>Call about #repairs</p>"_s;
        QVERIFY(m_session->saveNote(note));
        const auto summary = m_session->index().note(note.meta.id);
        QVERIFY(summary && summary->has_value());
        QCOMPARE((*summary)->tags, QStringList{u"home"_s});
    }

    /// Trashed notes past the retention period are deleted with their attachments, leave the
    /// favourites and get tombstones, which replace expired ones; newer notes stay.
    void purgeTrash()
    {
        core::Note old = core::makeNote();
        core::Note recent = core::makeNote();
        QVERIFY(m_session->saveNote(old));
        QVERIFY(m_session->saveNote(recent));
        QVERIFY(QDir().mkpath(m_session->library().attachmentsDir(old.meta.id)));
        const core::Tombstone expired{.id = core::newId(), .deleted = core::now().addDays(-91)};
        QVERIFY(m_session->updateInfo([&](core::LibraryInfo &info) {
            info.favorites = {{.kind = core::FavoriteRef::Kind::Note, .id = old.meta.id}};
            info.tombstones = {expired};
        }));

        QVERIFY(m_session->updateNotes(
            {old.meta.id}, [](core::NoteMeta &meta) { meta.trashed = core::now().addDays(-40); }));
        QVERIFY(m_session->updateNotes({recent.meta.id}, [](core::NoteMeta &meta) {
            meta.trashed = core::now().addDays(-10);
        }));

        QCOMPARE(m_session->purgeTrash().value(), 1);
        QVERIFY(!QFile::exists(m_session->library().notePath(old.meta.id)));
        QVERIFY(!QDir(m_session->library().attachmentsDir(old.meta.id)).exists());
        QVERIFY(m_session->library().info().favorites.isEmpty());
        const QList<core::Tombstone> tombstones = m_session->library().info().tombstones;
        QCOMPARE(tombstones.size(), 1);
        QCOMPARE(tombstones.first().id, old.meta.id);
        QVERIFY(QFile::exists(m_session->library().notePath(recent.meta.id)));
        QCOMPARE(m_session->index()
                     .count({.view = core::NoteFilter::View::Trash, .ids = {}, .tag = {}})
                     .value(),
                 1);
    }

    /// A library that keeps the trash until it is emptied never purges.
    void keepTrashForever()
    {
        QVERIFY(m_session->updateInfo(
            [](core::LibraryInfo &info) { info.trashRetentionDays = std::nullopt; }));
        core::Note note = core::makeNote();
        QVERIFY(m_session->saveNote(note));
        QVERIFY(m_session->updateNotes(
            {note.meta.id}, [](core::NoteMeta &meta) { meta.trashed = core::now().addYears(-5); }));
        QCOMPARE(m_session->purgeTrash().value(), 0);
    }

    /// A file rewritten by another program is reported; Jotter's own writes are not.
    void changedOutside()
    {
        core::Note note = core::makeNote();
        QVERIFY(m_session->saveNote(note));
        QVERIFY(!m_session->changedOutside(note.meta.id));
        note.body = u"<p>Edited elsewhere</p>"_s;
        QVERIFY(core::writeFileAtomic(m_session->library().notePath(note.meta.id),
                                      core::serializeNote(note)));
        QVERIFY(m_session->changedOutside(note.meta.id));
        m_session->markSeen(note.meta.id);
        QVERIFY(!m_session->changedOutside(note.meta.id));
    }

    /// `library.json` rewritten outside, as synchronisation does, becomes current.
    void infoChangedOutside()
    {
        core::LibraryInfo info = m_session->library().info();
        info.tagColors = {{u"work"_s, u"sky"_s}};
        QVERIFY(core::writeFileAtomic(QDir(m_dir->path()).filePath(u"library.json"_s),
                                      core::serializeLibraryInfo(info)));
        QSignalSpy changed(m_session.get(), &core::LibrarySession::infoChanged);
        m_session->refresh();
        QCOMPARE(changed.size(), 1);
        QCOMPARE(m_session->library().info(), info);
    }

    /// Deleting a collection removes its subtree; notes keep their other collections and, as
    /// chosen, notes left in none move to the trash, or all notes move into another collection.
    void deleteCollectionTree()
    {
        const auto work = m_session->createCollection(u"Work"_s, std::nullopt).value();
        const auto meetings = m_session->createCollection(u"Meetings"_s, work.id).value();
        const auto home = m_session->createCollection(u"Home"_s, std::nullopt).value();
        core::Note shared = core::makeNote();
        shared.meta.collections = {meetings.id, home.id};
        core::Note only = core::makeNote();
        only.meta.collections = {meetings.id};
        QVERIFY(m_session->saveNote(shared));
        QVERIFY(m_session->saveNote(only));

        using Notes = core::CollectionNotes;
        QVERIFY(!m_session->deleteCollection(work.id,
                                             {.kind = Notes::Kind::Move, .target = meetings.id}));
        QVERIFY(m_session->deleteCollection(work.id,
                                            {.kind = Notes::Kind::TrashOrphans, .target = {}}));
        QCOMPARE(m_session->collections().size(), 1);
        QCOMPARE(m_session->library().readNote(shared.meta.id)->meta.collections,
                 QStringList{home.id});
        QVERIFY(!m_session->library().readNote(shared.meta.id)->meta.trashed);
        QVERIFY(m_session->library().readNote(only.meta.id)->meta.trashed);

        const auto ideas = m_session->createCollection(u"Ideas"_s, std::nullopt).value();
        QVERIFY(
            m_session->deleteCollection(home.id, {.kind = Notes::Kind::Move, .target = ideas.id}));
        QCOMPARE(m_session->library().readNote(shared.meta.id)->meta.collections,
                 QStringList{ideas.id});
    }

    /// A collection cannot move into its own descendant.
    void moveCollectionRefusesCycle()
    {
        const auto work = m_session->createCollection(u"Work"_s, std::nullopt).value();
        const auto meetings = m_session->createCollection(u"Meetings"_s, work.id).value();
        QVERIFY(!m_session->moveCollection(work.id, meetings.id, {}));
        QVERIFY(m_session->moveCollection(meetings.id, std::nullopt, work.id));
        QCOMPARE(m_session->collections().first().id, meetings.id);
        QVERIFY(!m_session->collections().first().parent);
    }

    /// A note leaves Unfiled notes when it joins a collection and returns when it leaves its
    /// last one; a `quick` field of an earlier version does not keep it there.
    void unfiledFollowsCollections()
    {
        const auto work = m_session->createCollection(u"Work"_s, std::nullopt).value();
        const auto ideas = m_session->createCollection(u"Ideas"_s, std::nullopt).value();
        core::Note note = core::makeNote();
        note.meta.extra.insert(u"quick"_s, true);
        QVERIFY(m_session->saveNote(note));
        const auto unfiled = [this] {
            return m_session->index()
                .count({.view = core::NoteFilter::View::Unfiled, .ids = {}, .tag = {}})
                .value_or(-1);
        };
        QCOMPARE(unfiled(), 1);
        QVERIFY(m_session->changeCollections({note.meta.id}, {work.id, ideas.id}, {}));
        QCOMPARE(unfiled(), 0);
        QVERIFY(m_session->changeCollections({note.meta.id}, {}, {work.id}));
        QCOMPARE(unfiled(), 0);
        QVERIFY(m_session->changeCollections({note.meta.id}, {}, {ideas.id}));
        QCOMPARE(unfiled(), 1);
    }

    /// Renaming a tag renames its children too and merges with a tag of the new name; the
    /// text is not touched.
    void renameTag()
    {
        core::Note note = core::makeNote();
        note.meta.tags = {u"Work/Meetings"_s, u"job/meetings"_s, u"home"_s};
        note.body = u"<p>See #work</p>"_s;
        QVERIFY(m_session->saveNote(note));
        QCOMPARE(m_session->renameTag(u"work"_s, u"job"_s).value(), 1);
        const auto renamed = m_session->library().readNote(note.meta.id).value();
        QCOMPARE(renamed.meta.tags, (QStringList{u"job/Meetings"_s, u"home"_s}));
        QCOMPARE(renamed.body, note.body);
    }

    /// Colours move with a renamed tag and its children and are written to `library.json`; a
    /// tag merged into one that has a colour keeps that colour, and children without one take
    /// their parent's.
    void renameTagMovesColors()
    {
        core::Note note = core::makeNote();
        note.meta.tags = {u"work"_s, u"work/meetings"_s, u"job"_s};
        QVERIFY(m_session->saveNote(note));
        QVERIFY(m_session->updateInfo([](core::LibraryInfo &info) {
            info.tagColors = {{u"work"_s, u"red"_s},
                              {u"work/meetings"_s, u"sky"_s},
                              {u"job/meetings"_s, u"green"_s}};
        }));
        QVERIFY(m_session->renameTag(u"Work"_s, u"Job"_s));
        const QMap<QString, QString> expected{{u"job"_s, u"red"_s},
                                              {u"job/meetings"_s, u"green"_s}};
        QCOMPARE(m_session->library().info().tagColors, expected);
        QCOMPARE(core::Library::open(m_session->library().root())->info().tagColors, expected);
        QCOMPARE(core::tagColor(expected, u"JOB/planning/q1"_s), u"red"_s);
    }

    /// Deleting a tag removes it and its children, and leaves other notes alone.
    void deleteTag()
    {
        core::Note note = core::makeNote();
        note.meta.tags = {u"draft"_s, u"draft/old"_s, u"keep"_s};
        core::Note other = core::makeNote();
        other.meta.tags = {u"drafts"_s};
        QVERIFY(m_session->saveNote(note));
        QVERIFY(m_session->saveNote(other));
        QCOMPARE(m_session->deleteTag(u"draft"_s).value(), 1);
        QCOMPARE(m_session->library().readNote(note.meta.id)->meta.tags, QStringList{u"keep"_s});
        QCOMPARE(m_session->library().readNote(other.meta.id)->meta.tags, QStringList{u"drafts"_s});
    }

    /// Deleting a tag removes its colour and its children's, and keeps a similar name's.
    void deleteTagRemovesColors()
    {
        core::Note note = core::makeNote();
        note.meta.tags = {u"draft/old"_s, u"drafts"_s};
        QVERIFY(m_session->saveNote(note));
        QVERIFY(m_session->updateInfo([](core::LibraryInfo &info) {
            info.tagColors = {
                {u"draft"_s, u"red"_s}, {u"draft/old"_s, u"sky"_s}, {u"drafts"_s, u"green"_s}};
        }));
        QVERIFY(m_session->deleteTag(u"draft"_s));
        QCOMPARE(m_session->library().info().tagColors,
                 (QMap<QString, QString>{{u"drafts"_s, u"green"_s}}));
    }

    /// Bulk writes use another thread and connection while the session event loop stays usable.
    void bulkMutationRunsOffThread()
    {
        const QTemporaryDir dir;
        auto library = core::Library::create(dir.path());
        QVERIFY(library);
        auto opened = core::LibrarySession::open(dir.path());
        QVERIFY(opened);
        auto &session = **opened;
        QSignalSpy ready(&session, &core::LibrarySession::notesChanged);
        QVERIFY(ready.wait());
        core::Note note = core::makeNote();
        QVERIFY(session.saveNote(note));
        bool completed = false;
        bool outside = false;
        QThread *owner = QThread::currentThread();
        session.mutate(
            [&outside, owner, id = note.meta.id](core::LibrarySession &worker) {
                outside = QThread::currentThread() != owner;
                return worker.updateNotes(
                    {id}, [](core::NoteMeta &meta) { meta.tags = {QStringLiteral("bulk")}; });
            },
            [&completed](const core::Result<void> &result) { completed = result.has_value(); });
        QVERIFY(session.isMutating());
        QTRY_VERIFY(completed);
        QVERIFY(outside);
        QVERIFY(!session.isMutating());
        QCOMPARE(session.library().readNote(note.meta.id)->meta.tags,
                 QStringList{QStringLiteral("bulk")});
    }

    /// In-place file changes and recreated note folders both reach the watcher.
    void watcherRestoresSubscriptions()
    {
        const QTemporaryDir dir;
        auto library = core::Library::create(dir.path());
        QVERIFY(library);
        core::Note note = core::makeNote();
        QVERIFY(library->writeNote(note));
        core::LibraryWatcher watcher(dir.path());
        QSignalSpy changes(&watcher, &core::LibraryWatcher::changed);
        QTRY_VERIFY(!changes.isEmpty());
        changes.clear();
        QFile file(library->notePath(note.meta.id));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Append));
        QCOMPARE(file.write(" "), 1);
        file.close();
        QTRY_VERIFY(!changes.isEmpty());
        changes.clear();
        QVERIFY(QDir().rename(library->notesDir(), library->notesDir() + QStringLiteral("-old")));
        QVERIFY(QDir().mkdir(library->notesDir()));
        QVERIFY(library->writeNote(note));
        QTRY_VERIFY(!changes.isEmpty());
        changes.clear();
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Append));
        QCOMPARE(file.write(" "), 1);
        file.close();
        QTRY_VERIFY(!changes.isEmpty());
    }

private:
    std::optional<QTemporaryDir> m_dir;
    std::unique_ptr<core::LibrarySession> m_session;
};

QTEST_GUILESS_MAIN(SessionTest)
#include "tst_session.moc"
