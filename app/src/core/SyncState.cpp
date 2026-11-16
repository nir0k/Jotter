#include "core/SyncState.h"

#include "core/Files.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

#include <atomic>
#include <initializer_list>

namespace core {

namespace {

/// The schema version; an older state is dropped, which only makes the next run a first
/// connection.
constexpr int SchemaVersion = 1;

/// Prepares and runs a statement with its values bound in order.
///
/// @param db The connection.
/// @param sql The statement.
/// @param values The values of its placeholders.
/// @return The query positioned before its first row, or Io.
Result<QSqlQuery> exec(const QSqlDatabase &db, const QString &sql,
                       std::initializer_list<QVariant> values = {})
{
    QSqlQuery query(db);
    if (!query.prepare(sql)) {
        return fail(ErrorCode::Io, sql, query.lastError().text());
    }
    for (const QVariant &value : values) {
        query.addBindValue(value);
    }
    if (!query.exec()) {
        return fail(ErrorCode::Io, sql, query.lastError().text());
    }
    return query;
}

/// Turns the result of exec() into an empty result.
Result<void> done(const Result<QSqlQuery> &query)
{
    if (!query) {
        return std::unexpected(query.error());
    }
    return {};
}

} // namespace

SyncState::SyncState(QString connection)
    : m_connection(std::move(connection))
{}

SyncState::SyncState(SyncState &&other) noexcept
    : m_connection(std::exchange(other.m_connection, QString()))
{}

SyncState &SyncState::operator=(SyncState &&other) noexcept
{
    if (this != &other) {
        const SyncState old(std::move(*this));
        m_connection = std::exchange(other.m_connection, QString());
    }
    return *this;
}

SyncState::~SyncState()
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

QString SyncState::defaultPath(const QString &libraryId)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QStringLiteral("/jotter/sync/%1/state.sqlite").arg(libraryId);
}

Result<SyncState> SyncState::open(const QString &path)
{
    static std::atomic<int> counter = 0;
    SyncState state(QStringLiteral("jotter-sync-%1").arg(counter++));
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return fail(ErrorCode::Io, path, QStringLiteral("cannot create the sync state folder"));
    }
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), state.m_connection);
    db.setDatabaseName(path);
    if (!db.open()) {
        return fail(ErrorCode::Io, path, db.lastError().text());
    }
    for (const char *pragma : {"PRAGMA journal_mode = WAL", "PRAGMA busy_timeout = 5000",
                               "PRAGMA synchronous = NORMAL"}) {
        if (auto ok = exec(db, QString::fromLatin1(pragma)); !ok) {
            return std::unexpected(ok.error());
        }
    }
    auto version = exec(db, QStringLiteral("PRAGMA user_version"));
    if (!version) {
        return std::unexpected(version.error());
    }
    if (const int current = version->next() ? version->value(0).toInt() : 0;
        current != SchemaVersion) {
        constexpr const char *Files =
            "CREATE TABLE files (path TEXT PRIMARY KEY, hash BLOB NOT NULL, "
            "remote_size INTEGER NOT NULL, remote_modified INTEGER NOT NULL, "
            "local_size INTEGER NOT NULL, local_modified INTEGER NOT NULL, base BLOB)";
        for (const char *sql :
             {"DROP TABLE IF EXISTS settings", "DROP TABLE IF EXISTS files",
              "DROP TABLE IF EXISTS folders",
              "CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)", Files,
              "CREATE TABLE folders (path TEXT PRIMARY KEY, modified INTEGER NOT NULL)"}) {
            if (auto ok = exec(db, QString::fromLatin1(sql)); !ok) {
                return std::unexpected(ok.error());
            }
        }
        if (auto ok = exec(db, QStringLiteral("PRAGMA user_version = %1").arg(SchemaVersion));
            !ok) {
            return std::unexpected(ok.error());
        }
    }
    return state;
}

Result<QString> SyncState::deviceId()
{
    const QSqlDatabase db = QSqlDatabase::database(m_connection);
    auto query = exec(db, QStringLiteral("SELECT value FROM settings WHERE key = 'device'"));
    if (!query) {
        return std::unexpected(query.error());
    }
    if (query->next()) {
        return query->value(0).toString();
    }
    QString id = newId();
    if (auto ok = exec(db, QStringLiteral("INSERT INTO settings (key, value) VALUES ('device', ?)"),
                       {id});
        !ok) {
        return std::unexpected(ok.error());
    }
    return id;
}

Result<QHash<QString, SyncedFile>> SyncState::files() const
{
    auto query = exec(QSqlDatabase::database(m_connection),
                      QStringLiteral("SELECT path, hash, remote_size, remote_modified, local_size, "
                                     "local_modified FROM files"));
    if (!query) {
        return std::unexpected(query.error());
    }
    QHash<QString, SyncedFile> files;
    while (query->next()) {
        files.insert(query->value(0).toString(), {.hash = query->value(1).toByteArray(),
                                                  .remoteSize = query->value(2).toLongLong(),
                                                  .remoteModified = query->value(3).toLongLong(),
                                                  .localSize = query->value(4).toLongLong(),
                                                  .localModified = query->value(5).toLongLong()});
    }
    return files;
}

Result<std::optional<QByteArray>> SyncState::base(const QString &path) const
{
    auto query = exec(QSqlDatabase::database(m_connection),
                      QStringLiteral("SELECT base FROM files WHERE path = ?"), {path});
    if (!query) {
        return std::unexpected(query.error());
    }
    if (!query->next() || query->value(0).isNull()) {
        return std::optional<QByteArray>();
    }
    return std::optional(query->value(0).toByteArray());
}

Result<void> SyncState::putFile(const QString &path, const SyncedFile &file,
                                const std::optional<QByteArray> &base)
{
    return done(exec(QSqlDatabase::database(m_connection),
                     QStringLiteral("INSERT OR REPLACE INTO files (path, hash, remote_size, "
                                    "remote_modified, local_size, local_modified, base) "
                                    "VALUES (?, ?, ?, ?, ?, ?, ?)"),
                     {path, file.hash, file.remoteSize, file.remoteModified, file.localSize,
                      file.localModified,
                      base ? QVariant(*base) : QVariant(QMetaType(QMetaType::QByteArray))}));
}

Result<void> SyncState::touchFile(const QString &path, const SyncedFile &file)
{
    return done(exec(QSqlDatabase::database(m_connection),
                     QStringLiteral("UPDATE files SET hash = ?, remote_size = ?, "
                                    "remote_modified = ?, local_size = ?, local_modified = ? "
                                    "WHERE path = ?"),
                     {file.hash, file.remoteSize, file.remoteModified, file.localSize,
                      file.localModified, path}));
}

Result<void> SyncState::removeFile(const QString &path)
{
    return done(exec(QSqlDatabase::database(m_connection),
                     QStringLiteral("DELETE FROM files WHERE path = ?"), {path}));
}

Result<QHash<QString, qint64>> SyncState::folders() const
{
    auto query = exec(QSqlDatabase::database(m_connection),
                      QStringLiteral("SELECT path, modified FROM folders"));
    if (!query) {
        return std::unexpected(query.error());
    }
    QHash<QString, qint64> folders;
    while (query->next()) {
        folders.insert(query->value(0).toString(), query->value(1).toLongLong());
    }
    return folders;
}

Result<void> SyncState::putFolder(const QString &path, qint64 modified)
{
    return done(
        exec(QSqlDatabase::database(m_connection),
             QStringLiteral("INSERT OR REPLACE INTO folders (path, modified) VALUES (?, ?)"),
             {path, modified}));
}

Result<void> SyncState::removeFolder(const QString &path)
{
    return done(exec(QSqlDatabase::database(m_connection),
                     QStringLiteral("DELETE FROM folders WHERE path = ?"), {path}));
}

Result<void> SyncState::clear()
{
    const QSqlDatabase db = QSqlDatabase::database(m_connection);
    if (auto ok = exec(db, QStringLiteral("DELETE FROM files")); !ok) {
        return std::unexpected(ok.error());
    }
    return done(exec(db, QStringLiteral("DELETE FROM folders")));
}

Result<void> SyncState::begin()
{
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    if (!db.transaction()) {
        return fail(ErrorCode::Io, db.databaseName(), db.lastError().text());
    }
    return {};
}

Result<void> SyncState::commit()
{
    QSqlDatabase db = QSqlDatabase::database(m_connection);
    if (!db.commit()) {
        return fail(ErrorCode::Io, db.databaseName(), db.lastError().text());
    }
    return {};
}

} // namespace core
