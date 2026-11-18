#pragma once

#include "core/Error.h"
#include "core/Library.h"
#include "core/Search.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace core {

/// One note as the note list shows it, read from the index.
struct NoteSummary
{
    QString id;
    QString title;
    std::optional<QString> icon;
    std::optional<QString> color;
    QDateTime created;
    QDateTime updated;
    QStringList collections;
    QStringList tags;
    bool archived = false;
    bool trashed = false;
    bool hasLocation = false;
    /// The number of images and attachment files the note refers to.
    int attachments = 0;
    /// The start of the text, on one line; in search results the fragment around the matches.
    QString snippet;
    /// In search results, the matching words of the snippet.
    QList<TextSpan> matches;
};

/// Which notes a list shows.
struct NoteFilter
{
    enum class View {
        /// Every note that is neither archived nor trashed.
        All,
        /// Notes that are neither archived nor trashed and in no collection.
        Unfiled,
        /// The notes listed in `ids`, such as the favourites.
        Notes,
        /// Notes of the collections listed in `ids` (the caller adds descendants).
        Collections,
        /// Notes with the tag `tag` or one of its children.
        Tag,
        Archive,
        Trash,
    };

    View view = View::All;
    QStringList ids;
    QString tag;
    /// For Collections and Tag: also archived and trashed notes, as operations on a
    /// collection or tag need them; lists leave it off.
    bool anyState = false;
    /// For views of active notes: archived notes too, for a search with Include archived.
    bool includeArchived = false;
};

/// The order of a note list.
struct NoteSort
{
    enum class Key { Updated, Created, Title };

    Key key = Key::Updated;
    bool descending = true;
};

/// A tag with the number of notes carrying it, for the sidebar.
struct TagCount
{
    /// The tag in the spelling it was first used with.
    QString name;
    int count = 0;
};

/// A note file the index could not read, for Settings → Library → Problems.
struct Problem
{
    /// The file name inside the library's `notes` folder.
    QString path;
    Error error;
};

/// What a scan changed.
struct ScanReport
{
    int added = 0;
    int updated = 0;
    int removed = 0;
    /// Files read again because their size or modification time changed.
    int read = 0;
    /// Unreadable files recorded, read again or gone, which changes the list of problems.
    int problems = 0;
};

/// The local search index of a library: a SQLite database derived from the
/// note files, used for lists, counts and search. It holds nothing the library does not and is
/// rebuilt whenever its schema changes.
///
/// Each thread opens its own Index on the same file; the database uses write-ahead logging, so
/// the interface reads while a worker scans.
class Index
{
public:
    /// Opens or creates the index at @p path, rebuilding it when its schema is outdated.
    ///
    /// @param path The database file.
    /// @return The index, or Io.
    static Result<Index> open(const QString &path);

    /// Returns the index file of a library in the cache folder.
    static QString defaultPath(const QString &libraryId);

    Index(Index &&other) noexcept;
    Index &operator=(Index &&other) noexcept;
    Index(const Index &) = delete;
    Index &operator=(const Index &) = delete;
    ~Index();

    /// Brings the index up to date with the library's note files: files whose size or
    /// modification time changed are read again, vanished files are removed, and unreadable
    /// files are recorded as problems.
    ///
    /// @param library The library to scan.
    /// @param rebuild Replaces derived note rows atomically after successful enumeration.
    /// @return What changed, or Io.
    Result<ScanReport> scan(const Library &library, bool rebuild = false);

    /// Removes everything from the index; the next scan reads every file again.
    Result<void> clear();

    /// Forgets a note whose file Jotter has just deleted.
    Result<void> remove(const QString &id);

    /// Lists the notes moved to the trash before @p time, for the retention period.
    Result<QStringList> trashedBefore(const QDateTime &time) const;

    /// Records a note Jotter has just written, without waiting for the next scan.
    Result<void> put(const Note &note, const QString &path);

    /// Lists notes for a view in the given order.
    Result<QList<NoteSummary>> notes(const NoteFilter &filter, const NoteSort &sort) const;

    /// Searches the notes of a view. Every term must match in one of the
    /// scopes, in any word form; excluded terms must not; `#tag` terms and the filters narrow
    /// the notes further.
    ///
    /// @param filter The view searched; Search::includeArchived adds its archived notes.
    /// @param search The query, its scopes and the filters.
    /// @param collections The collection tree, for collection names and the descendants of the
    ///        collection filter.
    /// @param sort The order when nothing ranks the notes: a query without words to find.
    /// @return Notes ranked by relevance (BM25), then by updated time; the first 100 notes
    ///         matched in their text show the fragment of it around the matches, the others the
    ///         start of their text. Or Io.
    Result<QList<NoteSummary>> search(const NoteFilter &filter, const Search &search,
                                      const QList<Collection> &collections,
                                      const NoteSort &sort) const;

    /// Lists the notes that link to a note, by title; a note's links to
    /// itself are left out.
    ///
    /// @param id The linked note.
    /// @param anyState Whether trashed notes are listed too, as renaming their links needs.
    /// @return The linking notes, or Io.
    Result<QList<NoteSummary>> backlinks(const QString &id, bool anyState = false) const;

    /// Returns the summaries of the notes with the given identifiers that the index knows, such
    /// as the targets of a note's links.
    ///
    /// @return The summaries by identifier; unknown notes are missing; or Io.
    Result<QHash<QString, NoteSummary>> notesById(const QStringList &ids) const;

    /// Returns one note's summary, or no value when the index does not know it.
    Result<std::optional<NoteSummary>> note(const QString &id) const;

    /// Counts the notes of a view without listing them.
    Result<int> count(const NoteFilter &filter) const;

    /// Counts the active notes of every collection with its descendants, as a collection view
    /// lists them, in one query: a note in several collections of one subtree counts once.
    ///
    /// @param collections The collection tree.
    /// @return The counts by collection identifier, collections without notes included; or Io.
    Result<QHash<QString, int>> collectionCounts(const QList<Collection> &collections) const;

    /// Lists the tags of notes that are neither archived nor trashed, sorted by name.
    Result<QList<TagCount>> tags() const;

    /// Lists the note files that could not be read.
    Result<QList<Problem>> problems() const;

    /// Removes the attachment files that no note has referred to for the grace period.
    /// Files found unreferenced are recorded with the current time; a reference
    /// that comes back forgets them. The folders of notes that could not be read are left
    /// alone, as their references are unknown, and folders left empty without a note go.
    ///
    /// @param library The library, already scanned into this index.
    /// @param now The current time.
    /// @return The number of files removed, or Io.
    Result<int> removeOrphans(const Library &library, const QDateTime &now);

private:
    explicit Index(QString connection);

    QString m_connection;
};

} // namespace core
