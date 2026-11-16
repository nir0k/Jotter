#include "core/Files.h"

#include "core/Directory.h"
#include "core/FileGuard.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

#include <sys/stat.h>

namespace core {

QString newId()
{
    return QUuid::createUuidV7().toString(QUuid::WithoutBraces);
}

bool isValidId(const QString &id)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    return pattern.match(id).hasMatch();
}

QString formatTime(const QDateTime &time)
{
    return time.toUTC().toString(Qt::ISODateWithMs);
}

std::optional<QDateTime> parseTime(const QString &text)
{
    const QDateTime time = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!time.isValid()) {
        return std::nullopt;
    }
    return time.toUTC();
}

QString displayTime(const QDateTime &time)
{
    const QLocale locale;
    const QDateTime local = time.toLocalTime();
    return QStringLiteral("%1, %2").arg(locale.toString(local.date(), QLocale::LongFormat),
                                        locale.toString(local.time(), QLocale::ShortFormat));
}

QDateTime now()
{
    const QDateTime time = QDateTime::currentDateTimeUtc();
    return QDateTime::fromMSecsSinceEpoch(time.toMSecsSinceEpoch(), QTimeZone::UTC);
}

QString fileRevision(const QString &path)
{
    struct stat status{};
    if (::lstat(QFile::encodeName(path).constData(), &status) != 0) {
        return {};
    }
    return QStringLiteral("%1:%2:%3:%4:%5:%6:%7")
        .arg(status.st_dev)
        .arg(status.st_ino)
        .arg(status.st_size)
        .arg(status.st_mtim.tv_sec)
        .arg(status.st_mtim.tv_nsec)
        .arg(status.st_ctim.tv_sec)
        .arg(status.st_ctim.tv_nsec);
}

QString resolvedPath(const QString &path)
{
    QString existing = QDir(path).absolutePath();
    QStringList missing;
    while (!QFileInfo::exists(existing) && existing != u'/') {
        const qsizetype slash = existing.lastIndexOf(u'/');
        missing.prepend(existing.sliced(slash + 1));
        existing = slash == 0 ? QStringLiteral("/") : existing.first(slash);
    }
    QString resolved = QFileInfo(existing).canonicalFilePath();
    if (resolved.isEmpty()) {
        resolved = existing;
    }
    for (const QString &part : std::as_const(missing)) {
        resolved += (resolved.endsWith(u'/') ? QString() : QStringLiteral("/")) + part;
    }
    return resolved;
}

Result<QByteArray> readFile(const QString &path)
{
    const PathParts file = splitPath(path);
    auto directory = Directory::open(file.folder);
    return directory ? directory->read(file.name) : std::unexpected(directory.error());
}

Result<void> writeFileAtomic(const QString &path, const QByteArray &data,
                             std::optional<QFileDevice::Permissions> permissions)
{
    const FileGuard guard(path);
    const PathParts file = splitPath(path);
    auto directory = Directory::open(file.folder, true);
    return directory ? directory->write(file.name, data, permissions)
                     : std::unexpected(directory.error());
}

Result<void> copyFileAtomic(const QString &source, const QString &target)
{
    auto input = openFile(source, QIODevice::ReadOnly);
    if (!input) {
        return std::unexpected(input.error());
    }
    QSaveFile output(target);
    if (!output.open(QIODevice::WriteOnly)) {
        return fail(ErrorCode::Io, target, output.errorString());
    }
    while (!(*input)->atEnd()) {
        const QByteArray chunk = (*input)->read(qint64{1024} * 1024);
        if ((*input)->error() != QFileDevice::NoError) {
            return fail(ErrorCode::Io, source, (*input)->errorString());
        }
        if (output.write(chunk) != chunk.size()) {
            return fail(ErrorCode::Io, target, output.errorString());
        }
    }
    if (!output.commit()) {
        return fail(ErrorCode::Io, target, output.errorString());
    }
    return {};
}

Result<void> writePrivateFile(const QString &path, const QByteArray &data)
{
    const QString folder = splitPath(path).folder;
    if (!QDir().mkpath(folder) ||
        !QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                           QFileDevice::ExeOwner)) {
        return fail(ErrorCode::Io, folder, QStringLiteral("cannot protect the folder"));
    }
    return writeFileAtomic(path, data, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

QJsonObject unknownFields(const QJsonObject &object, std::initializer_list<QLatin1StringView> known)
{
    QJsonObject extra;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (std::ranges::find(known, it.key()) == known.end()) {
            extra.insert(it.key(), it.value());
        }
    }
    return extra;
}

QJsonValue optionalString(const std::optional<QString> &value)
{
    return value ? QJsonValue(*value) : QJsonValue(QJsonValue::Null);
}

QByteArray toJsonText(const QJsonObject &object)
{
    // QJsonDocument indents with four spaces; library files use two, so halve the indentation.
    const QByteArray indented = QJsonDocument(object).toJson(QJsonDocument::Indented);
    QByteArray result;
    result.reserve(indented.size());
    qsizetype lineStart = 0;
    while (lineStart < indented.size()) {
        qsizetype lineEnd = indented.indexOf('\n', lineStart);
        if (lineEnd < 0) {
            lineEnd = indented.size();
        }
        qsizetype spaces = 0;
        while (lineStart + spaces < lineEnd && indented.at(lineStart + spaces) == ' ') {
            ++spaces;
        }
        result.append(spaces / 2, ' ');
        result.append(indented.mid(lineStart + spaces, lineEnd - lineStart - spaces));
        result.append('\n');
        lineStart = lineEnd + 1;
    }
    return result;
}

Result<QJsonObject> parseJsonObject(const QByteArray &data, const QString &path)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError) {
        return fail(ErrorCode::Malformed, path, error.errorString());
    }
    if (!document.isObject()) {
        return fail(ErrorCode::Malformed, path, QStringLiteral("the top level is not an object"));
    }
    return document.object();
}

} // namespace core
