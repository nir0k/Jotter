#include "core/Files.h"
#include "core/LibrarySession.h"
#include "core/NoteLinks.h"
#include "core/TableOfContents.h"

#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;

namespace {

QString linkTo(const core::Note &note, const QString &text)
{
    return u"<a class=\"note-link\" href=\"%1\">%2</a>"_s.arg(core::noteLinkAddress(note.meta.id),
                                                              text);
}

core::NoteSummary summaryNamed(const QString &title)
{
    core::NoteSummary note;
    note.id = core::newId();
    note.title = title;
    note.updated = core::now();
    return note;
}

} // namespace

/// Checks web links, links between notes and the generated table of contents.
class LinksTest : public QObject
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

    /// Only `<note-id>.html` addresses are note links.
    void addresses()
    {
        const QString id = core::newId();
        QCOMPARE(core::noteLinkTarget(core::noteLinkAddress(id)), id);
        QVERIFY(!core::noteLinkTarget(u"https://example.com/a.html"_s));
        QVERIFY(!core::noteLinkTarget(u"index.html"_s));
    }

    /// Typed addresses are normalised and only web and mail links are accepted, typed or read.
    void webLinks()
    {
        QCOMPARE(core::normaliseLink(u"example.com/a"_s), u"https://example.com/a"_s);
        QCOMPARE(core::normaliseLink(u"me@example.com"_s), u"mailto:me@example.com"_s);
        QCOMPARE(core::normaliseLink(u"http://x.org"_s), u"http://x.org"_s);
        QVERIFY(core::normaliseLink(u"javascript:alert(1)"_s).isEmpty());
        QVERIFY(core::normaliseLink(u"file:///etc/passwd"_s).isEmpty());
        QVERIFY(core::isWebLink(u"mailto:me@example.com"_s));
        QVERIFY(!core::isWebLink(u"example.com"_s));
        QVERIFY(!core::isWebLink(u"file:///home/me/run.desktop"_s));
    }

    /// The index lists the notes linking to a note, leaving out the trash unless asked.
    void backlinks()
    {
        core::Note target = core::makeNote();
        target.meta.title = u"Target"_s;
        QVERIFY(m_session->saveNote(target));
        core::Note source = core::makeNote();
        source.meta.title = u"Source"_s;
        source.body = u"<p>See %1 and %1.</p>"_s.arg(linkTo(target, u"Target"_s));
        QVERIFY(m_session->saveNote(source));
        core::Note trashed = core::makeNote();
        trashed.body = u"<p>%1</p>"_s.arg(linkTo(target, u"it"_s));
        trashed.meta.trashed = core::now();
        QVERIFY(m_session->saveNote(trashed));

        const auto links = m_session->index().backlinks(target.meta.id);
        QVERIFY(links && links->size() == 1);
        QCOMPARE(links->first().id, source.meta.id);
        QCOMPARE(m_session->index().backlinks(target.meta.id, true)->size(), 2);
        const auto known = m_session->index().notesById({target.meta.id, core::newId()});
        QVERIFY(known && known->size() == 1 && known->contains(target.meta.id));
    }

    /// Renaming a note renames the links that show its old title; links with text of their
    /// own keep it.
    void renaming()
    {
        core::Note target = core::makeNote();
        target.meta.title = u"Plan <A>"_s;
        QVERIFY(m_session->saveNote(target));
        core::Note source = core::makeNote();
        source.body = u"<p>%1 %2</p>"_s.arg(linkTo(target, u"Plan &lt;A&gt;"_s),
                                            linkTo(target, u"see here"_s));
        QVERIFY(m_session->saveNote(source));

        target.meta.title = u"Plan B"_s;
        QVERIFY(m_session->saveNote(target));
        QTRY_COMPARE(
            m_session->library().readNote(source.meta.id)->body,
            u"<p>%1 %2</p>"_s.arg(linkTo(target, u"Plan B"_s), linkTo(target, u"see here"_s)));
    }

    /// The table of contents lists notes under each of their collections, nested collections
    /// as nested headings, then notes without a collection and the archive, leaving out the
    /// trash and empty collections.
    void tableOfContents()
    {
        core::Collection work;
        work.id = core::newId();
        work.name = u"Work"_s;
        core::Collection meetings;
        meetings.id = core::newId();
        meetings.parent = work.id;
        meetings.name = u"Meetings"_s;
        core::Collection empty;
        empty.id = core::newId();
        empty.name = u"Empty"_s;

        core::NoteSummary standup = summaryNamed(u"Standup"_s);
        standup.collections = {meetings.id, work.id};
        standup.tags = {u"daily"_s};
        core::NoteSummary loose = summaryNamed(u"Loose"_s);
        core::NoteSummary old = summaryNamed(u"Old"_s);
        old.archived = true;
        core::NoteSummary gone = summaryNamed(u"Gone"_s);
        gone.trashed = true;

        const QString page = QString::fromUtf8(
            core::tableOfContents({standup, loose, old, gone}, {work, meetings, empty}));
        QCOMPARE(page.count(u"notes/%1"_s.arg(core::noteLinkAddress(standup.id))), 2);
        QVERIFY(page.indexOf(u"<h2>Work</h2>"_s) < page.indexOf(u"<h3>Meetings</h3>"_s));
        QVERIFY(page.contains(u"#daily"_s));
        QVERIFY(page.indexOf(u"Loose"_s) < page.indexOf(u"Old"_s));
        QVERIFY(!page.contains(u"Gone"_s));
        QVERIFY(!page.contains(u"Empty"_s));
    }

private:
    std::optional<QTemporaryDir> m_dir;
    std::unique_ptr<core::LibrarySession> m_session;
};

QTEST_GUILESS_MAIN(LinksTest)
#include "tst_links.moc"
