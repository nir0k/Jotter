#include "core/Files.h"
#include "core/LibrarySession.h"
#include "core/Rules.h"

#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;

namespace {

core::Rule ruleWith(QList<core::RuleCondition> conditions, const QStringList &targets,
                    core::RuleAction::Kind kind = core::RuleAction::Kind::Add)
{
    core::Rule rule;
    rule.id = core::newId();
    rule.name = u"Rule"_s;
    rule.conditions = std::move(conditions);
    rule.action = {.kind = kind, .collections = targets};
    rule.created = core::now();
    rule.updated = rule.created;
    return rule;
}

core::Collection collectionNamed(const QString &name)
{
    core::Collection collection;
    collection.id = core::newId();
    collection.name = name;
    collection.created = core::now();
    collection.updated = collection.created;
    return collection;
}

} // namespace

/// Checks rules: conditions, when a rule acts, its actions, the rules file, and
/// rules running as notes are saved, with Undo and Apply to existing notes.
class RulesTest : public QObject
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
        m_work = collectionNamed(u"Work"_s);
        m_ideas = collectionNamed(u"Ideas"_s);
        QVERIFY(m_session->library().writeCollections({m_work, m_ideas}));
        m_session->refresh();
        QTRY_COMPARE(m_session->collections().size(), 2);
    }

    void cleanup()
    {
        m_session.reset();
        m_dir.reset();
    }

    /// Tag conditions, joined by All or Any: a tag matches its children and ignores case.
    void conditions()
    {
        using Kind = core::RuleCondition::Kind;
        core::Rule rule = ruleWith({{.kind = Kind::TagEquals, .value = u"#Work"_s},
                                    {.kind = Kind::TagEquals, .value = u"plans"_s}},
                                   {});
        QVERIFY(core::RuleEngine({rule}).matches(0, {u"work/planning"_s, u"Plans"_s}));
        QVERIFY(!core::RuleEngine({rule}).matches(0, {u"workshop"_s, u"plans"_s}));
        rule.matchAll = false;
        QVERIFY(core::RuleEngine({rule}).matches(0, {u"WORK"_s}));

        const core::RuleEngine none({ruleWith({{.kind = Kind::NoTags, .value = {}}}, {})});
        QVERIFY(none.matches(0, {}));
        QVERIFY(!none.matches(0, {u"t"_s}));
    }

    /// A rule acts only when the note starts matching it; Stop skips the rules after it; Move
    /// replaces the collections.
    void acting()
    {
        using Kind = core::RuleCondition::Kind;
        const QList<core::Collection> collections{m_work, m_ideas};
        core::Rule work = ruleWith({{.kind = Kind::TagEquals, .value = u"meet"_s}}, {m_work.id});
        work.stop = true;
        const core::Rule ideas =
            ruleWith({{.kind = Kind::TagEquals, .value = u"meet"_s}}, {m_ideas.id});
        const core::RuleEngine engine({work, ideas});

        core::NoteMeta meta;
        meta.tags = {u"meet"_s};
        QVERIFY(engine.apply(std::nullopt, meta, collections));
        QCOMPARE(meta.collections, QStringList{m_work.id});

        // Taken out of the collection by hand, the note stays out after the next edit.
        meta.collections.clear();
        meta.tags = {u"meet"_s, u"today"_s};
        QVERIFY(!engine.apply(QStringList{u"meet"_s}, meta, collections));
        QVERIFY(meta.collections.isEmpty());

        meta.archived = core::now();
        QVERIFY(!engine.apply(std::nullopt, meta, collections));

        core::NoteMeta moved;
        moved.tags = {u"meet"_s};
        moved.collections = {m_ideas.id};
        const core::Rule move = ruleWith({{.kind = Kind::TagEquals, .value = u"meet"_s}},
                                         {m_work.id}, core::RuleAction::Kind::Move);
        QVERIFY(core::RuleEngine({move}).apply(std::nullopt, moved, collections));
        QCOMPARE(moved.collections, QStringList{m_work.id});
    }

    /// `rules.json` reads back what was written, keeping fields it does not know. A rule with
    /// conditions of earlier versions is unsupported: it never matches and keeps its conditions
    /// and `caseSensitive`; an unknown condition makes the file unreadable.
    void file()
    {
        core::Rule rule = ruleWith({{.kind = core::RuleCondition::Kind::TagEquals, .value = u"x"_s},
                                    {.kind = core::RuleCondition::Kind::NoTags, .value = {}}},
                                   {m_work.id}, core::RuleAction::Kind::Move);
        rule.matchAll = false;
        rule.stop = true;
        rule.extra.insert(u"future"_s, 1);
        const auto parsed = core::parseRules(core::serializeRules({rule}), u"rules.json"_s);
        QVERIFY(parsed);
        QCOMPARE(*parsed, QList<core::Rule>{rule});

        const QByteArray legacy = core::serializeRules(*core::parseRules(
            R"({"format": 1, "rules": [{"id": "01a1872b-97b4-71f5-be24-84e81a41f47a",
                "match": "all", "caseSensitive": true, "stop": false,
                "created": "2026-10-20T08:02:00.000Z", "updated": "2026-10-20T08:02:00.000Z",
                "conditions": [{"field": "tag", "op": "equals", "value": "x"},
                               {"field": "title", "op": "contains", "value": "x"}],
                "action": {"kind": "add", "collections": []}}]})",
            u"rules.json"_s));
        const auto unsupported = core::parseRules(legacy, u"rules.json"_s);
        QVERIFY(unsupported);
        QVERIFY(!unsupported->first().supported);
        QVERIFY(unsupported->first().conditions.isEmpty());
        QVERIFY(!core::RuleEngine(*unsupported).matches(0, {u"x"_s}));
        QCOMPARE(core::serializeRules(*unsupported), legacy);
        QVERIFY(legacy.contains(R"("caseSensitive": true)"));
        QVERIFY(legacy.contains(R"("op": "contains")"));

        QCOMPARE(core::parseRules(R"({"format": 1, "rules": [{"id": "x"}]})", u"rules.json"_s)
                     .error()
                     .code,
                 core::ErrorCode::Malformed);
        QCOMPARE(core::parseRules(
                     R"({"format": 1, "rules": [{"id": "01a1872b-97b4-71f5-be24-84e81a41f47a",
                "match": "all", "created": "2026-10-20T08:02:00.000Z",
                "updated": "2026-10-20T08:02:00.000Z",
                "conditions": [{"field": "title", "op": "starts", "value": "x"}],
                "action": {"kind": "add", "collections": []}}]})",
                     u"rules.json"_s)
                     .error()
                     .code,
                 core::ErrorCode::Malformed);
    }

    /// Saving a note runs the rules on a change of its tags, announces the change and
    /// can undo it; Apply to existing notes files the notes that match.
    void session()
    {
        core::Note before = core::makeNote();
        before.meta.tags = {u"work"_s};
        QVERIFY(m_session->saveNote(before));

        const core::Rule rule = ruleWith(
            {{.kind = core::RuleCondition::Kind::TagEquals, .value = u"work"_s}}, {m_work.id});
        QVERIFY(m_session->saveRules({rule}));
        QCOMPARE(m_session->library().readRules()->size(), 1);

        QSignalSpy applied(m_session.get(), &core::LibrarySession::rulesApplied);
        core::Note note = core::makeNote();
        note.meta.title = u"Plan"_s;
        QVERIFY(m_session->saveNote(note));
        QVERIFY(applied.isEmpty());
        QVERIFY(m_session->changeTags({note.meta.id}, {u"Work/plans"_s}, {}));
        QCOMPARE(applied.size(), 1);
        QCOMPARE(m_session->library().readNote(note.meta.id)->meta.collections,
                 QStringList{m_work.id});

        const auto changes = applied.first().first().value<QList<core::RuleChange>>();
        QVERIFY(m_session->undoRules(changes));
        QVERIFY(m_session->library().readNote(note.meta.id)->meta.collections.isEmpty());
        QCOMPARE(applied.size(), 1);

        // The note saved before the rule existed matches it too.
        const auto ids = m_session->notesForRule(rule);
        QVERIFY(ids);
        QCOMPARE(ids->size(), 2);
        QVERIFY(m_session->applyRule(rule, *ids));
        QCOMPARE(m_session->library().readNote(before.meta.id)->meta.collections,
                 QStringList{m_work.id});
        QCOMPARE(applied.size(), 2);
    }

private:
    std::optional<QTemporaryDir> m_dir;
    std::unique_ptr<core::LibrarySession> m_session;
    core::Collection m_work;
    core::Collection m_ideas;
};

QTEST_GUILESS_MAIN(RulesTest)
#include "tst_rules.moc"
