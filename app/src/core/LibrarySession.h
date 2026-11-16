#pragma once

#include "core/Index.h"
#include "core/Library.h"
#include "core/LibraryWatcher.h"

#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>

namespace core {

/// What deleting a collection does with its notes.
struct CollectionNotes
{
    enum class Kind {
        /// The notes lose the deleted memberships and stay in their other collections.
        Keep,
        /// As Keep, and the notes left in no collection move to the trash.
        TrashOrphans,
        /// The notes move into the target collection.
        Move,
    };

    Kind kind = Kind::Keep;
    /// The collection the notes move into, for Move.
    QString target;
};

/// A change of a note's collections made by rules, kept so it can be undone.
struct RuleChange
{
    QString noteId;
    QString title;
    QStringList collectionsBefore;
    QStringList collectionsAfter;
};

/// An open library with everything the interface needs around it: the index for lists, the
/// collection tree, a watcher for outside changes and background scans.
///
/// Scans run on a worker thread with their own index connection; the interface keeps reading
/// from its own connection meanwhile. Changes that arrive during a scan start one more scan
/// when it ends, never two at once.
class LibrarySession : public QObject
{
    Q_OBJECT

public:
    /// Opens the library at @p root and its index, then starts the first scan.
    ///
    /// @param root The library folder.
    /// @param parent The QObject parent.
    /// @return The session, or the error of opening the library or its index.
    static Result<std::unique_ptr<LibrarySession>> open(const QString &root,
                                                        QObject *parent = nullptr);

    ~LibrarySession() override;

    const Library &library() const { return m_library; }
    const Index &index() const { return m_index; }
    const QList<Collection> &collections() const { return m_collections; }
    const QList<Rule> &rules() const { return m_rules.rules(); }

    /// Tells whether the library folder is reachable; while it is not, nothing can be edited.
    bool isAvailable() const { return m_available; }

    /// Runs a bulk change on a worker with its own index connection, after editors save.
    /// Completion is delivered on the session thread; errors are also emitted through failed().
    /// @param operation The work, using only the supplied worker session.
    /// @param completed Optional completion callback, called on the owning session thread.
    void mutate(std::function<Result<void>(LibrarySession &)> operation,
                std::function<void(const Result<void> &)> completed = {});
    /// Tells whether queued or running bulk changes temporarily suspend editing.
    bool isMutating() const { return m_mutating; }

    /// Tells whether a scan is running.
    bool isScanning() const { return m_scan.isRunning(); }

    /// Checks the folder again and rescans; used by Retry after the library was unavailable.
    void refresh();

    /// Empties the index and fills it again from the files (Settings → Library → Rebuild index).
    void rebuildIndex();

    /// Saves a note: sets `updated` to now, writes the file and records it in the index at once,
    /// so lists show the change without waiting for a scan. A note that is new or whose tags
    /// changed is first filed by the rules; what they changed is announced by
    /// rulesApplied().
    ///
    /// @param note The note; its `updated` time, and its collections when a rule acts, are set
    ///        here.
    /// @param expected The version read by the caller; a concurrent change refuses the write.
    /// @return Nothing, or the error of writing (ReadOnly for a library of a newer Jotter).
    Result<void> saveNote(Note &note, const std::optional<Note> &expected = std::nullopt);

    /// Changes the metadata of several notes and saves each, for archive, trash and favourites.
    ///
    /// @param ids The notes to change.
    /// @param change Applied to each note's metadata before it is saved.
    /// @return Nothing, or the first error; notes before it are already saved.
    Result<void> updateNotes(const QStringList &ids, const std::function<void(NoteMeta &)> &change);

    /// Deletes notes permanently with their attachment folders, removes them from the
    /// favourites and records their tombstones.
    ///
    /// @param ids The notes to delete.
    /// @return Nothing, or the first error; the other notes are still deleted.
    Result<void> deleteNotes(const QStringList &ids);

    /// Deletes the notes that have been in the trash longer than the library's retention period;
    /// a library that keeps the trash until it is emptied deletes nothing.
    ///
    /// @return The number of notes deleted, or an error.
    Result<int> purgeTrash();

    /// Creates a collection at the end of its parent's children, with the Folder icon.
    ///
    /// @param name The collection's name.
    /// @param parent The parent collection, or no value for the top level.
    /// @return The new collection, or the error of writing `collections.json`.
    Result<Collection> createCollection(const QString &name, const std::optional<QString> &parent);

    /// Changes a collection's name, icon or colour.
    Result<void> updateCollection(const QString &id,
                                  const std::function<void(Collection &)> &change);

    /// Moves a collection under @p parent, before the sibling @p before (empty: at the end). A
    /// collection cannot move into itself or its descendants.
    ///
    /// @return Nothing, or an error; a refused move changes nothing.
    Result<void> moveCollection(const QString &id, const std::optional<QString> &parent,
                                const QString &before);

    /// Deletes a collection with its descendants and handles their notes, in
    /// every state, as @p notes says.
    ///
    /// @param id The collection.
    /// @param notes What happens to the notes of the collection and its descendants.
    /// @return Nothing, NotFound for a Move target that does not exist or is being deleted, or
    ///     the error of writing the notes or the collections.
    Result<void> deleteCollection(const QString &id, const CollectionNotes &notes = {});

    /// Adds notes to collections and removes them from others.
    Result<void> changeCollections(const QStringList &noteIds, const QStringList &add,
                                   const QStringList &remove);

    /// Adds and removes tags of notes.
    Result<void> changeTags(const QStringList &noteIds, const QStringList &add,
                            const QStringList &remove);

    /// Renames a tag and its children in every note, merging with a tag of the new name, and
    /// moves their colours; a tag of the new name keeps its own colour.
    ///
    /// @return The number of notes changed, or an error.
    Result<int> renameTag(const QString &from, const QString &to);

    /// Deletes a tag and its children from every note, with their colours.
    ///
    /// @return The number of notes changed, or an error.
    Result<int> deleteTag(const QString &tag);

    /// Replaces the rules (Settings → Rules) and emits rulesChanged(). New rules get their
    /// creation time, and every rule that differs from the stored one its update time.
    ///
    /// @param rules The rules in the order they run in.
    /// @return Nothing, or the error of writing `rules.json`.
    Result<void> saveRules(QList<Rule> rules);

    /// Lists the notes that Apply to existing notes would change: active notes matching the
    /// rule whose collections its action changes, whether or not the rule is enabled.
    ///
    /// @param rule The rule.
    /// @return The notes, or the error of reading the index.
    Result<QStringList> notesForRule(const Rule &rule) const;

    /// Carries out a rule's action on notes and announces the changes by rulesApplied().
    ///
    /// @param rule The rule.
    /// @param ids The notes, as notesForRule() listed them.
    /// @return Nothing, or the first error; notes before it are already changed.
    Result<void> applyRule(const Rule &rule, const QStringList &ids);

    /// Gives notes back the collections they had before rules changed them (Undo).
    Result<void> undoRules(const QList<RuleChange> &changes);

    /// Changes `library.json` (favourites, library settings) and emits infoChanged().
    ///
    /// @param change Applied to a copy of the library information before it is written.
    /// @return Nothing, or the error of writing.
    Result<void> updateInfo(const std::function<void(LibraryInfo &)> &change);

    /// Records the current size and modification time of a note's file as known to this
    /// session: after writing it, and when the editor loads it.
    void markSeen(const QString &id);

    /// Tells whether a note's file differs from the one this session last wrote or loaded,
    /// that is, another program changed it since. Notes never seen are not
    /// reported.
    bool changedOutside(const QString &id) const;

Q_SIGNALS:
    /// Emitted after a scan or an operation changed the index, and after the first scan.
    void notesChanged();
    /// Emitted when library bytes may have changed, including attachment-only replacements.
    void filesChanged();
    /// Emitted after saveNote() wrote a note.
    void noteSaved(const QString &id);
    void collectionsChanged();
    /// Emitted after updateInfo() changed the favourites or the library settings, and when
    /// `library.json` changed outside, such as by synchronisation.
    void infoChanged();
    void availabilityChanged(bool available);
    /// Emitted when the rules changed, here or in `rules.json` outside.
    void rulesChanged();
    /// Emitted after an operation in which rules changed the collections of notes.
    void rulesApplied(const QList<core::RuleChange> &changes);
    /// Emitted when a scan or reading the collections failed.
    void failed(const core::Error &error);
    /// Direct connections let editors veto a bulk operation if saving fails.
    void savingRequested(bool &accepted);
    /// Emitted when editing is suspended or resumed for a bulk operation.
    void mutationChanged();
    /// Emitted on the session thread after each note in a bulk operation.
    void mutationProgress(int done, int total);

private:
    LibrarySession(Library library, Index index, QObject *parent, bool worker = false);

    /// Starts a scan, or marks one as pending while another runs.
    void scan();
    void scanFinished();
    /// Reads `library.json` again after a change outside and makes it current.
    void reloadInfo();
    void reloadCollections();
    void reloadRules();
    /// Writes a note with the current time and records it in the index; the rules file it first
    /// when it is new or its tags changed, and a new title renames the links to it.
    Result<void> store(Note &note, const std::optional<Note> &expected = std::nullopt);
    /// Renames the links to a renamed note that still show its old title, in every note that
    /// links to it.
    void renameLinksTo(const QString &id, const QString &oldTitle, const QString &newTitle);
    /// Writes `index.html` again on a worker thread from the notes and collections as they are now.
    void writeTableOfContents();
    /// Emits rulesApplied() for the changes rules made during an operation.
    void announceRuleChanges();
    /// Reads, changes and saves each note, like updateNotes() but for the whole note.
    Result<void> rewriteNotes(const QStringList &ids, const std::function<void(Note &)> &change);
    /// Writes the collection tree and makes it current.
    Result<void> writeCollections(const QList<Collection> &collections);
    /// Returns the notes that have a tag or one of its children, in any state.
    Result<QStringList> notesTagged(const QString &tag) const;
    /// Changes the tag colours in `library.json` when a tag or one of its children has a colour;
    /// otherwise the file is left alone.
    ///
    /// @param tag The renamed or deleted tag.
    /// @param change Applied to the tag colours before they are written.
    /// @return Nothing, or the error of writing.
    Result<void> updateTagColors(const QString &tag,
                                 const std::function<void(QMap<QString, QString> &)> &change);

    Library m_library;
    Index m_index;
    std::unique_ptr<LibraryWatcher> m_watcher;
    struct MutationResult
    {
        Result<void> result;
        QList<RuleChange> rules;
    };
    struct Mutation
    {
        std::function<Result<void>(LibrarySession &)> operation;
        std::function<void(const Result<void> &)> completed;
    };
    /// Starts the first queued bulk operation; called only on the session thread.
    void startMutation();
    QList<Mutation> m_mutations;
    QFutureWatcher<MutationResult> m_mutation;
    bool m_mutating = false;
    bool m_worker = false;
    QList<Collection> m_collections;
    RuleEngine m_rules;
    /// Changes rules made during the running operation, announced when it ends.
    QList<RuleChange> m_ruleChanges;
    QFutureWatcher<Result<ScanReport>> m_scan;
    bool m_scanPending = false;
    bool m_clearPending = false;
    /// Whether the next scan also removes orphaned attachment files: at start and hourly.
    bool m_sweepPending = true;
    /// Whether the running scan started from an emptied index.
    bool m_scanClears = false;
    bool m_firstScan = true;
    bool m_available = true;
    /// Size and modification time of the files this session wrote or loaded, by note.
    QHash<QString, QString> m_written;
    QTimer m_purgeTimer;
    /// Regenerates the table of contents a few seconds after changes.
    QTimer m_contentsTimer;
    QFuture<void> m_contents;
};

} // namespace core
