#include "core/Credentials.h"

#include "core/Files.h"

#include <QJsonObject>
#include <QStandardPaths>

namespace core {

namespace {

/// Reads the credentials file; a missing file holds no credentials.
Result<QJsonObject> readStore()
{
    const QString path = credentialsPath();
    auto data = readFile(path);
    if (!data) {
        if (data.error().code == ErrorCode::NotFound) {
            return QJsonObject();
        }
        return std::unexpected(data.error());
    }
    return parseJsonObject(*data, path);
}

} // namespace

QString credentialsPath()
{
    return resolvedPath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)) +
           QStringLiteral("/jotter/credentials.json");
}

Result<Credentials> readCredentials(const QString &libraryId)
{
    auto store = readStore();
    if (!store) {
        return std::unexpected(store.error());
    }
    const QJsonObject entry =
        store->value(QStringLiteral("libraries")).toObject().value(libraryId).toObject();
    return Credentials{.secret = entry.value(QStringLiteral("secret")).toString(),
                       .key = entry.value(QStringLiteral("key")).toString()};
}

Result<void> writeCredentials(const QString &libraryId, const Credentials &credentials)
{
    auto store = readStore();
    if (!store) {
        return std::unexpected(store.error());
    }
    QJsonObject libraries = store->value(QStringLiteral("libraries")).toObject();
    if (credentials == Credentials()) {
        if (!libraries.contains(libraryId)) {
            return {};
        }
        libraries.remove(libraryId);
    } else {
        QJsonObject entry;
        if (!credentials.secret.isEmpty()) {
            entry.insert(QStringLiteral("secret"), credentials.secret);
        }
        if (!credentials.key.isEmpty()) {
            entry.insert(QStringLiteral("key"), credentials.key);
        }
        libraries.insert(libraryId, entry);
    }
    store->insert(QStringLiteral("libraries"), libraries);
    return writePrivateFile(credentialsPath(), toJsonText(*store));
}

} // namespace core
