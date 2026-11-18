#include "core/Directory.h"
#include "core/Files.h"
#include "core/Index.h"
#include "core/PlainText.h"

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

namespace {

/// Gives a file a modification time in the past, so a rewrite within the same millisecond is
/// still seen as a change by the scan.
void ageFile(const QString &path)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-3600),
                             QFileDevice::FileModificationTime));
}

core::Note noteWith(const QString &title)
{
    core::Note note = core::makeNote();
    note.meta.title = title;
    note.body = QStringLiteral("<p>%1 body</p>").arg(title);
    return note;
}

} // namespace

/// Checks the search index: incremental scans, problems, the views of the note list, tag counts
/// and rebuilding an outdated index.
class IndexTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        m_dir.emplace();
        auto library = core::Library::create(m_dir->filePath(QStringLiteral("lib")));
        QVERIFY(library);
        m_library.emplace(std::move(*library));
        auto index = core::Index::open(m_dir->filePath(QStringLiteral("index.sqlite")));
        QVERIFY(index);
        m_index.emplace(std::move(*index));
    }

    void cleanup()
    {
        m_index.reset();
        m_library.reset();
        m_dir.reset();
    }

    /// A scan adds new files, reads only changed ones again and removes vanished ones.
    void incrementalScan()
    {
        const core::Note a = noteWith(QStringLiteral("A"));
        core::Note b = noteWith(QStringLiteral("B"));
        QVERIFY(m_library->writeNote(a));
        QVERIFY(m_library->writeNote(b));
        auto report = m_index->scan(*m_library);
        QVERIFY(report);
        QCOMPARE(report->added, 2);

        report = m_index->scan(*m_library);
        QCOMPARE(report->read, 0);

        ageFile(m_library->notePath(b.meta.id));
        b.meta.title = QStringLiteral("B2");
        QVERIFY(m_library->writeNote(b));
        QVERIFY(QFile::remove(m_library->notePath(a.meta.id)));
        report = m_index->scan(*m_library);
        QCOMPARE(report->read, 1);
        QCOMPARE(report->updated, 1);
        QCOMPARE(report->removed, 1);
        const auto notes = m_index->notes({}, {});
        QCOMPARE(notes->size(), 1);
        QCOMPARE(notes->first().title, QStringLiteral("B2"));
        QCOMPARE(notes->first().snippet, QStringLiteral("B body"));
    }

    /// A damaged file is listed as a problem instead of a note, and leaves the list once fixed
    /// or removed; the scan report counts each change of the list.
    void problemsComeAndGo()
    {
        const core::Note note = noteWith(QStringLiteral("Fine"));
        const QString path = m_library->notePath(note.meta.id);
        QVERIFY(core::writeFileAtomic(path, "<html>not a note</html>"));
        auto report = m_index->scan(*m_library);
        QVERIFY(report);
        QCOMPARE(report->problems, 1);
        QCOMPARE(m_index->problems()->size(), 1);
        QCOMPARE(m_index->count({}).value(), 0);
        QCOMPARE(m_index->scan(*m_library)->problems, 0);

        ageFile(path);
        QVERIFY(m_library->writeNote(note));
        report = m_index->scan(*m_library);
        QVERIFY(report);
        QCOMPARE(report->problems, 1);
        QVERIFY(m_index->problems()->isEmpty());
        QCOMPARE(m_index->count({}).value(), 1);

        const QString other = m_library->notePath(core::newId());
        QVERIFY(core::writeFileAtomic(other, "<html>not a note</html>"));
        QCOMPARE(m_index->scan(*m_library)->problems, 1);
        QVERIFY(QFile::remove(other));
        QCOMPARE(m_index->scan(*m_library)->problems, 1);
        QVERIFY(m_index->problems()->isEmpty());
    }

    /// Archived and trashed notes leave ordinary views,
    /// Unfiled notes are the active notes in no collection, and tag views include child tags.
    void views()
    {
        const QString work = core::newId();
        core::Note plain = noteWith(QStringLiteral("plain"));
        plain.meta.tags = {QStringLiteral("Work/Meetings")};
        core::Note filed = noteWith(QStringLiteral("filed"));
        filed.meta.collections = {work};
        filed.meta.tags = {QStringLiteral("work")};
        core::Note archived = noteWith(QStringLiteral("archived"));
        archived.meta.archived = core::now();
        archived.meta.collections = {work};
        core::Note trashed = noteWith(QStringLiteral("trashed"));
        trashed.meta.trashed = core::now();
        trashed.meta.tags = {QStringLiteral("work")};
        for (const core::Note *note : {&plain, &filed, &archived, &trashed}) {
            QVERIFY(m_library->writeNote(*note));
        }
        QVERIFY(m_index->scan(*m_library));

        const auto titles = [this](const core::NoteFilter &filter) {
            QStringList list;
            for (const core::NoteSummary &s :
                 m_index->notes(filter, {.key = core::NoteSort::Key::Title, .descending = false})
                     .value()) {
                list.append(s.title);
            }
            return list;
        };
        using View = core::NoteFilter::View;
        QCOMPARE(titles({}), (QStringList{QStringLiteral("filed"), QStringLiteral("plain")}));
        QCOMPARE(titles({.view = View::Unfiled, .ids = {}, .tag = {}}),
                 QStringList{QStringLiteral("plain")});
        QCOMPARE(titles({.view = View::Collections, .ids = {work}, .tag = {}}),
                 QStringList{QStringLiteral("filed")});
        QCOMPARE(titles({.view = View::Tag, .ids = {}, .tag = QStringLiteral("WORK")}),
                 (QStringList{QStringLiteral("filed"), QStringLiteral("plain")}));
        QCOMPARE(titles({.view = View::Archive, .ids = {}, .tag = {}}),
                 QStringList{QStringLiteral("archived")});
        QCOMPARE(titles({.view = View::Trash, .ids = {}, .tag = {}}),
                 QStringList{QStringLiteral("trashed")});

        // The trashed note's tag is not counted; each spelling comes from the oldest note.
        const auto tags = m_index->tags().value();
        QCOMPARE(tags.size(), 2);
        QCOMPARE(tags.at(0).name, QStringLiteral("work"));
        QCOMPARE(tags.at(0).count, 1);
        QCOMPARE(tags.at(1).name, QStringLiteral("Work/Meetings"));
    }

    /// Collection counts include the descendants, count a note in two collections of one
    /// subtree once, leave out archived notes and count a collection outside the tree (its
    /// parent is missing) with its own descendants.
    void collectionCounts()
    {
        const auto collection = [](const QString &id, const std::optional<QString> &parent) {
            core::Collection result;
            result.id = id;
            result.parent = parent;
            return result;
        };
        const QList<core::Collection> collections{
            collection(QStringLiteral("work"), std::nullopt),
            collection(QStringLiteral("meetings"), QStringLiteral("work")),
            collection(QStringLiteral("empty"), std::nullopt),
            collection(QStringLiteral("lost"), QStringLiteral("gone")),
            collection(QStringLiteral("found"), QStringLiteral("lost"))};
        core::Note both = noteWith(QStringLiteral("both"));
        both.meta.collections = {QStringLiteral("work"), QStringLiteral("meetings")};
        core::Note meeting = noteWith(QStringLiteral("meeting"));
        meeting.meta.collections = {QStringLiteral("meetings")};
        core::Note archived = noteWith(QStringLiteral("archived"));
        archived.meta.collections = {QStringLiteral("meetings")};
        archived.meta.archived = core::now();
        core::Note found = noteWith(QStringLiteral("found"));
        found.meta.collections = {QStringLiteral("found")};
        for (const core::Note *note : {&both, &meeting, &archived, &found}) {
            QVERIFY(m_library->writeNote(*note));
        }
        QVERIFY(m_index->scan(*m_library));

        const QHash<QString, int> counts = m_index->collectionCounts(collections).value();
        QCOMPARE(counts.value(QStringLiteral("work")), 2);
        QCOMPARE(counts.value(QStringLiteral("meetings")), 2);
        QCOMPARE(counts.value(QStringLiteral("empty"), -1), 0);
        QCOMPARE(counts.value(QStringLiteral("lost")), 1);
        QCOMPARE(counts.value(QStringLiteral("found")), 1);
    }

    /// An index with an older schema is emptied and rebuilt on open.
    void outdatedSchemaRebuilds()
    {
        QVERIFY(m_library->writeNote(noteWith(QStringLiteral("x"))));
        QVERIFY(m_index->scan(*m_library));
        m_index.reset();
        const QString path = m_dir->filePath(QStringLiteral("index.sqlite"));
        {
            QSqlDatabase db =
                QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("raw"));
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery(db).exec(QStringLiteral("PRAGMA user_version = 0"));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("raw"));
        auto index = core::Index::open(path);
        QVERIFY(index);
        QCOMPARE(index->count({}).value(), 0);
        QCOMPARE(index->scan(*m_library)->added, 1);
    }

    /// Body text is extracted with blocks on separate lines and references decoded.
    void plainText()
    {
        QCOMPARE(core::plainTextFromHtml(
                     u"<h1>Title</h1><p>a &amp; b&nbsp;&lt;c&gt; &#x2014; <strong>d</strong></p>"
                     u"<ul><li>one</li><li>two</li></ul>"),
                 QStringLiteral("Title\na & b <c> — d\none\ntwo"));
        QCOMPARE(core::snippet(QStringLiteral("alpha beta gamma"), 12),
                 QStringLiteral("alpha beta…"));
    }

    /// An unreadable directory never clears the previous index, including an explicit rebuild.
    void failedEnumerationPreservesIndex()
    {
        core::Note note = core::makeNote();
        QVERIFY(m_library->writeNote(note));
        QVERIFY(m_index->scan(*m_library));
        QCOMPARE(m_index->count({}).value(), 1);
        const QString notes = m_library->notesDir();
        QVERIFY(QDir().rename(notes, notes + QStringLiteral("-old")));
        QVERIFY(core::writeFileAtomic(notes, "not a directory"));
        QVERIFY(!m_index->scan(*m_library));
        QCOMPARE(m_index->count({}).value(), 1);
        QVERIFY(!m_index->scan(*m_library, true));
        QCOMPARE(m_index->count({}).value(), 1);
    }

private:
    std::optional<QTemporaryDir> m_dir;
    std::optional<core::Library> m_library;
    std::optional<core::Index> m_index;
};

QTEST_GUILESS_MAIN(IndexTest)
#include "tst_index.moc"
