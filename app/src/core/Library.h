#pragma once

#include "core/Error.h"
#include "core/Note.h"
#include "core/Rules.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>

#include <functional>
#include <optional>

namespace core {

/// The library format version this build reads and writes.
inline constexpr int LibraryFormat = 1;

/// An entry of the Favourites section: a collection or a note, in the person's order.
struct FavoriteRef
{
    enum class Kind { Collection, Note };

    Kind kind = Kind::Note;
    QString id;

    bool operator==(const FavoriteRef &) const = default;
};

/// Days a tombstone is kept in `library.json`.
inline constexpr int TombstoneDays = 90;

/// A permanently deleted note, remembered so that synchronisation does not bring it back from
/// a device that still has it.
struct Tombstone
{
    QString id;
    QDateTime deleted;

    bool operator==(const Tombstone &) const = default;
};

/// The contents of `library.json`.
struct LibraryInfo
{
    QString id;
    QDateTime created;
    QList<FavoriteRef> favorites;
    /// Tag colour keys by tag key (case-folded name); children of a coloured tag
    /// without a colour of their own take its colour (see tagColor()).
    QMap<QString, QString> tagColors;
    /// Days a trashed note is kept; no value keeps it until the trash is emptied.
    std::optional<int> trashRetentionDays = 30;
    /// Notes deleted permanently within the last TombstoneDays, ordered by time of deletion.
    QList<Tombstone> tombstones;
    /// Fields this build does not know, written back unchanged.
    QJsonObject extra;
    QJsonObject extraSettings;

    bool operator==(const LibraryInfo &) const = default;
};

/// A collection of `collections.json`.
struct Collection
{
    QString id;
    /// The parent collection, or no value for a top-level collection.
    std::optional<QString> parent;
    QString name;
    /// Glyph name; collections are created with `Folder`.
    QString icon = QStringLiteral("Folder");
    /// Tag colour key of the name.
    std::optional<QString> color;
    QDateTime created;
    QDateTime updated;
    /// Fields this build does not know, written back unchanged.
    QJsonObject extra;

    bool operator==(const Collection &) const = default;
};

/// Removes the tombstones older than TombstoneDays.
///
/// @param tombstones The tombstones to prune.
/// @param now The current time.
void dropExpiredTombstones(QList<Tombstone> &tombstones, const QDateTime &now);

/// Parses `library.json` of a library this build can write.
///
/// @param data The file's contents.
/// @param path The file, for errors.
/// @return The library information, or Malformed / NewerFormat.
Result<LibraryInfo> parseLibraryInfo(const QByteArray &data, const QString &path);

/// Serialises the library information into the contents of `library.json`.
QByteArray serializeLibraryInfo(const LibraryInfo &info);

/// Parses `collections.json`.
///
/// @param data The file's contents.
/// @param path The file, for errors.
/// @return The collections in their order, or Malformed / NewerFormat.
Result<QList<Collection>> parseCollections(const QByteArray &data, const QString &path);

/// Serialises the collections into the contents of `collections.json`; the list order is the
/// order within each parent.
QByteArray serializeCollections(const QList<Collection> &collections);

/// Returns a collection and all its descendants, the collection first. A damaged
/// `collections.json` may contain a cycle; each collection is listed once.
QStringList collectionSubtree(const QList<Collection> &collections, const QString &id);

/// A collection with its nesting level, as a flat list of the tree shows it.
struct CollectionLevel
{
    const Collection *collection = nullptr;
    int depth = 0;
};

/// Lists the collection tree depth first, each parent before its children in their order, for
/// flat lists of the tree such as pickers. Collections that do not hang from the top level (a
/// missing parent or a cycle in a damaged file) are left out.
///
/// @param collections The collections; the result points into this list.
/// @return The collections with their levels.
QList<CollectionLevel> collectionTree(const QList<Collection> &collections);

/// A library folder: its layout, `library.json`, the collection tree and the note files.
///
/// The library's files are the only source of truth. Every write is atomic. A library written
/// by a newer Jotter opens read-only and refuses writes.
class Library
{
public:
    /// Creates a new library in @p root, which must be missing or empty.
    ///
    /// @param root The library folder.
    /// @return The open library, or NotEmpty / Io.
    static Result<Library> create(const QString &root);

    /// Opens an existing library.
    ///
    /// @param root The library folder.
    /// @return The library, read-only when its format is newer; or NotFound / Malformed / Io.
    static Result<Library> open(const QString &root);

    /// Tells whether @p root holds a library (it has a `library.json`).
    static bool exists(const QString &root);

    const QString &root() const { return m_root; }
    const LibraryInfo &info() const { return m_info; }
    bool isReadOnly() const { return m_readOnly; }

    /// Returns the folder holding the note files.
    QString notesDir() const;

    /// Returns the file of a note.
    QString notePath(const QString &id) const;

    /// Returns the attachment folder of a note.
    QString attachmentsDir(const QString &noteId) const;

    /// Replaces `library.json`.
    Result<void> writeInfo(const LibraryInfo &info);

    /// Applies a user edit to the current on-disk metadata within a short local transaction.
    Result<void> changeInfo(const std::function<void(LibraryInfo &)> &change);

    /// Reads a note file.
    Result<Note> readNote(const QString &id) const;

    /// Writes a note file atomically, with the style sheet of the numbering styles it uses.
    Result<void> writeNote(const Note &note) const;

    /// Reads `rules.json`; a missing file holds no rules.
    ///
    /// @return The rules in their order, or Malformed / NewerFormat / Io.
    Result<QList<Rule>> readRules() const;

    /// Replaces `rules.json`; the list order is the order rules run in.
    Result<void> writeRules(const QList<Rule> &rules) const;

    /// Writes the generated table of contents, `index.html`, when its
    /// contents changed; an unchanged file is left alone, so watchers see no change.
    ///
    /// @param contents The document.
    /// @return Nothing, or ReadOnly / Io.
    Result<void> writeTableOfContents(const QByteArray &contents) const;

    /// Reads `collections.json`.
    Result<QList<Collection>> readCollections() const;

    /// Replaces `collections.json`; the list order is the order within each parent.
    Result<void> writeCollections(const QList<Collection> &collections) const;

private:
    Library(QString root, LibraryInfo info, bool readOnly);

    /// Refuses a write while the library is read-only.
    Result<void> checkWritable() const;

    QString m_root;
    LibraryInfo m_info;
    bool m_readOnly = false;
    mutable std::optional<QList<Collection>> m_collectionBase;
    mutable std::optional<QList<Rule>> m_ruleBase;
};

} // namespace core
