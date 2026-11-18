#include "core/LibrarySession.h"

#include "core/Directory.h"
#include "core/FileGuard.h"
#include "core/Files.h"
#include "core/NoteLinks.h"
#include "core/TableOfContents.h"
#include "core/Tags.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtConcurrentRun>

#include <algorithm>

namespace {

/// The trash is checked for expired notes, and the attachment folders for files no note refers
/// to, at start and every hour.
constexpr int PurgeIntervalMs = 60 * 60 * 1000;

/// The table of contents is written this long after the last change.
constexpr int ContentsDelayMs = 3000;

/// An entry of the tag colours, as QMap::removeIf() passes it.
using TagColorEntry = QMap<QString, QString>::iterator;

} // namespace

namespace core {

Result<std::unique_ptr<LibrarySession>> LibrarySession::open(const QString &root, QObject *parent)
{
    auto library = Library::open(root);
    if (!library) {
        return std::unexpected(library.error());
    }
    auto index = Index::open(Index::defaultPath(library->info().id));
    if (!index) {
        return std::unexpected(index.error());
    }
    std::unique_ptr<LibrarySession> session(
        new LibrarySession(std::move(*library), std::move(*index), parent));
    session->reloadCollections();
    session->reloadRules();
    session->scan();
    return session;
}

LibrarySession::LibrarySession(Library library, Index index, QObject *parent, bool workerMode)
    : QObject(parent)
    , m_library(std::move(library))
    , m_index(std::move(index))
    , m_worker(workerMode)
{
    if (workerMode) {
        return;
    }
    m_watcher = std::make_unique<LibraryWatcher>(m_library.root());
    connect(m_watcher.get(), &LibraryWatcher::changed, this, &LibrarySession::refresh);
    connect(&m_scan, &QFutureWatcher<Result<ScanReport>>::finished, this,
            &LibrarySession::scanFinished);
    m_purgeTimer.setInterval(PurgeIntervalMs);
    connect(&m_purgeTimer, &QTimer::timeout, this, [this] {
        mutate([](LibrarySession &worker) -> Result<void> {
            auto purged = worker.purgeTrash();
            return purged ? Result<void>() : std::unexpected(purged.error());
        });
        m_sweepPending = true;
        scan();
    });
    m_purgeTimer.start();
    m_contentsTimer.setSingleShot(true);
    m_contentsTimer.setInterval(ContentsDelayMs);
    connect(&m_contentsTimer, &QTimer::timeout, this, &LibrarySession::writeTableOfContents);
    for (const auto signal : {&LibrarySession::notesChanged, &LibrarySession::collectionsChanged}) {
        connect(this, signal, &m_contentsTimer, qOverload<>(&QTimer::start));
    }
    connect(this, &LibrarySession::noteSaved, &m_contentsTimer, qOverload<>(&QTimer::start));
}

void LibrarySession::writeTableOfContents()
{
    if (m_library.isReadOnly() || !m_available) {
        return;
    }
    if (m_contents.isRunning()) {
        m_contentsTimer.start();
        return;
    }
    auto notes = m_index.notes({}, {});
    auto archived = m_index.notes({.view = NoteFilter::View::Archive, .ids = {}, .tag = {}}, {});
    if (!notes || !archived) {
        Q_EMIT failed(notes ? archived.error() : notes.error());
        return;
    }
    notes->append(*archived);
    m_contents = QtConcurrent::run([library = m_library, notes = std::move(*notes),
                                    collections = m_collections] {
        if (auto written = library.writeTableOfContents(tableOfContents(notes, collections));
            !written) {
            qWarning("Cannot write the table of contents: %s", qPrintable(written.error().detail));
        }
    });
}

LibrarySession::~LibrarySession()
{
    // The worker uses its own copy of the library and index path; waiting keeps its database
    // connection from outliving the application.
    m_mutation.waitForFinished();
    m_scan.waitForFinished();
    m_contents.waitForFinished();
}

void LibrarySession::mutate(std::function<Result<void>(LibrarySession &)> operation,
                            std::function<void(const Result<void> &)> completed)
{
    bool accepted = true;
    Q_EMIT savingRequested(accepted);
    if (!accepted) {
        auto error = fail(ErrorCode::Io, m_library.root(),
                          QStringLiteral("pending edits could not be saved"));
        if (completed) {
            completed(error);
        }
        Q_EMIT failed(error.error());
        return;
    }
    m_mutations.append({.operation = std::move(operation), .completed = std::move(completed)});
    if (m_mutating) {
        return;
    }
    m_mutating = true;
    Q_EMIT mutationChanged();
    startMutation();
}

void LibrarySession::startMutation()
{
    const QString root = m_library.root();
    const QString indexPath = Index::defaultPath(m_library.info().id);
    auto operation = m_mutations.first().operation;
    disconnect(&m_mutation, nullptr, this, nullptr);
    connect(&m_mutation, &QFutureWatcherBase::finished, this, [this] {
        const MutationResult outcome = m_mutation.result();
        auto completed = m_mutations.takeFirst().completed;
        reloadInfo();
        reloadCollections();
        reloadRules();
        if (m_mutations.isEmpty()) {
            m_mutating = false;
            Q_EMIT mutationChanged();
        }
        Q_EMIT notesChanged();
        if (!outcome.rules.isEmpty()) {
            Q_EMIT rulesApplied(outcome.rules);
        }
        if (!outcome.result) {
            Q_EMIT failed(outcome.result.error());
        }
        if (completed) {
            completed(outcome.result);
        }
        if (!m_mutations.isEmpty() && !m_mutation.isRunning()) {
            startMutation();
        }
    });
    m_mutation.setFuture(QtConcurrent::run([this, root, indexPath,
                                            operation = std::move(operation)] {
        auto library = Library::open(root);
        auto index = Index::open(indexPath);
        if (!library || !index) {
            return MutationResult{
                .result = std::unexpected(library ? index.error() : library.error()), .rules = {}};
        }
        LibrarySession worker(std::move(*library), std::move(*index), nullptr, true);
        worker.reloadCollections();
        worker.reloadRules();
        QList<RuleChange> rules;
        connect(&worker, &LibrarySession::rulesApplied, &worker,
                [&rules](const QList<RuleChange> &changes) { rules.append(changes); });
        connect(&worker, &LibrarySession::mutationProgress, this, &LibrarySession::mutationProgress,
                Qt::QueuedConnection);
        std::optional<Error> failure;
        connect(&worker, &LibrarySession::failed, &worker, [&failure](const Error &error) {
            if (!failure) {
                failure = error;
            }
        });
        auto result = operation(worker);
        if (result && failure) {
            result = std::unexpected(*failure);
        }
        return MutationResult{.result = std::move(result), .rules = std::move(rules)};
    }));
}

void LibrarySession::refresh()
{
    Q_EMIT filesChanged();
    const bool available = Library::exists(m_library.root());
    if (available != m_available) {
        m_available = available;
        Q_EMIT availabilityChanged(available);
    }
    if (available) {
        reloadInfo();
        reloadCollections();
        reloadRules();
        scan();
    }
}

void LibrarySession::rebuildIndex()
{
    m_clearPending = true;
    scan();
}

void LibrarySession::scan()
{
    if (m_scan.isRunning()) {
        m_scanPending = true;
        return;
    }
    m_scanPending = false;
    const bool clear = std::exchange(m_clearPending, false);
    // Orphaned attachments are looked for only in a writable, reachable library, after the
    // scan has brought every note's references up to date.
    const bool sweep =
        std::exchange(m_sweepPending, false) && !m_library.isReadOnly() && m_available;
    m_scanClears = clear;
    const Library library = m_library;
    m_scan.setFuture(QtConcurrent::run([library, clear, sweep]() -> Result<ScanReport> {
        auto index = Index::open(Index::defaultPath(library.info().id));
        if (!index) {
            return std::unexpected(index.error());
        }
        auto report = index->scan(library, clear);
        if (report && sweep) {
            if (auto removed = index->removeOrphans(library, now()); !removed) {
                return std::unexpected(removed.error());
            }
        }
        return report;
    }));
}

void LibrarySession::scanFinished()
{
    const Result<ScanReport> result = m_scan.result();
    if (!result) {
        Q_EMIT failed(result.error());
    } else if (m_firstScan || m_scanClears ||
               result->added + result->updated + result->removed + result->problems > 0) {
        Q_EMIT notesChanged();
    }
    const int retentionDays = m_library.info().trashRetentionDays.value_or(0);
    if (result && std::exchange(m_firstScan, false) && retentionDays > 0) {
        auto expired = m_index.trashedBefore(now().addDays(-retentionDays));
        if (expired && !expired->isEmpty()) {
            mutate([](LibrarySession &worker) -> Result<void> {
                auto purged = worker.purgeTrash();
                return purged ? Result<void>() : std::unexpected(purged.error());
            });
        }
    }
    if (m_scanPending || m_clearPending) {
        scan();
    }
}

void LibrarySession::reloadInfo()
{
    auto library = Library::open(m_library.root());
    if (!library) {
        Q_EMIT failed(library.error());
        return;
    }
    // The index belongs to the library's identifier; a folder that now holds another library is
    // not taken over by an open session.
    if (library->info().id != m_library.info().id) {
        return;
    }
    if (library->info() != m_library.info() || library->isReadOnly() != m_library.isReadOnly()) {
        m_library = std::move(*library);
        Q_EMIT infoChanged();
    }
}

void LibrarySession::reloadCollections()
{
    auto collections = m_library.readCollections();
    if (!collections) {
        Q_EMIT failed(collections.error());
        return;
    }
    if (*collections != m_collections) {
        m_collections = std::move(*collections);
        Q_EMIT collectionsChanged();
    }
}

Result<void> LibrarySession::store(Note &note, const std::optional<Note> &expected)
{
    // The index still holds the note as it was before this change.
    const auto known = m_index.note(note.meta.id);
    const std::optional<NoteSummary> previous = known ? *known : std::optional<NoteSummary>();
    std::optional<RuleChange> filed;
    if (!m_rules.rules().isEmpty()) {
        std::optional<QStringList> before;
        bool relevant = known.has_value();
        if (previous) {
            const auto keys = [](const QStringList &tags) {
                QStringList result;
                for (const QString &tag : tags) {
                    result.append(tagKey(tag));
                }
                result.sort();
                return result;
            };
            before = previous->tags;
            // Rules look only at the tags.
            relevant = keys(previous->tags) != keys(note.meta.tags);
        }
        RuleChange change{.noteId = note.meta.id,
                          .title = note.meta.title,
                          .collectionsBefore = note.meta.collections,
                          .collectionsAfter = {}};
        if (relevant && m_rules.apply(before, note.meta, m_collections)) {
            change.collectionsAfter = note.meta.collections;
            filed = std::move(change);
        }
    }
    note.meta.updated = now();
    {
        // Never hold a filesystem guard while waiting for SQLite: a background index scan
        // may already own its write transaction and need to read this same file.
        const FileGuard guard(m_library.notePath(note.meta.id));
        if (expected) {
            auto current = m_library.readNote(note.meta.id);
            if (!current) {
                return std::unexpected(current.error());
            }
            if (*current != *expected) {
                return fail(ErrorCode::Deferred, m_library.notePath(note.meta.id),
                            QStringLiteral("the note changed before it could be written"));
            }
        }
        if (auto written = m_library.writeNote(note); !written) {
            return written;
        }
    }
    markSeen(note.meta.id);
    if (filed) {
        m_ruleChanges.append(std::move(*filed));
    }
    if (auto indexed = m_index.put(note, m_library.notePath(note.meta.id)); !indexed) {
        // The source file is already durable. A derived-index failure must not turn a saved
        // buffer into a stale retry that can never pass its version check.
        Q_EMIT failed(indexed.error());
        if (!m_worker) {
            scan();
        }
    }
    if (previous && previous->title != note.meta.title) {
        renameLinksTo(note.meta.id, previous->title, note.meta.title);
    }
    return {};
}

void LibrarySession::renameLinksTo(const QString &id, const QString &oldTitle,
                                   const QString &newTitle)
{
    if (!m_worker) {
        mutate([id, oldTitle, newTitle](LibrarySession &worker) -> Result<void> {
            worker.renameLinksTo(id, oldTitle, newTitle);
            return {};
        });
        return;
    }

    const auto linking = m_index.backlinks(id, true);
    if (!linking) {
        Q_EMIT failed(linking.error());
        return;
    }
    for (const NoteSummary &summary : *linking) {
        auto note = m_library.readNote(summary.id);
        if (!note) {
            Q_EMIT failed(note.error());
            continue;
        }
        const Note before = *note;
        if (auto renamed = renameNoteLinks(note->body, id, oldTitle, newTitle)) {
            note->body = std::move(*renamed);
            // The linking note keeps its title, so this does not rename further.
            if (auto stored = store(*note, before); !stored) {
                Q_EMIT failed(stored.error());
            }
        }
    }
}

void LibrarySession::announceRuleChanges()
{
    if (!m_ruleChanges.isEmpty()) {
        Q_EMIT rulesApplied(std::exchange(m_ruleChanges, {}));
    }
}

Result<void> LibrarySession::saveNote(Note &note, const std::optional<Note> &expected)
{
    if (auto stored = store(note, expected); !stored) {
        return stored;
    }
    Q_EMIT noteSaved(note.meta.id);
    announceRuleChanges();
    return {};
}

void LibrarySession::reloadRules()
{
    auto rules = m_library.readRules();
    if (!rules) {
        Q_EMIT failed(rules.error());
        return;
    }
    if (*rules != m_rules.rules()) {
        m_rules = RuleEngine(std::move(*rules));
        Q_EMIT rulesChanged();
    }
}

Result<void> LibrarySession::saveRules(QList<Rule> rules)
{
    const QDateTime time = now();
    for (Rule &rule : rules) {
        const auto stored = std::ranges::find(m_rules.rules(), rule.id, &Rule::id);
        if (stored == m_rules.rules().end()) {
            rule.created = time;
            rule.updated = time;
        } else if (*stored != rule) {
            rule.updated = time;
        }
    }
    if (auto written = m_library.writeRules(rules); !written) {
        return written;
    }
    m_rules = RuleEngine(std::move(rules));
    Q_EMIT rulesChanged();
    return {};
}

Result<QStringList> LibrarySession::notesForRule(const Rule &rule) const
{
    const auto notes = m_index.notes({}, {});
    if (!notes) {
        return std::unexpected(notes.error());
    }
    const RuleEngine engine({rule});
    QStringList ids;
    for (const NoteSummary &note : *notes) {
        NoteMeta meta;
        meta.title = note.title;
        meta.tags = note.tags;
        meta.collections = note.collections;
        if (engine.matches(0, meta.tags) && RuleEngine::act(rule, meta, m_collections)) {
            ids.append(note.id);
        }
    }
    return ids;
}

Result<void> LibrarySession::applyRule(const Rule &rule, const QStringList &ids)
{
    // The rule's changes are recorded like those of rules running on an edit, for Undo;
    // rewriteNotes() announces them.
    auto result = rewriteNotes(ids, [&](Note &note) {
        RuleChange change{.noteId = note.meta.id,
                          .title = note.meta.title,
                          .collectionsBefore = note.meta.collections,
                          .collectionsAfter = {}};
        if (RuleEngine::act(rule, note.meta, m_collections)) {
            change.collectionsAfter = note.meta.collections;
            m_ruleChanges.append(std::move(change));
        }
    });
    return result;
}

Result<void> LibrarySession::undoRules(const QList<RuleChange> &changes)
{
    QHash<QString, const RuleChange *> byNote;
    QStringList ids;
    for (const RuleChange &change : changes) {
        byNote.insert(change.noteId, &change);
        ids.append(change.noteId);
    }
    return rewriteNotes(ids, [&](Note &note) {
        const RuleChange *change = byNote.value(note.meta.id);
        note.meta.collections = change->collectionsBefore;
    });
}

Result<void> LibrarySession::updateNotes(const QStringList &ids,
                                         const std::function<void(NoteMeta &)> &change)
{
    return rewriteNotes(ids, [&](Note &note) { change(note.meta); });
}

Result<void> LibrarySession::rewriteNotes(const QStringList &ids,
                                          const std::function<void(Note &)> &change)
{
    Result<void> result;
    int done = 0;
    for (const QString &id : ids) {
        auto note = m_library.readNote(id);
        if (!note) {
            result = std::unexpected(note.error());
            break;
        }
        const Note before = *note;
        change(*note);
        if (auto stored = store(*note, before); !stored) {
            result = stored;
            break;
        }
        ++done;
        if (done % 16 == 0 || done == ids.size()) {
            Q_EMIT mutationProgress(done, static_cast<int>(ids.size()));
        }
    }
    Q_EMIT notesChanged();
    announceRuleChanges();
    return result;
}

Result<void> LibrarySession::deleteNotes(const QStringList &ids)
{
    if (m_library.isReadOnly()) {
        return fail(ErrorCode::ReadOnly, m_library.root());
    }
    Result<void> result;
    const QDateTime deleted = now();
    if (auto recorded = updateInfo([&](LibraryInfo &info) {
            for (const QString &id : ids) {
                if (isValidId(id)) {
                    info.tombstones.removeIf(
                        [&](const Tombstone &entry) { return entry.id == id; });
                    info.tombstones.append({.id = id, .deleted = deleted});
                }
            }
            info.favorites.removeIf([&](const FavoriteRef &favorite) {
                return favorite.kind == FavoriteRef::Kind::Note && ids.contains(favorite.id);
            });
            dropExpiredTombstones(info.tombstones, deleted);
        });
        !recorded) {
        return recorded;
    }
    for (const QString &id : ids) {
        // An identifier always names one note; an empty or malformed one would name the whole
        // attachments folder below.
        if (!isValidId(id)) {
            continue;
        }
        const QString path = m_library.notePath(id);
        {
            const FileGuard guard(path);
            if (!removePath(path)) {
                result = fail(ErrorCode::Io, path, QStringLiteral("cannot delete the note file"));
                continue;
            }
            if (auto removed = removePath(m_library.attachmentsDir(id), true); !removed) {
                result = removed;
                continue;
            }
        }
        m_written.remove(id);
        if (auto removed = m_index.remove(id); !removed && result) {
            result = removed;
        }
    }
    Q_EMIT notesChanged();
    return result;
}

Result<int> LibrarySession::purgeTrash()
{
    const std::optional<int> days = m_library.info().trashRetentionDays;
    if (!days || m_library.isReadOnly() || !m_available) {
        return 0;
    }
    auto expired = m_index.trashedBefore(now().addDays(-*days));
    if (!expired) {
        return std::unexpected(expired.error());
    }
    if (expired->isEmpty()) {
        return 0;
    }
    if (auto deleted = deleteNotes(*expired); !deleted) {
        return std::unexpected(deleted.error());
    }
    return static_cast<int>(expired->size());
}

Result<void> LibrarySession::updateInfo(const std::function<void(LibraryInfo &)> &change)
{
    if (auto written = m_library.changeInfo(change); !written) {
        return written;
    }
    Q_EMIT infoChanged();
    return {};
}

bool LibrarySession::changedOutside(const QString &id) const
{
    const auto written = m_written.constFind(id);
    if (written == m_written.constEnd()) {
        return false;
    }
    return fileRevision(m_library.notePath(id)) != *written;
}

void LibrarySession::markSeen(const QString &id)
{
    m_written.insert(id, fileRevision(m_library.notePath(id)));
}

Result<void> LibrarySession::writeCollections(const QList<Collection> &collections)
{
    if (auto written = m_library.writeCollections(collections); !written) {
        return written;
    }
    auto current = m_library.readCollections();
    m_collections = current.value_or(collections);
    Q_EMIT collectionsChanged();
    return {};
}

Result<Collection> LibrarySession::createCollection(const QString &name,
                                                    const std::optional<QString> &parent)
{
    Collection collection{.id = newId(),
                          .parent = parent,
                          .name = name,
                          .icon = QStringLiteral("Folder"),
                          .color = std::nullopt,
                          .created = now(),
                          .updated = now(),
                          .extra = {}};
    QList<Collection> collections = m_collections;
    collections.append(collection);
    if (auto written = writeCollections(collections); !written) {
        return std::unexpected(written.error());
    }
    return collection;
}

Result<void> LibrarySession::updateCollection(const QString &id,
                                              const std::function<void(Collection &)> &change)
{
    QList<Collection> collections = m_collections;
    for (Collection &collection : collections) {
        if (collection.id == id) {
            change(collection);
            collection.updated = now();
            return writeCollections(collections);
        }
    }
    return fail(ErrorCode::NotFound, id, QStringLiteral("no such collection"));
}

Result<void> LibrarySession::moveCollection(const QString &id, const std::optional<QString> &parent,
                                            const QString &before)
{
    if (parent && collectionSubtree(m_collections, id).contains(*parent)) {
        return fail(ErrorCode::Malformed, id, QStringLiteral("a collection cannot hold itself"));
    }
    QList<Collection> collections = m_collections;
    const auto found = std::ranges::find(collections, id, &Collection::id);
    if (found == collections.end()) {
        return fail(ErrorCode::NotFound, id, QStringLiteral("no such collection"));
    }
    Collection moved = *found;
    collections.erase(found);
    moved.parent = parent;
    moved.updated = now();
    // The list order is the order within each parent, so inserting before the sibling (or at
    // the end of the list) places it among its new siblings.
    const auto target = std::ranges::find(collections, before, &Collection::id);
    collections.insert(target, moved);
    return writeCollections(collections);
}

Result<void> LibrarySession::deleteCollection(const QString &id, const CollectionNotes &notes)
{
    const QStringList subtree = collectionSubtree(m_collections, id);
    const bool move = notes.kind == CollectionNotes::Kind::Move;
    if (move &&
        (subtree.contains(notes.target) ||
         std::ranges::find(m_collections, notes.target, &Collection::id) == m_collections.end())) {
        return fail(ErrorCode::NotFound, notes.target);
    }
    auto members = m_index.notes(
        {.view = NoteFilter::View::Collections, .ids = subtree, .tag = {}, .anyState = true}, {});
    if (!members) {
        return std::unexpected(members.error());
    }
    QStringList noteIds;
    for (const NoteSummary &note : *members) {
        noteIds.append(note.id);
    }
    if (auto changed = updateNotes(noteIds,
                                   [&](NoteMeta &meta) {
                                       meta.collections.removeIf([&](const QString &collection) {
                                           return subtree.contains(collection);
                                       });
                                       if (move && !meta.collections.contains(notes.target)) {
                                           meta.collections.append(notes.target);
                                       }
                                       if (notes.kind == CollectionNotes::Kind::TrashOrphans &&
                                           meta.collections.isEmpty() && !meta.trashed) {
                                           meta.trashed = now();
                                       }
                                   });
        !changed) {
        return changed;
    }
    QList<Collection> collections = m_collections;
    collections.removeIf(
        [&](const Collection &collection) { return subtree.contains(collection.id); });
    if (auto written = writeCollections(collections); !written) {
        return written;
    }
    const auto deleted = [&](const FavoriteRef &favorite) {
        return favorite.kind == FavoriteRef::Kind::Collection && subtree.contains(favorite.id);
    };
    // `library.json` is written only when a deleted collection was a favourite.
    if (std::ranges::none_of(m_library.info().favorites, deleted)) {
        return {};
    }
    return updateInfo([&](LibraryInfo &info) { info.favorites.removeIf(deleted); });
}

Result<void> LibrarySession::changeCollections(const QStringList &noteIds, const QStringList &add,
                                               const QStringList &remove)
{
    return updateNotes(noteIds, [&](NoteMeta &meta) {
        meta.collections.removeIf(
            [&](const QString &collection) { return remove.contains(collection); });
        for (const QString &collection : add) {
            if (!meta.collections.contains(collection)) {
                meta.collections.append(collection);
            }
        }
    });
}

Result<void> LibrarySession::changeTags(const QStringList &noteIds, const QStringList &add,
                                        const QStringList &remove)
{
    return updateNotes(noteIds, [&](NoteMeta &meta) {
        meta.tags.removeIf([&](const QString &tag) {
            return std::ranges::any_of(remove,
                                       [&](const QString &r) { return tagKey(r) == tagKey(tag); });
        });
        meta.tags = mergeTags(meta.tags, add);
    });
}

Result<QStringList> LibrarySession::notesTagged(const QString &tag) const
{
    auto notes =
        m_index.notes({.view = NoteFilter::View::Tag, .ids = {}, .tag = tag, .anyState = true}, {});
    if (!notes) {
        return std::unexpected(notes.error());
    }
    QStringList ids;
    for (const NoteSummary &note : *notes) {
        ids.append(note.id);
    }
    return ids;
}

Result<void>
LibrarySession::updateTagColors(const QString &tag,
                                const std::function<void(QMap<QString, QString> &)> &change)
{
    const QList<QString> keys = m_library.info().tagColors.keys();
    if (std::ranges::none_of(keys, [&](const QString &key) { return isTagWithin(key, tag); })) {
        return {};
    }
    return updateInfo([&](LibraryInfo &info) { change(info.tagColors); });
}

Result<int> LibrarySession::renameTag(const QString &from, const QString &to)
{
    const auto ids = notesTagged(from);
    if (!ids) {
        return std::unexpected(ids.error());
    }
    const auto renamed = updateNotes(*ids, [&](NoteMeta &meta) {
        QStringList tags;
        for (const QString &tag : meta.tags) {
            tags.append(renamedTag(tag, from, to).value_or(tag));
        }
        meta.tags = mergeTags(tags, {});
    });
    if (!renamed) {
        return std::unexpected(renamed.error());
    }
    // Colours move with their tags; a tag merged into one with a colour keeps that colour.
    const auto moveColors = [&](QMap<QString, QString> &colors) {
        QMap<QString, QString> moved;
        colors.removeIf([&](TagColorEntry entry) {
            if (!isTagWithin(entry.key(), from)) {
                return false;
            }
            moved.insert(tagKey(*renamedTag(entry.key(), from, to)), entry.value());
            return true;
        });
        for (auto it = moved.cbegin(); it != moved.cend(); ++it) {
            if (!colors.contains(it.key())) {
                colors.insert(it.key(), it.value());
            }
        }
    };
    if (const auto colored = updateTagColors(from, moveColors); !colored) {
        return std::unexpected(colored.error());
    }
    return static_cast<int>(ids->size());
}

Result<int> LibrarySession::deleteTag(const QString &tag)
{
    const auto ids = notesTagged(tag);
    if (!ids) {
        return std::unexpected(ids.error());
    }
    const auto deleted = updateNotes(*ids, [&](NoteMeta &meta) {
        meta.tags.removeIf([&](const QString &candidate) { return isTagWithin(candidate, tag); });
    });
    if (!deleted) {
        return std::unexpected(deleted.error());
    }
    const auto removeColors = [&](QMap<QString, QString> &colors) {
        colors.removeIf([&](TagColorEntry entry) { return isTagWithin(entry.key(), tag); });
    };
    if (const auto uncolored = updateTagColors(tag, removeColors); !uncolored) {
        return std::unexpected(uncolored.error());
    }
    return static_cast<int>(ids->size());
}

} // namespace core
