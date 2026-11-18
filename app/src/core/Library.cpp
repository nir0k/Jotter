#include "core/Library.h"

#include "core/Attachments.h"
#include "core/Directory.h"
#include "core/FileGuard.h"
#include "core/Files.h"
#include "core/Html.h"
#include "core/Merge.h"
#include "core/Tags.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QMultiHash>
#include <QSet>

#include <functional>

namespace core {

namespace {

constexpr QLatin1StringView InfoFile("library.json");
constexpr QLatin1StringView CollectionsFile("collections.json");
constexpr QLatin1StringView RulesFile("rules.json");
constexpr QLatin1StringView ContentsFile("index.html");
constexpr QLatin1StringView NotesDir("notes");
constexpr QLatin1StringView AttachmentsDir("attachments");

QJsonObject infoToJson(const LibraryInfo &info)
{
    QJsonArray favorites;
    for (const FavoriteRef &favorite : info.favorites) {
        favorites.append(QJsonObject{
            {QStringLiteral("kind"), favorite.kind == FavoriteRef::Kind::Collection
                                         ? QStringLiteral("collection")
                                         : QStringLiteral("note")},
            {QStringLiteral("id"), favorite.id},
        });
    }
    QJsonObject tagColors;
    for (auto it = info.tagColors.cbegin(); it != info.tagColors.cend(); ++it) {
        tagColors.insert(it.key(), it.value());
    }
    QJsonArray tombstones;
    for (const Tombstone &tombstone : info.tombstones) {
        tombstones.append(QJsonObject{
            {QStringLiteral("id"), tombstone.id},
            {QStringLiteral("deleted"), formatTime(tombstone.deleted)},
        });
    }
    QJsonObject settings = info.extraSettings;
    settings.insert(QStringLiteral("trashRetentionDays"), info.trashRetentionDays
                                                              ? QJsonValue(*info.trashRetentionDays)
                                                              : QJsonValue(QJsonValue::Null));
    QJsonObject object = info.extra;
    object.insert(QStringLiteral("format"), LibraryFormat);
    object.insert(QStringLiteral("id"), info.id);
    object.insert(QStringLiteral("created"), formatTime(info.created));
    object.insert(QStringLiteral("favorites"), favorites);
    object.insert(QStringLiteral("tagColors"), tagColors);
    object.insert(QStringLiteral("settings"), settings);
    object.insert(QStringLiteral("tombstones"), tombstones);
    return object;
}

/// Parses `library.json`. A newer format is accepted as far as its identifier, so that the
/// library can still be shown read-only.
Result<LibraryInfo> infoFromJson(const QJsonObject &object, const QString &path, bool newer)
{
    LibraryInfo info;
    info.id = object.value(QStringLiteral("id")).toString();
    const auto created = parseTime(object.value(QStringLiteral("created")).toString());
    if (!isValidId(info.id) || !created) {
        return fail(ErrorCode::Malformed, path, QStringLiteral("invalid id or created time"));
    }
    info.created = *created;
    if (newer) {
        return info;
    }
    for (const auto &[key, array] : {std::pair{QLatin1StringView("favorites"), true},
                                     {QLatin1StringView("tagColors"), false},
                                     {QLatin1StringView("settings"), false},
                                     {QLatin1StringView("tombstones"), true}}) {
        const QJsonValue value = object.value(key);
        if (!value.isUndefined() && (array ? !value.isArray() : !value.isObject())) {
            return fail(ErrorCode::Malformed, path, QStringLiteral("invalid field %1").arg(key));
        }
    }
    for (const QJsonValue value : object.value(QStringLiteral("favorites")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QString kind = entry.value(QStringLiteral("kind")).toString();
        const QString id = entry.value(QStringLiteral("id")).toString();
        if ((kind != QLatin1StringView("collection") && kind != QLatin1StringView("note")) ||
            !isValidId(id)) {
            return fail(ErrorCode::Malformed, path, QStringLiteral("invalid favourite"));
        }
        info.favorites.append({.kind = kind == QLatin1StringView("collection")
                                           ? FavoriteRef::Kind::Collection
                                           : FavoriteRef::Kind::Note,
                               .id = id});
    }
    const QJsonObject tagColors = object.value(QStringLiteral("tagColors")).toObject();
    for (auto it = tagColors.constBegin(); it != tagColors.constEnd(); ++it) {
        if (!it.value().isString() || normalizedTag(it.key()).isEmpty()) {
            return fail(ErrorCode::Malformed, path, QStringLiteral("invalid tag colour"));
        }
        info.tagColors.insert(tagKey(normalizedTag(it.key())), it.value().toString());
    }
    for (const QJsonValue value : object.value(QStringLiteral("tombstones")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QString id = entry.value(QStringLiteral("id")).toString();
        const auto deleted = parseTime(entry.value(QStringLiteral("deleted")).toString());
        if (!isValidId(id) || !deleted) {
            return fail(ErrorCode::Malformed, path, QStringLiteral("invalid tombstone"));
        }
        info.tombstones.append({.id = id, .deleted = *deleted});
    }
    const QJsonObject settings = object.value(QStringLiteral("settings")).toObject();
    const QJsonValue retention = settings.value(QStringLiteral("trashRetentionDays"));
    if (retention.isNull()) {
        info.trashRetentionDays = std::nullopt;
    } else if (retention.isDouble() && retention.toInt(-1) > 0) {
        info.trashRetentionDays = retention.toInt();
    } else if (!retention.isUndefined()) {
        return fail(ErrorCode::Malformed, path, QStringLiteral("invalid trashRetentionDays"));
    }
    info.extra = unknownFields(
        object, {QLatin1StringView("format"), QLatin1StringView("id"), QLatin1StringView("created"),
                 QLatin1StringView("favorites"), QLatin1StringView("tagColors"),
                 QLatin1StringView("settings"), QLatin1StringView("tombstones")});
    info.extraSettings = unknownFields(settings, {QLatin1StringView("trashRetentionDays")});
    return info;
}

/// Returns the text of an empty list file such as `rules.json`.
QByteArray emptyListFile(const QString &key)
{
    return toJsonText(QJsonObject{{QStringLiteral("format"), LibraryFormat}, {key, QJsonArray()}});
}

} // namespace

void dropExpiredTombstones(QList<Tombstone> &tombstones, const QDateTime &now)
{
    const QDateTime limit = now.addDays(-TombstoneDays);
    tombstones.removeIf([&](const Tombstone &tombstone) { return tombstone.deleted < limit; });
}

Result<LibraryInfo> parseLibraryInfo(const QByteArray &data, const QString &path)
{
    auto object = parseJsonObject(data, path);
    if (!object) {
        return std::unexpected(object.error());
    }
    const int format = object->value(QStringLiteral("format")).toInt(0);
    if (format < 1) {
        return fail(ErrorCode::Malformed, path, QStringLiteral("invalid format"));
    }
    if (format > LibraryFormat) {
        return fail(ErrorCode::NewerFormat, path);
    }
    return infoFromJson(*object, path, false);
}

QByteArray serializeLibraryInfo(const LibraryInfo &info)
{
    return toJsonText(infoToJson(info));
}

Result<QList<Collection>> parseCollections(const QByteArray &data, const QString &path)
{
    auto object = parseJsonObject(data, path);
    if (!object) {
        return std::unexpected(object.error());
    }
    if (object->value(QStringLiteral("format")).toInt(0) > LibraryFormat) {
        return fail(ErrorCode::NewerFormat, path);
    }
    if (object->value(QStringLiteral("format")).toInt(0) < 1 ||
        !object->value(QStringLiteral("collections")).isArray()) {
        return fail(ErrorCode::Malformed, path, QStringLiteral("invalid collections document"));
    }
    QList<Collection> collections;
    for (const QJsonValue value : object->value(QStringLiteral("collections")).toArray()) {
        const QJsonObject entry = value.toObject();
        Collection collection;
        collection.id = entry.value(QStringLiteral("id")).toString();
        const QJsonValue parent = entry.value(QStringLiteral("parent"));
        if (parent.isString()) {
            collection.parent = parent.toString();
        }
        collection.name = entry.value(QStringLiteral("name")).toString();
        collection.icon = entry.value(QStringLiteral("icon")).toString(QStringLiteral("Folder"));
        if (const QJsonValue color = entry.value(QStringLiteral("color")); color.isString()) {
            collection.color = color.toString();
        }
        const auto created = parseTime(entry.value(QStringLiteral("created")).toString());
        const auto updated = parseTime(entry.value(QStringLiteral("updated")).toString());
        if (!isValidId(collection.id) || (collection.parent && !isValidId(*collection.parent)) ||
            !entry.value(QStringLiteral("name")).isString() || !created || !updated) {
            return fail(ErrorCode::Malformed, path,
                        QStringLiteral("invalid collection %1").arg(collection.id));
        }
        collection.created = *created;
        collection.updated = *updated;
        collection.extra = unknownFields(
            entry, {QLatin1StringView("id"), QLatin1StringView("parent"), QLatin1StringView("name"),
                    QLatin1StringView("icon"), QLatin1StringView("color"),
                    QLatin1StringView("created"), QLatin1StringView("updated")});
        collections.append(std::move(collection));
    }
    return collections;
}

QByteArray serializeCollections(const QList<Collection> &collections)
{
    QJsonArray list;
    for (const Collection &collection : collections) {
        QJsonObject entry = collection.extra;
        entry.insert(QStringLiteral("id"), collection.id);
        entry.insert(QStringLiteral("parent"), optionalString(collection.parent));
        entry.insert(QStringLiteral("name"), collection.name);
        entry.insert(QStringLiteral("icon"), collection.icon);
        entry.insert(QStringLiteral("color"), optionalString(collection.color));
        entry.insert(QStringLiteral("created"), formatTime(collection.created));
        entry.insert(QStringLiteral("updated"), formatTime(collection.updated));
        list.append(entry);
    }
    return toJsonText(QJsonObject{{QStringLiteral("format"), LibraryFormat},
                                  {QStringLiteral("collections"), list}});
}

QStringList collectionSubtree(const QList<Collection> &collections, const QString &id)
{
    QMultiHash<QString, QString> children;
    for (const Collection &collection : collections) {
        children.insert(collection.parent.value_or(QString()), collection.id);
    }
    QStringList ids{id};
    QSet<QString> seen{id};
    for (qsizetype i = 0; i < ids.size(); ++i) {
        for (const QString &child : children.values(ids.at(i))) {
            if (!seen.contains(child)) {
                seen.insert(child);
                ids.append(child);
            }
        }
    }
    return ids;
}

QList<CollectionLevel> collectionTree(const QList<Collection> &collections)
{
    // Children in their order under each parent; the top level is under an empty identifier.
    QHash<QString, QList<const Collection *>> children;
    for (const Collection &collection : collections) {
        children[collection.parent.value_or(QString())].append(&collection);
    }
    QList<CollectionLevel> levels;
    QSet<QString> listed;
    const std::function<void(const QString &, int)> addLevel = [&](const QString &parent,
                                                                   int depth) {
        for (const Collection *collection : children.value(parent)) {
            if (listed.contains(collection->id)) {
                continue;
            }
            listed.insert(collection->id);
            levels.append({.collection = collection, .depth = depth});
            addLevel(collection->id, depth + 1);
        }
    };
    addLevel(QString(), 0);
    return levels;
}

Library::Library(QString root, LibraryInfo info, ReadOnlyReason readOnly)
    : m_root(std::move(root))
    , m_info(std::move(info))
    , m_readOnly(readOnly)
{}

Result<Library> Library::create(const QString &root)
{
    const QDir dir(root);
    if (dir.exists() && !dir.isEmpty(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)) {
        return fail(ErrorCode::NotEmpty, root);
    }
    LibraryInfo info;
    info.id = newId();
    info.created = now();

    const QString absolute = resolvedPath(root);
    for (const QLatin1StringView sub : {NotesDir, AttachmentsDir}) {
        if (auto made = makeDirectory(absolute + u'/' + sub); !made) {
            return std::unexpected(made.error());
        }
    }
    const std::initializer_list<std::pair<QLatin1StringView, QByteArray>> files{
        {InfoFile, serializeLibraryInfo(info)},
        {CollectionsFile, emptyListFile(QStringLiteral("collections"))},
        {RulesFile, emptyListFile(QStringLiteral("rules"))},
    };
    for (const auto &[name, data] : files) {
        if (auto written = writeFileAtomic(absolute + u'/' + name, data); !written) {
            return std::unexpected(written.error());
        }
    }
    return Library(absolute, std::move(info), ReadOnlyReason::None);
}

Result<Library> Library::open(const QString &root)
{
    const QString absolute = resolvedPath(root);
    const QString path = absolute + u'/' + InfoFile;
    auto data = readFile(path);
    if (!data) {
        return std::unexpected(data.error());
    }
    auto object = parseJsonObject(*data, path);
    if (!object) {
        return std::unexpected(object.error());
    }
    const int format = object->value(QStringLiteral("format")).toInt(0);
    if (format < 1) {
        return fail(ErrorCode::Malformed, path, QStringLiteral("invalid format"));
    }
    const bool newer = format > LibraryFormat;
    auto info = infoFromJson(*object, path, newer);
    if (!info) {
        return std::unexpected(info.error());
    }
    if (newer) {
        return Library(absolute, std::move(*info), ReadOnlyReason::NewerFormat);
    }
    // Writes replace files through their folders, so the folders decide whether the library
    // can be changed. A folder that cannot be written is not recreated either.
    if (!QFileInfo(absolute).isWritable()) {
        return Library(absolute, std::move(*info), ReadOnlyReason::NoPermission);
    }
    // Folders removed by hand are recreated; they hold no data of their own.
    for (const QLatin1StringView sub : {NotesDir, AttachmentsDir}) {
        const QString folder = absolute + u'/' + sub;
        if (auto made = makeDirectory(folder); !made) {
            return std::unexpected(made.error());
        }
        if (!QFileInfo(folder).isWritable()) {
            return Library(absolute, std::move(*info), ReadOnlyReason::NoPermission);
        }
    }
    return Library(absolute, std::move(*info), ReadOnlyReason::None);
}

bool Library::exists(const QString &root)
{
    return QFileInfo::exists(QDir(root).filePath(InfoFile));
}

QString Library::notesDir() const
{
    return m_root + u'/' + NotesDir;
}

QString Library::notePath(const QString &id) const
{
    return notesDir() + u'/' + id + QStringLiteral(".html");
}

QString Library::attachmentsDir(const QString &noteId) const
{
    return core::attachmentsDir(m_root, noteId);
}

Result<void> Library::checkWritable() const
{
    if (isReadOnly()) {
        return fail(ErrorCode::ReadOnly, m_root);
    }
    return {};
}

Result<void> Library::writeInfo(const LibraryInfo &info)
{
    if (auto writable = checkWritable(); !writable) {
        return writable;
    }
    const QString path = m_root + u'/' + InfoFile;
    const FileGuard guard(path);
    auto data = readFile(path);
    if (!data) {
        return std::unexpected(data.error());
    }
    auto current = parseLibraryInfo(*data, path);
    if (!current || current->id != m_info.id) {
        return current ? fail(ErrorCode::OtherLibrary, path) : std::unexpected(current.error());
    }
    const LibraryInfo merged = mergeLibraryInfo(m_info, info, *current, now());
    if (auto written = writeFileAtomic(path, serializeLibraryInfo(merged)); !written) {
        return written;
    }
    m_info = merged;
    return {};
}

Result<void> Library::changeInfo(const std::function<void(LibraryInfo &)> &change)
{
    const QString path = m_root + u'/' + InfoFile;
    const FileGuard guard(path);
    auto data = readFile(path);
    if (!data) {
        return std::unexpected(data.error());
    }
    auto current = parseLibraryInfo(*data, path);
    if (!current || current->id != m_info.id) {
        return current ? fail(ErrorCode::OtherLibrary, path) : std::unexpected(current.error());
    }
    m_info = *current;
    change(*current);
    return writeInfo(*current);
}

Result<Note> Library::readNote(const QString &id) const
{
    if (!isValidId(id)) {
        return fail(ErrorCode::Refused, id);
    }
    const QString path = notePath(id);
    auto data = readFile(path);
    if (!data) {
        return std::unexpected(data.error());
    }
    return parseNote(*data, path);
}

Result<void> Library::writeNote(const Note &note) const
{
    if (auto writable = checkWritable(); !writable) {
        return writable;
    }
    if (!isValidId(note.meta.id)) {
        return fail(ErrorCode::Refused, note.meta.id);
    }
    const QString problem = html::bodyProblem(note.body);
    if (!problem.isEmpty()) {
        return fail(ErrorCode::Malformed, notePath(note.meta.id), problem);
    }
    return writeFileAtomic(notePath(note.meta.id), serializeNote(note));
}

Result<QList<Rule>> Library::readRules() const
{
    const QString path = m_root + u'/' + RulesFile;
    auto data = readFile(path);
    if (!data) {
        if (data.error().code == ErrorCode::NotFound) {
            return QList<Rule>{};
        }
        return std::unexpected(data.error());
    }
    auto parsed = parseRules(*data, path);
    if (parsed) {
        m_ruleBase = *parsed;
    }
    return parsed;
}

Result<void> Library::writeRules(const QList<Rule> &rules) const
{
    if (auto writable = checkWritable(); !writable) {
        return writable;
    }
    const QString path = m_root + u'/' + RulesFile;
    const FileGuard guard(path);
    auto data = readFile(path);
    if (!data && data.error().code != ErrorCode::NotFound) {
        return std::unexpected(data.error());
    }
    auto current = data ? parseRules(*data, path) : Result<QList<Rule>>(QList<Rule>());
    if (!current) {
        return std::unexpected(current.error());
    }
    const auto merged = m_ruleBase ? mergeRules(*m_ruleBase, rules, *current) : rules;
    auto written = writeFileAtomic(path, serializeRules(merged));
    if (written) {
        m_ruleBase = merged;
    }
    return written;
}

Result<void> Library::writeTableOfContents(const QByteArray &contents) const
{
    if (auto writable = checkWritable(); !writable) {
        return writable;
    }
    const QString path = m_root + u'/' + ContentsFile;
    if (const auto current = readFile(path); current && *current == contents) {
        return {};
    }
    return writeFileAtomic(path, contents);
}

Result<QList<Collection>> Library::readCollections() const
{
    const QString path = m_root + u'/' + CollectionsFile;
    auto data = readFile(path);
    if (!data) {
        // A missing file is an empty tree: the library may predate the person's first collection.
        if (data.error().code == ErrorCode::NotFound) {
            return QList<Collection>{};
        }
        return std::unexpected(data.error());
    }
    auto parsed = parseCollections(*data, path);
    if (parsed) {
        m_collectionBase = *parsed;
    }
    return parsed;
}

Result<void> Library::writeCollections(const QList<Collection> &collections) const
{
    if (auto writable = checkWritable(); !writable) {
        return writable;
    }
    const QString path = m_root + u'/' + CollectionsFile;
    const FileGuard guard(path);
    auto data = readFile(path);
    if (!data && data.error().code != ErrorCode::NotFound) {
        return std::unexpected(data.error());
    }
    auto current =
        data ? parseCollections(*data, path) : Result<QList<Collection>>(QList<Collection>());
    if (!current) {
        return std::unexpected(current.error());
    }
    const auto merged =
        m_collectionBase ? mergeCollections(*m_collectionBase, collections, *current) : collections;
    auto written = writeFileAtomic(path, serializeCollections(merged));
    if (written) {
        m_collectionBase = merged;
    }
    return written;
}

} // namespace core
