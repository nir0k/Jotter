#include "core/Files.h"
#include "core/Index.h"
#include "core/Search.h"
#include "core/Stemmer.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace {

core::Note noteWith(const QString &title, const QString &text, const QStringList &tags = {})
{
    core::Note note = core::makeNote();
    note.meta.title = title;
    note.meta.tags = tags;
    note.body = QStringLiteral("<p>%1</p>").arg(text);
    return note;
}

core::Search searchFor(const QString &query, core::SearchScopes scopes = core::AllScopes)
{
    core::Search search;
    search.query = query;
    search.scopes = scopes;
    return search;
}

} // namespace

/// Checks search: word forms, the query syntax, scopes, filters, ranking and
/// keeping the full-text index in step with changed and removed notes.
class SearchTest : public QObject
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

    /// Word forms meet in one stem; case and diacritics are ignored, ё reads as е, й stays.
    void stems()
    {
        core::Stemmer stemmer;
        const QString note = stemmer.stem(u"заметка");
        QCOMPARE(stemmer.stem(u"заметок"), note);
        QCOMPARE(stemmer.stem(u"Заметке"), note);
        QCOMPARE(stemmer.stem(u"notes"), stemmer.stem(u"Note"));
        QCOMPARE(stemmer.stem(u"Ёлка"), stemmer.stem(u"елка"));
        QCOMPARE(stemmer.stem(u"Café"), stemmer.stem(u"cafe"));
        QVERIFY(stemmer.stem(u"йод") != stemmer.stem(u"иод"));
        QCOMPARE(core::splitWords(u"e-mail, ok").size(), 3);
    }

    /// The query splits into words, phrases, exclusions and tags; only the term that ends the
    /// query is a prefix.
    void parsing()
    {
        using Kind = core::SearchTerm::Kind;
        const QList<core::SearchTerm> terms =
            core::parseQuery(u"plan \"weekly review\" -draft #Work/ - \"open");
        QCOMPARE(terms.size(), 5);
        QCOMPARE(terms.at(0), (core::SearchTerm{Kind::Word, QStringLiteral("plan"), false, false}));
        QCOMPARE(terms.at(1),
                 (core::SearchTerm{Kind::Phrase, QStringLiteral("weekly review"), false, false}));
        QCOMPARE(terms.at(2), (core::SearchTerm{Kind::Word, QStringLiteral("draft"), false, true}));
        QCOMPARE(terms.at(3), (core::SearchTerm{Kind::Tag, QStringLiteral("Work"), false, false}));
        QCOMPARE(terms.at(4),
                 (core::SearchTerm{Kind::Phrase, QStringLiteral("open"), true, false}));
        QVERIFY(core::parseQuery(u"plan").at(0).prefix);
        QVERIFY(!core::parseQuery(u"plan ").at(0).prefix);
        QVERIFY(!core::Search().isActive());
        QVERIFY(!searchFor(QStringLiteral(" - # ")).isActive());
    }

    /// The editor highlights every form of the words and a phrase only where its words meet.
    void highlights()
    {
        const core::TermMatcher matcher(core::parseQuery(u"заметка \"weekly plan\""));
        const QList<core::TextSpan> spans = matcher.find(u"Заметки, plan; weekly plans");
        QCOMPARE(spans, (QList<core::TextSpan>{{.start = 0, .length = 7},
                                               {.start = 15, .length = 6},
                                               {.start = 22, .length = 5}}));
    }

    /// Search finds word forms, phrases, prefixes and tags, and leaves out excluded words and
    /// archived notes unless asked.
    void query()
    {
        const core::Note meeting =
            noteWith(QStringLiteral("Weekly planning"), QStringLiteral("Заметки о встрече"),
                     {QStringLiteral("work/meetings")});
        const core::Note trip =
            noteWith(QStringLiteral("Shopping"), QStringLiteral("notes for the trip"));
        core::Note archived = noteWith(QStringLiteral("Old"), QStringLiteral("заметка"));
        archived.meta.archived = core::now();
        for (const core::Note &note : {meeting, trip, archived}) {
            QVERIFY(m_library->writeNote(note));
        }
        QVERIFY(m_index->scan(*m_library));

        QCOMPARE(find(QStringLiteral("заметок")), QStringList{meeting.meta.id});
        core::Search withArchive = searchFor(QStringLiteral("заметок"));
        withArchive.includeArchived = true;
        QCOMPARE(find(withArchive).size(), 2);
        QCOMPARE(find(QStringLiteral("note")), QStringList{trip.meta.id});
        QCOMPARE(find(QStringLiteral("встр")), QStringList{meeting.meta.id});
        QVERIFY(find(QStringLiteral("встр ")).isEmpty());
        QCOMPARE(find(QStringLiteral("\"for the trip\"")), QStringList{trip.meta.id});
        QVERIFY(find(QStringLiteral("\"trip for\"")).isEmpty());
        QCOMPARE(find(QStringLiteral("-заметка")), QStringList{trip.meta.id});
        QCOMPARE(find(QStringLiteral("#work")), QStringList{meeting.meta.id});
        QCOMPARE(find(QStringLiteral("#wor")), QStringList{meeting.meta.id});
        QVERIFY(find(QStringLiteral("#wor ")).isEmpty());
        QVERIFY(find(QStringLiteral("NEAR AND \"")).isEmpty());

        // The list shows the fragment around the match with the word marked.
        const auto results = m_index->search({}, searchFor(QStringLiteral("встреча")), {}, {});
        QVERIFY(results && results->size() == 1);
        const core::NoteSummary &found = results->first();
        QCOMPARE(found.matches.size(), 1);
        QCOMPARE(found.snippet.sliced(found.matches.first().start, found.matches.first().length),
                 QStringLiteral("встрече"));
    }

    /// Scopes choose the columns; a collection name finds the notes of its descendants too.
    void scopes()
    {
        core::Collection projects;
        projects.id = QStringLiteral("p");
        projects.name = QStringLiteral("Проекты");
        core::Collection site;
        site.id = QStringLiteral("s");
        site.parent = projects.id;
        site.name = QStringLiteral("Site");
        const QList<core::Collection> collections{projects, site};
        core::Note design = noteWith(QStringLiteral("Design"), QStringLiteral("colours"),
                                     {QStringLiteral("ideas")});
        design.meta.collections = {site.id};
        QVERIFY(m_library->writeNote(design));
        QVERIFY(m_index->scan(*m_library));

        using core::SearchScope;
        QVERIFY(find(searchFor(QStringLiteral("colours"), SearchScope::Title)).isEmpty());
        QCOMPARE(find(searchFor(QStringLiteral("colour"), SearchScope::Text)).size(), 1);
        QCOMPARE(find(searchFor(QStringLiteral("idea"), SearchScope::Tags)).size(), 1);
        QVERIFY(find(searchFor(QStringLiteral("проект"), SearchScope::Text)).isEmpty());
        QCOMPARE(find(searchFor(QStringLiteral("проект design"),
                                SearchScope::Collections | SearchScope::Title),
                      collections),
                 QStringList{design.meta.id});
        QVERIFY(
            find(searchFor(QStringLiteral("проект -site"), SearchScope::Collections), collections)
                .isEmpty());
    }

    /// Filters narrow the notes without a query and keep the list's order.
    void filters()
    {
        core::Note old = noteWith(QStringLiteral("Old"), QStringLiteral("text"),
                                  {QStringLiteral("work/meetings")});
        old.meta.created = QDateTime::currentDateTimeUtc().addDays(-10);
        const core::Note fresh =
            noteWith(QStringLiteral("Fresh"), QStringLiteral("text"), {QStringLiteral("home")});
        QVERIFY(m_library->writeNote(old));
        QVERIFY(m_library->writeNote(fresh));
        QVERIFY(m_index->scan(*m_library));

        core::Search search;
        search.createdFrom = QDateTime::currentDateTimeUtc().addDays(-1);
        QCOMPARE(find(search), QStringList{fresh.meta.id});
        search = {};
        search.tags = {QStringLiteral("Work"), QStringLiteral("garden")};
        QCOMPARE(find(search), QStringList{old.meta.id});
        search.withAttachments = true;
        QVERIFY(find(search).isEmpty());
    }

    /// A title match ranks above a match in the text.
    void ranking()
    {
        const core::Note inText = noteWith(QStringLiteral("Other"), QStringLiteral("alpha beta"));
        const core::Note inTitle = noteWith(QStringLiteral("Alpha"), QStringLiteral("gamma"));
        QVERIFY(m_library->writeNote(inText));
        QVERIFY(m_library->writeNote(inTitle));
        QVERIFY(m_index->scan(*m_library));
        QCOMPARE(find(QStringLiteral("alpha")), (QStringList{inTitle.meta.id, inText.meta.id}));
    }

    /// Changing or removing a note changes what search finds, and a rebuilt index finds the
    /// same notes.
    void staysInStep()
    {
        core::Note note = noteWith(QStringLiteral("Draft"), QStringLiteral("apples"));
        QVERIFY(m_library->writeNote(note));
        QVERIFY(m_index->scan(*m_library));
        note.body = QStringLiteral("<p>pears</p>");
        QVERIFY(m_index->put(note, m_library->notePath(note.meta.id)));
        QVERIFY(find(QStringLiteral("apples")).isEmpty());
        QCOMPARE(find(QStringLiteral("pears")).size(), 1);
        QVERIFY(m_index->clear());
        QVERIFY(m_index->scan(*m_library));
        QCOMPARE(find(QStringLiteral("apples")).size(), 1);
        QVERIFY(QFile::remove(m_library->notePath(note.meta.id)));
        QVERIFY(m_index->scan(*m_library));
        QVERIFY(find(QStringLiteral("apples")).isEmpty());
    }

private:
    /// Searches all notes and returns the identifiers of the results in their order.
    QStringList find(const core::Search &search, const QList<core::Collection> &collections = {})
    {
        const auto results = m_index->search({}, search, collections, {});
        if (!results) {
            qWarning() << results.error().detail;
            return {QStringLiteral("error")};
        }
        QStringList ids;
        for (const core::NoteSummary &note : *results) {
            ids.append(note.id);
        }
        return ids;
    }

    QStringList find(const QString &query) { return find(searchFor(query)); }

    std::optional<QTemporaryDir> m_dir;
    std::optional<core::Library> m_library;
    std::optional<core::Index> m_index;
};

QTEST_GUILESS_MAIN(SearchTest)
#include "tst_search.moc"
