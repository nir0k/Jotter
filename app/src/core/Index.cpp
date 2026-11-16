#include "core/Index.h"

#include "core/Attachments.h"
#include "core/Directory.h"
#include "core/FileGuard.h"
#include "core/Files.h"
#include "core/PlainText.h"
#include "core/Search.h"
#include "core/Stemmer.h"
#include "core/Tags.h"

#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlDriver>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>

#include <array>
#include <atomic>
#include <ranges>
#include <sqlite3.h>
#include <sys/stat.h>

namespace core {

namespace {

/// Bumped whenever the tables change; an index with another version is rebuilt.
constexpr int SchemaVersion = 7;

constexpr qsizetype SnippetLength = 200;

/// A scan commits after reading this many notes, so the write lock is never held for long.
constexpr int ScanBatch = 200;

/// Separates list values inside a group_concat result; it cannot occur in identifiers or tags.
constexpr QChar ListSeparator(0x1F);

constexpr std::array Schema{
    // `seq` is the stable row number the full-text table refers to; `tags` holds the note's tags
    // as text for it.
    "CREATE TABLE notes ("
    " seq INTEGER PRIMARY KEY, id TEXT NOT NULL UNIQUE, file TEXT NOT NULL,"
    " size INTEGER NOT NULL, mtime INTEGER NOT NULL, title TEXT NOT NULL, icon TEXT, color TEXT,"
    " created INTEGER NOT NULL, updated INTEGER NOT NULL, archived INTEGER, trashed INTEGER,"
    " has_location INTEGER NOT NULL, attachments INTEGER NOT NULL,"
    " snippet TEXT NOT NULL, text TEXT NOT NULL, tags TEXT NOT NULL)",
    // The full-text index reads its content from `notes`, so the text is stored once; the
    // triggers keep it in step with every change of a note.
    "CREATE VIRTUAL TABLE notes_fts USING fts5(title, text, tags, content='notes',"
    " content_rowid='seq', tokenize='jotter')",
    "CREATE TRIGGER notes_fts_insert AFTER INSERT ON notes BEGIN"
    " INSERT INTO notes_fts (rowid, title, text, tags)"
    " VALUES (new.seq, new.title, new.text, new.tags); END",
    "CREATE TRIGGER notes_fts_delete AFTER DELETE ON notes BEGIN"
    " INSERT INTO notes_fts (notes_fts, rowid, title, text, tags)"
    " VALUES ('delete', old.seq, old.title, old.text, old.tags); END",
    "CREATE TRIGGER notes_fts_update AFTER UPDATE OF title, text, tags ON notes BEGIN"
    " INSERT INTO notes_fts (notes_fts, rowid, title, text, tags)"
    " VALUES ('delete', old.seq, old.title, old.text, old.tags);"
    " INSERT INTO notes_fts (rowid, title, text, tags)"
    " VALUES (new.seq, new.title, new.text, new.tags); END",
    "CREATE TABLE note_collections ("
    " note_id TEXT NOT NULL REFERENCES notes(id) ON DELETE CASCADE,"
    " collection_id TEXT NOT NULL, PRIMARY KEY (note_id, collection_id))",
    "CREATE INDEX note_collections_collection ON note_collections(collection_id)",
    "CREATE TABLE note_tags ("
    " note_id TEXT NOT NULL REFERENCES notes(id) ON DELETE CASCADE,"
    " tag TEXT NOT NULL, tag_key TEXT NOT NULL, PRIMARY KEY (note_id, tag_key))",
    "CREATE INDEX note_tags_key ON note_tags(tag_key)",
    // The attachment files a note refers to, by the folder (note) that holds them.
    "CREATE TABLE note_files ("
    " note_id TEXT NOT NULL REFERENCES notes(id) ON DELETE CASCADE,"
    " folder TEXT NOT NULL, file TEXT NOT NULL, PRIMARY KEY (note_id, folder, file))",
    "CREATE INDEX note_files_file ON note_files(folder, file)",
    // The notes a note links to, for backlinks.
    "CREATE TABLE note_links ("
    " note_id TEXT NOT NULL REFERENCES notes(id) ON DELETE CASCADE,"
    " target_id TEXT NOT NULL, PRIMARY KEY (note_id, target_id))",
    "CREATE INDEX note_links_target ON note_links(target_id)",
    // Attachment files no note refers to, since when; they are removed after the grace period.
    "CREATE TABLE orphans ("
    " folder TEXT NOT NULL, file TEXT NOT NULL, since INTEGER NOT NULL,"
    " PRIMARY KEY (folder, file))",
    "CREATE TABLE problems ("
    " file TEXT PRIMARY KEY, size INTEGER NOT NULL, mtime INTEGER NOT NULL,"
    " code INTEGER NOT NULL, detail TEXT NOT NULL)",
    // Facts about how the index was built, such as the stemmers' fingerprint.
    "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)",
};

/// The tables emptied by clear(); deleting the notes empties the full-text table through its
/// trigger.
constexpr std::array Tables{"note_tags", "note_collections", "note_files", "note_links",
                            "notes",     "orphans",          "problems"};

/// Every table of this and earlier schemas, dropped before the index is built again.
constexpr std::array DroppedTables{
    "notes_fts",  "note_tags", "note_collections", "note_files", "note_styles",
    "note_links", "notes",     "orphans",          "problems",   "meta"};

/// Weights of the title, text and tags columns in the relevance of a match.
constexpr QLatin1StringView Relevance("bm25(notes_fts, 10.0, 1.0, 5.0)");

/// Mark the matching words of a fragment; they cannot occur in a note's text.
constexpr QChar MatchStart(0x02);
constexpr QChar MatchEnd(0x03);

// The FTS5 tokenizer `jotter`: every word is reduced by the Stemmer, in the
// index as in queries, so all word forms meet. Each database connection creates its own
// tokenizer instances, so a stemmer is never shared between threads.

int createTokenizer(void * /*context*/, const char ** /*arguments*/, int /*count*/,
                    Fts5Tokenizer **tokenizer)
{
    *tokenizer = reinterpret_cast<Fts5Tokenizer *>(new (std::nothrow) Stemmer);
    return *tokenizer != nullptr ? SQLITE_OK : SQLITE_NOMEM;
}

void deleteTokenizer(Fts5Tokenizer *tokenizer)
{
    delete reinterpret_cast<Stemmer *>(tokenizer);
}

/// Splits UTF-8 text into stemmed tokens, reporting each word's position in bytes, which
/// snippet() uses to mark the matching words.
int tokenize(Fts5Tokenizer *tokenizer, void *context, int /*flags*/, const char *text, int length,
             int (*emit)(void *, int, const char *, int, int, int))
{
    auto *stemmer = reinterpret_cast<Stemmer *>(tokenizer);
    const QString string = QString::fromUtf8(text, length);
    // The UTF-8 offset of each UTF-16 position; a surrogate pair takes four bytes at its start.
    QList<int> offsets(string.size() + 1);
    int byte = 0;
    for (qsizetype i = 0; i < string.size(); ++i) {
        offsets[i] = byte;
        const char16_t c = string[i].unicode();
        if (c < 0x80) {
            byte += 1;
        } else if (c < 0x800) {
            byte += 2;
        } else if (QChar::isHighSurrogate(c)) {
            byte += 4;
        } else if (!QChar::isLowSurrogate(c)) {
            byte += 3;
        }
    }
    offsets[string.size()] = byte;
    for (const TextSpan &word : splitWords(string)) {
        const QByteArray stem =
            stemmer->stem(QStringView(string).sliced(word.start, word.length)).toUtf8();
        if (stem.isEmpty()) {
            continue;
        }
        const int rc = emit(context, 0, stem.constData(), static_cast<int>(stem.size()),
                            offsets.at(word.start), offsets.at(word.start + word.length));
        if (rc != SQLITE_OK) {
            return rc;
        }
    }
    return SQLITE_OK;
}

/// Registers the `jotter` tokenizer on a connection, before the full-text table is used.
///
/// Qt's SQLite driver hands out its `sqlite3` handle; Jotter links the same system SQLite
/// library, so the FTS5 interface can be reached through it.
Result<void> registerTokenizer(const QSqlDatabase &db)
{
    const QVariant handle = db.driver()->handle();
    if (!handle.isValid() || qstrcmp(handle.typeName(), "sqlite3*") != 0) {
        return fail(ErrorCode::Io, db.databaseName(),
                    QStringLiteral("the SQLite driver gives no connection handle"));
    }
    sqlite3 *connection = *static_cast<sqlite3 *const *>(handle.constData());
    fts5_api *api = nullptr;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(connection, "SELECT fts5(?1)", -1, &statement, nullptr) == SQLITE_OK) {
        sqlite3_bind_pointer(statement, 1, static_cast<void *>(&api), "fts5_api_ptr", nullptr);
        sqlite3_step(statement);
    }
    sqlite3_finalize(statement);
    if (api == nullptr) {
        return fail(ErrorCode::Io, db.databaseName(),
                    QStringLiteral("SQLite is built without FTS5"));
    }
    fts5_tokenizer tokenizer{&createTokenizer, &deleteTokenizer, &tokenize};
    if (api->xCreateTokenizer(api, "jotter", nullptr, &tokenizer, nullptr) != SQLITE_OK) {
        return fail(ErrorCode::Io, db.databaseName(),
                    QString::fromUtf8(sqlite3_errmsg(connection)));
    }
    return {};
}

/// Runs a prepared or plain statement, turning a database failure into an Io error.
Result<void> run(QSqlQuery &query, const QString &sql = {})
{
    const bool ok = sql.isEmpty() ? query.exec() : query.exec(sql);
    if (!ok) {
        return fail(ErrorCode::Io, query.lastQuery(), query.lastError().text());
    }
    return {};
}

/// Starts a transaction, turning a database failure into an Io error: statements run without
/// one would be committed one by one, and a later rollback could not undo them.
Result<void> begin(QSqlDatabase &db)
{
    if (!db.transaction()) {
        return fail(ErrorCode::Io, db.databaseName(), db.lastError().text());
    }
    return {};
}

/// Binds text to a NOT NULL column: Qt passes a null QString, such as the title of a new note,
/// to SQLite as NULL.
QVariant textValue(const QString &text)
{
    return text.isNull() ? QVariant(QStringLiteral("")) : QVariant(text);
}

QVariant timeValue(const std::optional<QDateTime> &time)
{
    return time ? QVariant(time->toMSecsSinceEpoch()) : QVariant(QMetaType(QMetaType::LongLong));
}

QDateTime timeFrom(const QVariant &value)
{
    return QDateTime::fromMSecsSinceEpoch(value.toLongLong(), QTimeZone::UTC);
}

std::optional<QString> optionalFrom(const QVariant &value)
{
    return value.isNull() ? std::nullopt : std::optional<QString>(value.toString());
}

QStringList listFrom(const QVariant &value)
{
    return value.isNull() ? QStringList() : value.toString().split(ListSeparator);
}

/// The size and modification time that decide whether a file must be read again.
struct FileStamp
{
    qint64 size = 0;
    qint64 mtime = 0;

    bool operator==(const FileStamp &) const = default;
};

FileStamp stampOf(const QFileInfo &info)
{
    struct stat status{};
    const QByteArray path = QFile::encodeName(info.absoluteFilePath());
    const qint64 modified =
        ::lstat(path.constData(), &status) == 0
            ? static_cast<qint64>(status.st_mtim.tv_sec) * 1000000000 + status.st_mtim.tv_nsec
            : -1;
    return {.size = info.size(), .mtime = modified};
}

/// Writes one note with its collections, tags, attachment files and links, replacing what the
/// index had for it.
Result<void> storeNote(QSqlDatabase &db, const Note &note, const QString &file, FileStamp stamp)
{
    const QString text = plainTextFromHtml(note.body);
    const BodyReferences references = bodyReferences(note.body);
    QSet<std::pair<QString, QString>> files;
    for (const AttachmentRef &ref : references.attachments) {
        files.insert({ref.noteId, ref.fileName});
    }
    QSqlQuery query(db);
    // An upsert keeps the row number, so the full-text table follows through its update trigger
    // (a replace would delete without firing the delete trigger).
    query.prepare(QStringLiteral(
        "INSERT INTO notes (id, file, size, mtime, title, icon, color, created, updated,"
        " archived, trashed, has_location, attachments, snippet, text, tags)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT (id) DO UPDATE SET file = excluded.file, size = excluded.size,"
        " mtime = excluded.mtime, title = excluded.title, icon = excluded.icon,"
        " color = excluded.color, created = excluded.created, updated = excluded.updated,"
        " archived = excluded.archived, trashed = excluded.trashed,"
        " has_location = excluded.has_location, attachments = excluded.attachments,"
        " snippet = excluded.snippet, text = excluded.text, tags = excluded.tags"));
    const NoteMeta &m = note.meta;
    for (const QVariant &value :
         {QVariant(m.id), QVariant(file), QVariant(stamp.size), QVariant(stamp.mtime),
          textValue(m.title), m.icon ? QVariant(*m.icon) : QVariant(QMetaType(QMetaType::QString)),
          m.color ? QVariant(*m.color) : QVariant(QMetaType(QMetaType::QString)),
          QVariant(m.created.toMSecsSinceEpoch()), QVariant(m.updated.toMSecsSinceEpoch()),
          timeValue(m.archived), timeValue(m.trashed), QVariant(m.location ? 1 : 0),
          QVariant(files.size()), textValue(snippet(text, SnippetLength)), textValue(text),
          textValue(m.tags.join(u'\n'))}) {
        query.addBindValue(value);
    }
    if (auto ok = run(query); !ok) {
        return ok;
    }
    for (const char *table : {"note_collections", "note_tags", "note_files", "note_links"}) {
        query.prepare(
            QStringLiteral("DELETE FROM %1 WHERE note_id = ?").arg(QLatin1StringView(table)));
        query.addBindValue(m.id);
        if (auto ok = run(query); !ok) {
            return ok;
        }
    }
    query.prepare(QStringLiteral("INSERT OR IGNORE INTO note_collections VALUES (?, ?)"));
    for (const QString &collection : m.collections) {
        query.addBindValue(m.id);
        query.addBindValue(collection);
        if (auto ok = run(query); !ok) {
            return ok;
        }
    }
    query.prepare(QStringLiteral("INSERT OR IGNORE INTO note_tags VALUES (?, ?, ?)"));
    for (const QString &tag : m.tags) {
        query.addBindValue(m.id);
        query.addBindValue(tag);
        query.addBindValue(tagKey(tag));
        if (auto ok = run(query); !ok) {
            return ok;
        }
    }
    query.prepare(QStringLiteral("INSERT OR IGNORE INTO note_files VALUES (?, ?, ?)"));
    for (const auto &[folder, name] : files) {
        query.addBindValue(m.id);
        query.addBindValue(folder);
        query.addBindValue(name);
        if (auto ok = run(query); !ok) {
            return ok;
        }
    }
    query.prepare(QStringLiteral("INSERT OR IGNORE INTO note_links VALUES (?, ?)"));
    for (const QString &target : references.noteLinks) {
        query.addBindValue(m.id);
        query.addBindValue(target);
        if (auto ok = run(query); !ok) {
            return ok;
        }
    }
    return {};
}

/// A condition of a WHERE clause with its bound values in their order.
using Condition = std::pair<QString, QVariantList>;

QString placeholders(qsizetype count)
{
    return QStringList(count, QStringLiteral("?")).join(u',');
}

/// Builds a condition on notes with any of @p tags or their children; with @p prefix, also tags
/// that merely start with one, for a tag still being typed in a query.
Condition tagCondition(const QStringList &tags, bool prefix)
{
    QStringList parts;
    QVariantList values;
    for (const QString &tag : tags) {
        const QString key = tagKey(tag);
        // substr() counts characters, not UTF-16 code units.
        const qsizetype length = key.toUcs4().size();
        if (prefix) {
            parts.append(QStringLiteral("substr(t.tag_key, 1, ?) = ?"));
            values << length << key;
        } else {
            parts.append(QStringLiteral("t.tag_key = ? OR substr(t.tag_key, 1, ?) = ?"));
            values << key << length + 1 << QString(key + u'/');
        }
    }
    return {QStringLiteral("EXISTS (SELECT 1 FROM note_tags t WHERE t.note_id = n.id AND (%1))")
                .arg(parts.join(QStringLiteral(" OR "))),
            values};
}

/// Builds a condition on notes in any of the collections @p ids.
Condition collectionCondition(const QStringList &ids)
{
    return {QStringLiteral("EXISTS (SELECT 1 FROM note_collections c"
                           " WHERE c.note_id = n.id AND c.collection_id IN (%1))")
                .arg(placeholders(ids.size())),
            QVariantList(ids.cbegin(), ids.cend())};
}

/// Builds the WHERE clause of a view and its bound values.
Condition whereClause(const NoteFilter &filter)
{
    QString active = QStringLiteral("n.archived IS NULL AND n.trashed IS NULL");
    if (filter.anyState) {
        active = QStringLiteral("1");
    } else if (filter.includeArchived) {
        active = QStringLiteral("n.trashed IS NULL");
    }
    switch (filter.view) {
    case NoteFilter::View::All:
        return {active, {}};
    case NoteFilter::View::Unfiled:
        return {active +
                    QStringLiteral(" AND NOT EXISTS"
                                   " (SELECT 1 FROM note_collections c WHERE c.note_id = n.id)"),
                {}};
    case NoteFilter::View::Notes:
        return {QStringLiteral("n.trashed IS NULL AND n.id IN (%1)")
                    .arg(placeholders(filter.ids.size())),
                QVariantList(filter.ids.cbegin(), filter.ids.cend())};
    case NoteFilter::View::Collections: {
        const auto [condition, values] = collectionCondition(filter.ids);
        return {active + QStringLiteral(" AND ") + condition, values};
    }
    case NoteFilter::View::Tag: {
        const auto [condition, values] = tagCondition({filter.tag}, false);
        return {active + QStringLiteral(" AND ") + condition, values};
    }
    case NoteFilter::View::Archive:
        return {QStringLiteral("n.archived IS NOT NULL AND n.trashed IS NULL"), {}};
    case NoteFilter::View::Trash:
        return {QStringLiteral("n.trashed IS NOT NULL"), {}};
    }
    return {active, {}};
}

/// Writes a term as an FTS5 string: quoted, so query syntax inside it is plain text, and with
/// `*` for a prefix (a quoted phrase takes it on its last word).
QString ftsString(const SearchTerm &term)
{
    QString text = term.text;
    text.replace(u'"', QStringLiteral("\"\""));
    return u'"' + text + u'"' + (term.prefix ? QStringLiteral("*") : QString());
}

/// Returns the FTS5 column filter of the scopes, or empty when no column is searched.
QString ftsColumns(SearchScopes scopes)
{
    QStringList columns;
    for (const auto &[scope, name] :
         {std::pair{SearchScope::Title, "title"}, std::pair{SearchScope::Text, "text"},
          std::pair{SearchScope::Tags, "tags"}}) {
        if (scopes.testFlag(scope)) {
            columns.append(QString::fromLatin1(name));
        }
    }
    return columns.isEmpty() ? QString() : QStringLiteral("{%1} : ").arg(columns.join(u' '));
}

/// Lists the collections whose names match a term, with their descendants.
QStringList collectionsMatching(SearchTerm term, const QList<Collection> &collections)
{
    term.exclude = false;
    const TermMatcher matcher({term});
    QStringList ids;
    for (const Collection &collection : collections) {
        if (!matcher.find(collection.name).isEmpty()) {
            ids.append(collectionSubtree(collections, collection.id));
        }
    }
    ids.removeDuplicates();
    return ids;
}

/// Turns a fragment marked by snippet() into one line of text and the spans of its matches.
QString fragmentText(const QString &marked, QList<TextSpan> &matches)
{
    QString text;
    text.reserve(marked.size());
    bool space = false;
    qsizetype start = -1;
    for (const QChar c : marked) {
        if (c == MatchEnd) {
            if (start >= 0 && text.size() > start) {
                matches.append({.start = start, .length = text.size() - start});
            }
            start = -1;
            continue;
        }
        if (c.isSpace() || c == MatchStart) {
            space = space || c != MatchStart;
            if (c == MatchStart) {
                if (space && !text.isEmpty()) {
                    text += u' ';
                }
                space = false;
                start = text.size();
            }
            continue;
        }
        if (space && !text.isEmpty()) {
            text += u' ';
        }
        space = false;
        text += c;
    }
    return text;
}

NoteSummary summaryFrom(const QSqlQuery &query)
{
    NoteSummary s;
    s.id = query.value(0).toString();
    s.title = query.value(1).toString();
    s.icon = optionalFrom(query.value(2));
    s.color = optionalFrom(query.value(3));
    s.created = timeFrom(query.value(4));
    s.updated = timeFrom(query.value(5));
    s.archived = !query.value(6).isNull();
    s.trashed = !query.value(7).isNull();
    s.hasLocation = query.value(8).toBool();
    s.attachments = query.value(9).toInt();
    s.snippet = query.value(10).toString();
    s.collections = listFrom(query.value(11));
    s.tags = listFrom(query.value(12));
    return s;
}

constexpr QLatin1StringView SummaryColumns(
    "n.id, n.title, n.icon, n.color, n.created, n.updated, n.archived, n.trashed,"
    " n.has_location, n.attachments, n.snippet,"
    " (SELECT group_concat(collection_id, char(31)) FROM note_collections WHERE note_id = n.id),"
    // Tags keep the note's order: they are inserted in it each time the note is written.
    " (SELECT group_concat(tag, char(31) ORDER BY rowid) FROM note_tags WHERE note_id = n.id)");

/// Lists the notes matching a condition in the order of a list.
Result<QList<NoteSummary>> listNotes(const QSqlDatabase &db, const Condition &where,
                                     const NoteSort &sort)
{
    const QString order = sort.key == NoteSort::Key::Created ? QStringLiteral("n.created")
                                                             : QStringLiteral("n.updated");
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT %1 FROM notes n WHERE %2 ORDER BY %3 %4, n.id")
                      .arg(SummaryColumns, where.first, order,
                           sort.descending ? QStringLiteral("DESC") : QStringLiteral("ASC")));
    for (const QVariant &value : where.second) {
        query.addBindValue(value);
    }
    if (auto ok = run(query); !ok) {
        return std::unexpected(ok.error());
    }
    QList<NoteSummary> list;
    while (query.next()) {
        list.append(summaryFrom(query));
    }
    if (sort.key == NoteSort::Key::Title) {
        // SQLite folds only ASCII case, so titles are ordered here by the locale's rules.
        QCollator collator;
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        collator.setNumericMode(true);
        std::ranges::stable_sort(list, [&](const NoteSummary &a, const NoteSummary &b) {
            const int compared = collator.compare(a.title, b.title);
            return sort.descending ? compared > 0 : compared < 0;
        });
    }
    return list;
}

} // namespace

Index::Index(QString connection)
    : m_connection(std::move(connection))
{}

Index::Index(Index &&other) noexcept
    : m_connection(std::exchange(other.m_connection, QString()))
{}

Index &Index::operator=(Index &&other) noexcept
{
    if (this != &other) {
        const Index old(std::move(*this));
        m_connection = std::exchange(other.m_connection, QString());
    }
    return *this;
}

Index::~Index()
{
    if (m_connection.isEmpty()) {
        return;
    }
    {
        QSqlDatabase db = QSqlDatabase::database(m_connection, false);
        db.close();
    }
    QSqlDatabase::removeDatabase(m_connection);
}

QString Index::defaultPath(const QString &libraryId)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
           QStringLiteral("/jotter/%1/index.sqlite").arg(libraryId);
}

Result<Index> Index::open(const QString &path)
{
    static std::atomic<int> counter = 0;
    Index index(QStringLiteral("jotter-index-%1").arg(counter++));
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return fail(ErrorCode::Io, path, QStringLiteral("cannot create the cache folder"));
    }
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), index.m_connection);
    db.setDatabaseName(path);
    if (!db.open()) {
        return fail(ErrorCode::Io, path, db.lastError().text());
    }
    QSqlQuery query(db);
    for (const char *pragma : {"PRAGMA journal_mode = WAL", "PRAGMA foreign_keys = ON",
                               "PRAGMA busy_timeout = 5000", "PRAGMA synchronous = NORMAL"}) {
        if (auto ok = run(query, QString::fromLatin1(pragma)); !ok) {
            return std::unexpected(ok.error());
        }
    }
    if (auto ok = registerTokenizer(db); !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = run(query, QStringLiteral("PRAGMA user_version")); !ok) {
        return std::unexpected(ok.error());
    }
    const int version = query.next() ? query.value(0).toInt() : 0;
    const QString stems = Stemmer().fingerprint();
    bool current = version == SchemaVersion;
    if (current) {
        // Words stemmed differently than when they were indexed could not be removed again.
        current = query.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'stems'")) &&
                  query.next() && query.value(0).toString() == stems;
    }
    if (!current) {
        // The index is derived data: an outdated one is dropped and filled again by a scan.
        if (auto begun = begin(db); !begun) {
            return std::unexpected(begun.error());
        }
        for (const char *table : DroppedTables) {
            if (auto ok = run(
                    query, QStringLiteral("DROP TABLE IF EXISTS %1").arg(QLatin1StringView(table)));
                !ok) {
                db.rollback();
                return std::unexpected(ok.error());
            }
        }
        for (const char *statement : Schema) {
            if (auto ok = run(query, QString::fromLatin1(statement)); !ok) {
                db.rollback();
                return std::unexpected(ok.error());
            }
        }
        query.prepare(QStringLiteral("INSERT INTO meta VALUES ('stems', ?)"));
        query.addBindValue(stems);
        if (auto ok = run(query); !ok) {
            db.rollback();
            return std::unexpected(ok.error());
        }
        if (auto ok = run(query, QStringLiteral("PRAGMA user_version = %1").arg(SchemaVersion));
            !ok) {
            db.rollback();
            return std::unexpected(ok.error());
        }
        if (!db.commit()) {
            return fail(ErrorCode::Io, db.databaseName(), db.lastError().text());
        }
    }
    return index;
}

Result<ScanReport> Index::scan(const Library &library, bool rebuild)
{
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);

    // Validate enumeration before touching the previous derived state, including rebuilds.
    auto files = listDirectory(library.notesDir());
    if (!files) {
        return std::unexpected(files.error());
    }
    QHash<QString, std::pair<QString, FileStamp>> knownNotes; // file -> (id, stamp)
    if (auto ok = run(query, QStringLiteral("SELECT file, id, size, mtime FROM notes")); !ok) {
        return std::unexpected(ok.error());
    }
    while (query.next()) {
        knownNotes.insert(query.value(0).toString(), {query.value(1).toString(),
                                                      {.size = query.value(2).toLongLong(),
                                                       .mtime = query.value(3).toLongLong()}});
    }
    QHash<QString, FileStamp> knownProblems;
    if (auto ok = run(query, QStringLiteral("SELECT file, size, mtime FROM problems")); !ok) {
        return std::unexpected(ok.error());
    }
    while (query.next()) {
        knownProblems.insert(query.value(0).toString(), {.size = query.value(1).toLongLong(),
                                                         .mtime = query.value(2).toLongLong()});
    }

    ScanReport report;
    QSet<QString> present;
    if (auto begun = begin(db); !begun) {
        return std::unexpected(begun.error());
    }
    const auto abort = [&db](const Error &error) {
        db.rollback();
        return std::unexpected(error);
    };
    if (rebuild) {
        if (auto cleared = run(query, QStringLiteral("DELETE FROM notes")); !cleared) {
            return abort(cleared.error());
        }
        if (auto cleared = run(query, QStringLiteral("DELETE FROM problems")); !cleared) {
            return abort(cleared.error());
        }
        knownNotes.clear();
        knownProblems.clear();
    }
    for (const QString &file : *files) {
        if (!file.endsWith(QLatin1StringView(".html"))) {
            continue;
        }
        const FileGuard guard(library.notesDir() + u'/' + file);
        const QFileInfo info(library.notesDir() + u'/' + file);
        if (!isValidId(info.completeBaseName())) {
            continue; // Files not named <uuid>.html are not notes.
        }
        present.insert(file);
        const FileStamp stamp = stampOf(info);
        const auto known = knownNotes.constFind(file);
        if ((known != knownNotes.constEnd() && known->second == stamp) ||
            knownProblems.value(file, FileStamp{.size = -1, .mtime = -1}) == stamp) {
            continue;
        }
        // Committing in batches lets the interface write a saved note between them instead of
        // waiting for a long scan; every committed row matches its file, so a scan that stops
        // half way leaves a consistent index that the next scan completes.
        if (report.read > 0 && report.read % ScanBatch == 0) {
            if (!db.commit()) {
                return abort({.code = ErrorCode::Io,
                              .path = db.databaseName(),
                              .detail = db.lastError().text()});
            }
            if (auto begun = begin(db); !begun) {
                return std::unexpected(begun.error());
            }
        }
        ++report.read;
        auto note = library.readNote(info.completeBaseName());
        if (note) {
            if (auto ok = storeNote(db, *note, file, stamp); !ok) {
                return abort(ok.error());
            }
            query.prepare(QStringLiteral("DELETE FROM problems WHERE file = ?"));
            query.addBindValue(file);
            if (auto ok = run(query); !ok) {
                return abort(ok.error());
            }
            ++(known != knownNotes.constEnd() ? report.updated : report.added);
        } else {
            query.prepare(QStringLiteral("DELETE FROM notes WHERE file = ?"));
            query.addBindValue(file);
            if (auto ok = run(query); !ok) {
                return abort(ok.error());
            }
            query.prepare(QStringLiteral("INSERT OR REPLACE INTO problems VALUES (?, ?, ?, ?, ?)"));
            // An error without a detail, such as NewerFormat, has a null string, which Qt binds
            // as NULL; the detail column is NOT NULL.
            const QString detail =
                note.error().detail.isNull() ? QStringLiteral("") : note.error().detail;
            for (const QVariant &value :
                 {QVariant(file), QVariant(stamp.size), QVariant(stamp.mtime),
                  QVariant(static_cast<int>(note.error().code)), QVariant(detail)}) {
                query.addBindValue(value);
            }
            if (auto ok = run(query); !ok) {
                return abort(ok.error());
            }
            if (known != knownNotes.constEnd()) {
                ++report.removed;
            }
        }
    }
    for (auto it = knownNotes.constBegin(); it != knownNotes.constEnd(); ++it) {
        if (!present.contains(it.key())) {
            query.prepare(QStringLiteral("DELETE FROM notes WHERE file = ?"));
            query.addBindValue(it.key());
            if (auto ok = run(query); !ok) {
                return abort(ok.error());
            }
            ++report.removed;
        }
    }
    for (auto it = knownProblems.constBegin(); it != knownProblems.constEnd(); ++it) {
        if (!present.contains(it.key())) {
            query.prepare(QStringLiteral("DELETE FROM problems WHERE file = ?"));
            query.addBindValue(it.key());
            if (auto ok = run(query); !ok) {
                return abort(ok.error());
            }
        }
    }
    if (!db.commit()) {
        return fail(ErrorCode::Io, db.databaseName(), db.lastError().text());
    }
    return report;
}

Result<void> Index::remove(const QString &id)
{
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("DELETE FROM notes WHERE id = ?"));
    query.addBindValue(id);
    return run(query);
}

Result<QStringList> Index::trashedBefore(const QDateTime &time) const
{
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("SELECT id FROM notes WHERE trashed IS NOT NULL AND trashed < ?"));
    query.addBindValue(time.toMSecsSinceEpoch());
    if (auto ok = run(query); !ok) {
        return std::unexpected(ok.error());
    }
    QStringList ids;
    while (query.next()) {
        ids.append(query.value(0).toString());
    }
    return ids;
}

Result<void> Index::clear()
{
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    if (auto begun = begin(db); !begun) {
        return begun;
    }
    for (const char *table : Tables) {
        if (auto ok = run(query, QStringLiteral("DELETE FROM %1").arg(QLatin1StringView(table)));
            !ok) {
            db.rollback();
            return ok;
        }
    }
    if (!db.commit()) {
        return fail(ErrorCode::Io, db.databaseName(), db.lastError().text());
    }
    return {};
}

Result<void> Index::put(const Note &note, const QString &path)
{
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    const QFileInfo info(path);
    if (auto begun = begin(db); !begun) {
        return begun;
    }
    if (auto ok = storeNote(db, note, info.fileName(), stampOf(info)); !ok) {
        db.rollback();
        return ok;
    }
    if (!db.commit()) {
        return fail(ErrorCode::Io, db.databaseName(), db.lastError().text());
    }
    return {};
}

Result<QList<NoteSummary>> Index::notes(const NoteFilter &filter, const NoteSort &sort) const
{
    return listNotes(QSqlDatabase::database(m_connection), whereClause(filter), sort);
}

Result<QList<NoteSummary>> Index::search(const NoteFilter &filter, const Search &search,
                                         const QList<Collection> &collections,
                                         const NoteSort &sort) const
{
    NoteFilter view = filter;
    view.includeArchived = search.includeArchived;
    auto [where, values] = whereClause(view);
    const auto require = [&where, &values](const QString &condition,
                                           const QVariantList &bound = {}) {
        where += QStringLiteral(" AND ") + condition;
        values += bound;
    };
    for (const auto &[time, condition] : {std::pair{&search.createdFrom, "n.created >= ?"},
                                          std::pair{&search.createdUntil, "n.created < ?"},
                                          std::pair{&search.updatedFrom, "n.updated >= ?"},
                                          std::pair{&search.updatedUntil, "n.updated < ?"}}) {
        if (*time) {
            require(QString::fromLatin1(condition), {(*time)->toMSecsSinceEpoch()});
        }
    }
    if (!search.collections.isEmpty()) {
        QStringList ids;
        for (const QString &id : search.collections) {
            ids.append(collectionSubtree(collections, id));
        }
        ids.removeDuplicates();
        const auto [condition, bound] = collectionCondition(ids);
        require(condition, bound);
    }
    if (!search.tags.isEmpty()) {
        const auto [condition, bound] = tagCondition(search.tags, false);
        require(condition, bound);
    }
    if (search.withAttachments) {
        require(QStringLiteral("n.attachments > 0"));
    }
    if (search.withLocation) {
        require(QStringLiteral("n.has_location = 1"));
    }

    // Every term must match somewhere in the chosen scopes: in the full-text columns or in the
    // name of one of the note's collections.
    const QString columns = ftsColumns(search.scopes);
    QStringList ranked;
    for (const SearchTerm &term : parseQuery(search.query)) {
        Condition match;
        if (term.kind == SearchTerm::Kind::Tag) {
            match = tagCondition({term.text}, term.prefix);
        } else {
            QStringList parts;
            if (!columns.isEmpty()) {
                const QString expression = columns + ftsString(term);
                parts.append(QStringLiteral(
                    "n.seq IN (SELECT rowid FROM notes_fts WHERE notes_fts MATCH ?)"));
                match.second.append(expression);
                if (!term.exclude) {
                    ranked.append(expression);
                }
            }
            if (search.scopes.testFlag(SearchScope::Collections)) {
                if (const QStringList ids = collectionsMatching(term, collections);
                    !ids.isEmpty()) {
                    const auto [condition, bound] = collectionCondition(ids);
                    parts.append(condition);
                    match.second += bound;
                }
            }
            match.first = parts.isEmpty()
                              ? QStringLiteral("0")
                              : QStringLiteral("(%1)").arg(parts.join(QStringLiteral(" OR ")));
        }
        require(term.exclude ? QStringLiteral("NOT ") + match.first : match.first, match.second);
    }
    const QSqlDatabase db = QSqlDatabase::database(m_connection);
    if (ranked.isEmpty()) {
        // Filters alone have no relevance: the notes keep the list's order.
        return listNotes(db, {where, values}, sort);
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT %1, r.fragment FROM notes n LEFT JOIN (SELECT rowid, %2"
                                 " AS rank, snippet(notes_fts, 1, char(2), char(3), '…', 24)"
                                 " AS fragment FROM notes_fts WHERE notes_fts MATCH ?) r"
                                 " ON r.rowid = n.seq WHERE %3"
                                 " ORDER BY r.rank IS NULL, r.rank, n.updated DESC, n.id")
                      .arg(SummaryColumns, Relevance, where));
    query.addBindValue(QStringLiteral("(%1)").arg(ranked.join(QStringLiteral(") OR ("))));
    for (const QVariant &value : values) {
        query.addBindValue(value);
    }
    if (auto ok = run(query); !ok) {
        return std::unexpected(ok.error());
    }
    QList<NoteSummary> list;
    while (query.next()) {
        NoteSummary note = summaryFrom(query);
        // A note found by a collection name or with matches only in its title keeps the start
        // of its text.
        QList<TextSpan> matches;
        const QString fragment = fragmentText(query.value(13).toString(), matches);
        if (!matches.isEmpty()) {
            note.snippet = fragment;
            note.matches = std::move(matches);
        }
        list.append(std::move(note));
    }
    return list;
}

Result<QList<NoteSummary>> Index::backlinks(const QString &id, bool anyState) const
{
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(
        QStringLiteral("SELECT %1 FROM notes n JOIN note_links l ON l.note_id = n.id"
                       " WHERE l.target_id = ? AND n.id <> ? %2 ORDER BY n.title, n.id")
            .arg(SummaryColumns, anyState ? QString() : QStringLiteral("AND n.trashed IS NULL")));
    query.addBindValue(id);
    query.addBindValue(id);
    if (auto ok = run(query); !ok) {
        return std::unexpected(ok.error());
    }
    QList<NoteSummary> list;
    while (query.next()) {
        list.append(summaryFrom(query));
    }
    return list;
}

Result<QHash<QString, NoteSummary>> Index::notesById(const QStringList &ids) const
{
    QHash<QString, NoteSummary> found;
    if (ids.isEmpty()) {
        return found;
    }
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("SELECT %1 FROM notes n WHERE n.id IN (%2)")
                      .arg(SummaryColumns, placeholders(ids.size())));
    for (const QString &id : ids) {
        query.addBindValue(id);
    }
    if (auto ok = run(query); !ok) {
        return std::unexpected(ok.error());
    }
    while (query.next()) {
        NoteSummary note = summaryFrom(query);
        found.insert(note.id, std::move(note));
    }
    return found;
}

Result<std::optional<NoteSummary>> Index::note(const QString &id) const
{
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("SELECT %1 FROM notes n WHERE n.id = ?").arg(SummaryColumns));
    query.addBindValue(id);
    if (auto ok = run(query); !ok) {
        return std::unexpected(ok.error());
    }
    if (!query.next()) {
        return std::optional<NoteSummary>();
    }
    return std::optional<NoteSummary>(summaryFrom(query));
}

Result<int> Index::count(const NoteFilter &filter) const
{
    const auto [where, values] = whereClause(filter);
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM notes n WHERE %1").arg(where));
    for (const QVariant &value : values) {
        query.addBindValue(value);
    }
    if (auto ok = run(query); !ok) {
        return std::unexpected(ok.error());
    }
    return query.next() ? query.value(0).toInt() : 0;
}

Result<QHash<QString, int>> Index::collectionCounts(const QList<Collection> &collections) const
{
    QSqlQuery query(QSqlDatabase::database(m_connection));
    if (auto ok = run(query, QStringLiteral("SELECT c.collection_id, n.seq FROM note_collections c"
                                            " JOIN notes n ON n.id = c.note_id"
                                            " WHERE n.archived IS NULL AND n.trashed IS NULL"));
        !ok) {
        return std::unexpected(ok.error());
    }
    QHash<QString, QSet<qint64>> notes;
    while (query.next()) {
        notes[query.value(0).toString()].insert(query.value(1).toLongLong());
    }
    QHash<QString, int> counts;
    // The tree lists each parent before its children, so walking it backwards finishes every
    // child before its notes join its parent's.
    for (const CollectionLevel &level : collectionTree(collections) | std::views::reverse) {
        const Collection &collection = *level.collection;
        const QSet<qint64> own = notes.value(collection.id);
        counts.insert(collection.id, static_cast<int>(own.size()));
        if (collection.parent) {
            notes[*collection.parent].unite(own);
        }
    }
    // A collection outside the tree (a missing parent or a cycle in a damaged file) is counted
    // on its own.
    for (const Collection &collection : collections) {
        if (!counts.contains(collection.id)) {
            QSet<qint64> all;
            for (const QString &id : collectionSubtree(collections, collection.id)) {
                all.unite(notes.value(id));
            }
            counts.insert(collection.id, static_cast<int>(all.size()));
        }
    }
    return counts;
}

Result<QList<TagCount>> Index::tags() const
{
    QSqlQuery query(QSqlDatabase::database(m_connection));
    // Oldest notes first, so each tag keeps the spelling it was first used with.
    if (auto ok = run(query, QStringLiteral("SELECT t.tag_key, t.tag FROM note_tags t"
                                            " JOIN notes n ON n.id = t.note_id"
                                            " WHERE n.archived IS NULL AND n.trashed IS NULL"
                                            " ORDER BY n.created, n.id"));
        !ok) {
        return std::unexpected(ok.error());
    }
    QHash<QString, qsizetype> positions;
    QList<TagCount> tags;
    while (query.next()) {
        const QString key = query.value(0).toString();
        const auto found = positions.constFind(key);
        if (found == positions.constEnd()) {
            positions.insert(key, tags.size());
            tags.append({.name = query.value(1).toString(), .count = 1});
        } else {
            ++tags[*found].count;
        }
    }
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::ranges::sort(tags, [&](const TagCount &a, const TagCount &b) {
        return collator.compare(a.name, b.name) < 0;
    });
    return tags;
}

Result<QList<Problem>> Index::problems() const
{
    QSqlQuery query(QSqlDatabase::database(m_connection));
    if (auto ok =
            run(query, QStringLiteral("SELECT file, code, detail FROM problems ORDER BY file"));
        !ok) {
        return std::unexpected(ok.error());
    }
    QList<Problem> problems;
    while (query.next()) {
        problems.append({.path = query.value(0).toString(),
                         .error = {.code = static_cast<ErrorCode>(query.value(1).toInt()),
                                   .path = query.value(0).toString(),
                                   .detail = query.value(2).toString()}});
    }
    return problems;
}

Result<int> Index::removeOrphans(const Library &library, const QDateTime &now)
{
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    QSqlQuery query(db);
    QSet<std::pair<QString, QString>> referenced;
    if (auto ok = run(query, QStringLiteral("SELECT folder, file FROM note_files")); !ok) {
        return std::unexpected(ok.error());
    }
    while (query.next()) {
        referenced.insert({query.value(0).toString(), query.value(1).toString()});
    }
    QHash<std::pair<QString, QString>, qint64> orphans;
    if (auto ok = run(query, QStringLiteral("SELECT folder, file, since FROM orphans")); !ok) {
        return std::unexpected(ok.error());
    }
    while (query.next()) {
        orphans.insert({query.value(0).toString(), query.value(1).toString()},
                       query.value(2).toLongLong());
    }
    QSet<QString> unreadable;
    if (auto ok = run(query, QStringLiteral("SELECT file FROM problems")); !ok) {
        return std::unexpected(ok.error());
    }
    while (query.next()) {
        unreadable.insert(QFileInfo(query.value(0).toString()).completeBaseName());
    }

    const qint64 expired = now.addDays(-OrphanGraceDays).toMSecsSinceEpoch();
    int removed = 0;
    QSet<std::pair<QString, QString>> present;
    if (auto begun = begin(db); !begun) {
        return std::unexpected(begun.error());
    }
    const QDir root(library.attachmentsDir(QString()));
    auto folders = listDirectory(root.path());
    if (!folders) {
        db.rollback();
        return std::unexpected(folders.error());
    }
    for (const QString &folder : *folders) {
        if (!isValidId(folder) || unreadable.contains(folder)) {
            continue;
        }
        const QDir dir(root.filePath(folder));
        auto files = listDirectory(dir.path());
        if (!files) {
            db.rollback();
            return std::unexpected(files.error());
        }
        for (const QString &file : *files) {
            const std::pair<QString, QString> key{folder, file};
            if (referenced.contains(key)) {
                continue;
            }
            const auto since = orphans.constFind(key);
            if (since == orphans.constEnd()) {
                query.prepare(QStringLiteral("INSERT INTO orphans VALUES (?, ?, ?)"));
                query.addBindValue(folder);
                query.addBindValue(file);
                query.addBindValue(now.toMSecsSinceEpoch());
                if (auto ok = run(query); !ok) {
                    db.rollback();
                    return std::unexpected(ok.error());
                }
                present.insert(key);
            } else if (*since <= expired && removePath(dir.filePath(file))) {
                ++removed;
            } else {
                present.insert(key);
            }
        }
        const auto remaining = listDirectory(dir.path());
        if (remaining && remaining->isEmpty() && !QFileInfo::exists(library.notePath(folder))) {
            const auto cleaned = removePath(root.filePath(folder));
            Q_UNUSED(cleaned)
        }
    }
    // Files referenced again, removed or gone are forgotten.
    for (auto it = orphans.constBegin(); it != orphans.constEnd(); ++it) {
        if (!present.contains(it.key())) {
            query.prepare(QStringLiteral("DELETE FROM orphans WHERE folder = ? AND file = ?"));
            query.addBindValue(it.key().first);
            query.addBindValue(it.key().second);
            if (auto ok = run(query); !ok) {
                db.rollback();
                return std::unexpected(ok.error());
            }
        }
    }
    if (!db.commit()) {
        return fail(ErrorCode::Io, db.databaseName(), db.lastError().text());
    }
    return removed;
}

} // namespace core
