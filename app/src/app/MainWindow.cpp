#include "app/MainWindow.h"

#include "app/ExportDialog.h"
#include "app/Locator.h"
#include "app/MembershipDialog.h"
#include "app/NoteList.h"
#include "app/Settings.h"
#include "app/Sidebar.h"
#include "core/Files.h"
#include "core/LibrarySession.h"
#include "core/SyncEngine.h"
#include "core/Tags.h"
#include "editor/NoteEditor.h"
#include "ui/Icons.h"
#include "ui/Popups.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFutureWatcher>
#include <QHash>
#include <QKeyEvent>
#include <QScopedValueRollback>
#include <QSet>
#include <QShortcut>
#include <QSplitter>
#include <QtConcurrentRun>

#include <algorithm>

namespace {

// Below 900 pixels the sidebar starts hidden, below 600 the panes show one at a time.
constexpr int MediumWidth = 900;
constexpr int NarrowWidth = 600;

/// Returns how many of @p total notes have an entry, as a check state.
Qt::CheckState membership(int count, qsizetype total)
{
    if (count == 0) {
        return Qt::Unchecked;
    }
    return count == static_cast<int>(total) ? Qt::Checked : Qt::PartiallyChecked;
}

} // namespace

MainWindow::MainWindow(Settings &settings, QWidget *parent)
    : QMainWindow(parent)
    , m_settings(settings)
    , m_splitter(new QSplitter(this))
    , m_sidebar(new Sidebar(m_splitter))
    , m_list(new NoteList(m_splitter))
    , m_editor(new editor::NoteEditor(m_splitter))
    , m_toast(new ui::Toast(this))
    , m_locator(new Locator(this))
{
    const ui::Theme &theme = *ui::Theme::current();
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(1);
    m_sidebar->setMinimumWidth(theme.px(11));
    m_list->setMinimumWidth(theme.px(16));
    m_editor->setMinimumWidth(theme.px(18));
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setStretchFactor(2, 1);
    setCentralWidget(m_splitter);
    setMinimumSize(theme.px(20), theme.px(20));

    if (!m_splitter->restoreState(m_settings.splitterState())) {
        m_splitter->setSizes({theme.px(15), theme.px(22), theme.px(40)});
    }
    if (!restoreGeometry(m_settings.windowGeometry())) {
        resize(theme.px(80), theme.px(52));
    }

    connect(m_sidebar, &Sidebar::viewSelected, this, &MainWindow::showView);
    connect(m_sidebar, &Sidebar::settingsRequested, this, &MainWindow::settingsRequested);
    connect(m_sidebar, &Sidebar::entryClicked, this, [this] {
        if (m_layout == Layout::Narrow) {
            showPane(Pane::List);
        }
    });
    connect(m_list, &NoteList::noteSelected, this, [this](const QString &id) {
        openNote(id);
        if (m_layout == Layout::Narrow) {
            showPane(Pane::Note);
        }
    });
    connect(m_list, &NoteList::sortChanged, this, [this](const core::NoteSort &sort) {
        m_settings.setNoteSort(sort);
        refreshList();
    });
    m_wideSidebar = m_settings.sidebarVisible();
    connect(m_list, &NoteList::sidebarRequested, this, [this] {
        switch (m_layout) {
        case Layout::Wide:
            m_wideSidebar = !m_wideSidebar;
            break;
        case Layout::Medium:
            m_sidebarOpen = !m_sidebarOpen;
            break;
        case Layout::Narrow:
            m_pane = Pane::Sidebar;
            break;
        }
        updateLayout();
    });
    connect(m_list, &NoteList::retryRequested, this, [this] {
        if (m_session != nullptr) {
            m_session->refresh();
        }
    });
    connect(m_list, &NoteList::newNoteRequested, this, &MainWindow::createNote);
    connect(m_list, &NoteList::searchChanged, this, &MainWindow::refreshList);
    auto *searchNotes = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")), this);
    connect(searchNotes, &QShortcut::activated, this, [this] {
        setFocusMode(false);
        if (m_layout == Layout::Narrow) {
            showPane(Pane::List);
        }
        m_list->focusSearch();
    });
    connect(m_list, &NoteList::subcollectionsToggled, this, [this](bool included) {
        QStringList own = m_settings.ownNotesOnly();
        own.removeAll(m_viewRef.id);
        if (!included) {
            own.append(m_viewRef.id);
        }
        m_settings.setOwnNotesOnly(own);
        refreshList();
    });
    connect(m_sidebar, &Sidebar::failed, this,
            [this](const QString &message) { m_toast->showMessage(message); });
    connect(m_sidebar, &Sidebar::message, this,
            [this](const QString &text) { m_toast->showMessage(text); });
    connect(m_sidebar, &Sidebar::emptyTrashRequested, this, &MainWindow::emptyTrash);
    connect(m_editor, &editor::NoteEditor::message, this,
            [this](const QString &message) { m_toast->showMessage(message); });
    auto *newCollection = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")), this);
    connect(newCollection, &QShortcut::activated, this,
            [this] { m_sidebar->createCollection(std::nullopt); });
    connect(m_list, &NoteList::emptyTrashRequested, this, &MainWindow::emptyTrash);
    connect(m_list, &NoteList::actionRequested, this, &MainWindow::perform);
    connect(m_editor, &editor::NoteEditor::actionRequested, this, &MainWindow::perform);
    auto *newNote = new QShortcut(QKeySequence::New, this);
    connect(newNote, &QShortcut::activated, this, &MainWindow::createNote);
    connect(m_editor, &editor::NoteEditor::backRequested, this, [this] { showPane(Pane::List); });
    connect(m_editor, &editor::NoteEditor::titleChanged, this, &QWidget::setWindowTitle);
    connect(m_editor, &editor::NoteEditor::openNoteRequested, this, &MainWindow::revealNote);
    connect(m_editor, &editor::NoteEditor::noteLinkActivated, this, &MainWindow::followLink);
    auto *back = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Left), this);
    connect(back, &QShortcut::activated, this, &MainWindow::goBack);

    // Geotags of new notes, and Add location from the editor.
    m_editor->setLocationsEnabled(m_settings.addLocation());
    connect(m_editor, &editor::NoteEditor::locationRequested, m_locator, &Locator::locate);
    connect(m_locator, &Locator::located, this,
            [this](const QString &id, const core::GeoLocation &location) {
                // The note may have been discarded meanwhile, as an empty new note is.
                if (m_session == nullptr || !isWritable() ||
                    !m_session->index().note(id).value_or(std::nullopt)) {
                    return;
                }
                if (auto saved = m_session->updateNotes(
                        {id}, [&](core::NoteMeta &meta) { meta.location = location; });
                    !saved) {
                    m_toast->showMessage(core::describe(saved.error()));
                }
            });
    connect(m_locator, &Locator::failed, m_editor, &editor::NoteEditor::setLocationUnavailable);

    // Quitting without closing the window (the session ends) still keeps the window state.
    connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::saveState);

    auto *settingsShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+S")), this);
    connect(settingsShortcut, &QShortcut::activated, this, &MainWindow::settingsRequested);
    auto *focusMode = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+D")), this);
    connect(focusMode, &QShortcut::activated, this, [this] { setFocusMode(!m_focusMode); });
    connect(m_editor, &editor::NoteEditor::focusModeRequested, this,
            [this] { setFocusMode(!m_focusMode); });
    // Focus mode needs an open note: without one, the panes come back to choose another.
    connect(m_editor, &editor::NoteEditor::noteClosed, this, [this] { setFocusMode(false); });
    auto *syncNow = new QShortcut(QKeySequence(Qt::Key_F5), this);
    connect(syncNow, &QShortcut::activated, this, &MainWindow::syncRequested);
    connect(m_sidebar, &Sidebar::syncRequested, this, &MainWindow::syncRequested);
    auto *nextPane = new QShortcut(QKeySequence(Qt::Key_F6), this);
    connect(nextPane, &QShortcut::activated, this, [this] { focusNextPane(true); });
    auto *previousPane = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F6), this);
    connect(previousPane, &QShortcut::activated, this, [this] { focusNextPane(false); });
}

bool MainWindow::setSession(core::LibrarySession *session)
{
    if (!m_editor->setSession(session)) {
        return false;
    }
    if (m_session != nullptr) {
        m_session->disconnect(this);
    }
    m_session = session;
    m_announcedProblems.clear();
    // A search of the previous library that is still running is not shown.
    ++m_listGeneration;
    m_sidebar->setSession(session);
    if (session == nullptr) {
        return true;
    }
    // A scan reports its end with notesChanged, so that is when new problems are known.
    connect(session, &core::LibrarySession::notesChanged, this, [this] {
        m_sidebar->reload();
        refreshList();
        announceProblems();
    });
    connect(session, &core::LibrarySession::noteSaved, this, [this] {
        m_sidebar->reload();
        refreshList();
    });
    connect(session, &core::LibrarySession::infoChanged, this, [this] {
        m_sidebar->reload();
        refreshList();
    });
    connect(session, &core::LibrarySession::collectionsChanged, this, [this] {
        m_sidebar->reload();
        refreshList();
    });
    connect(session, &core::LibrarySession::rulesApplied, this, &MainWindow::announceRules);
    connect(session, &core::LibrarySession::failed, this,
            [this](const core::Error &error) { m_toast->showMessage(core::describe(error)); });
    connect(session, &core::LibrarySession::mutationChanged, this, [this] {
        m_sidebar->setEnabled(!m_session->isMutating());
        m_list->setEnabled(!m_session->isMutating());
    });
    connect(session, &core::LibrarySession::mutationProgress, this, [this](int done, int total) {
        m_toast->showMessage(tr("Updating library: %1 of %2 notes").arg(done).arg(total));
    });
    connect(session, &core::LibrarySession::availabilityChanged, this, [this](bool available) {
        m_list->setUnavailable(!available, m_session->library().root());
    });

    QStringList expanded = m_settings.expandedItems();
    if (expanded.isEmpty()) {
        expanded = {QStringLiteral("section:favorites"), QStringLiteral("section:collections"),
                    QStringLiteral("section:tags")};
    }
    m_sidebar->setExpandedItems(expanded);
    m_noteId = m_settings.lastNote();
    const View last = View::fromKey(m_settings.lastView());
    m_sidebar->selectView(last);
    showView(last);
    // The index may know problems already, when its scan ended before the window got the session.
    announceProblems();
    return true;
}

void MainWindow::showView(const View &view)
{
    m_viewRef = view;
    if (view.kind == View::Kind::FavoriteNote) {
        m_noteId = view.id;
    }
    refreshList();
    if (m_layout == Layout::Narrow) {
        showPane(view.kind == View::Kind::FavoriteNote ? Pane::Note : Pane::List);
    }
}

void MainWindow::refreshList()
{
    // Filling the list can save the open note, which announces a change and asks for another
    // refresh; that one runs after this one instead of inside it.
    if (m_refreshing) {
        m_refreshPending = true;
        return;
    }
    const QScopedValueRollback<bool> refreshing(m_refreshing, true);
    do {
        m_refreshPending = false;
        fillList();
    } while (m_refreshPending);
}

void MainWindow::fillList()
{
    if (m_session == nullptr) {
        return;
    }
    const core::NoteSort sort = m_settings.noteSort();
    const core::LibraryInfo &info = m_session->library().info();
    // A collection lists its subcollections' notes too, unless switched off for it.
    const bool isCollection = m_viewRef.kind == View::Kind::Collection;
    const bool subcollections = !isCollection || !m_settings.ownNotesOnly().contains(m_viewRef.id);
    const bool hasChildren =
        isCollection && core::collectionSubtree(m_session->collections(), m_viewRef.id).size() > 1;
    m_list->setSubcollectionsToggle(hasChildren, subcollections);
    // A search covers the view, or all notes with Search everywhere; the archive and the
    // trash are searched only in their own views.
    const bool ownState =
        m_viewRef.kind == View::Kind::Archive || m_viewRef.kind == View::Kind::Trash;
    m_list->setSearchWidening(m_viewRef.kind != View::Kind::All && !ownState, !ownState);
    m_list->setSearchSources(m_session->collections(),
                             m_session->index().tags().value_or(QList<core::TagCount>()));
    const core::Search search = m_list->search();
    const bool searching = search.isActive();
    const View listed = searching && m_list->searchesEverywhere() ? View() : m_viewRef;
    const core::NoteFilter filter =
        filterFor(listed, info, m_session->collections(), subcollections);
    const quint64 generation = ++m_listGeneration;
    if (!searching) {
        showNotes(listed, m_session->index().notes(filter, sort), search);
    } else if (!m_searchRunning) {
        // A running search sees the newer fill when it ends and fills the list again.
        startSearch(listed, filter, search, generation);
    }
}

void MainWindow::startSearch(const View &listed, const core::NoteFilter &filter,
                             const core::Search &search, quint64 generation)
{
    using Found = core::Result<QList<core::NoteSummary>>;
    auto *watcher = new QFutureWatcher<Found>(this);
    m_searchRunning = true;
    const core::LibrarySession *session = m_session;
    connect(watcher, &QFutureWatcher<Found>::finished, this,
            [this, watcher, session, listed, search, generation] {
                watcher->deleteLater();
                m_searchRunning = false;
                if (m_session != session) {
                    return;
                }
                if (generation != m_listGeneration) {
                    refreshList();
                    return;
                }
                {
                    const QScopedValueRollback<bool> refreshing(m_refreshing, true);
                    m_refreshPending = false;
                    showNotes(listed, watcher->result(), search);
                }
                if (m_refreshPending) {
                    refreshList();
                }
            });
    // The worker opens its own connection to the index, as the interface's may not be shared
    // between threads.
    watcher->setFuture(QtConcurrent::run(
        [path = core::Index::defaultPath(m_session->library().info().id), filter, search,
         collections = m_session->collections(), sort = m_settings.noteSort()]() -> Found {
            auto index = core::Index::open(path);
            if (!index) {
                return std::unexpected(index.error());
            }
            return index->search(filter, search, collections, sort);
        }));
}

void MainWindow::showNotes(const View &listed, core::Result<QList<core::NoteSummary>> notes,
                           const core::Search &search)
{
    const core::LibraryInfo &info = m_session->library().info();
    m_editor->setSearchTerms(search.isActive() && search.scopes.testFlag(core::SearchScope::Text)
                                 ? core::parseQuery(search.query)
                                 : QList<core::SearchTerm>());
    if (!notes) {
        m_list->setEmptyText(QStringLiteral("Error"), tr("The note list could not be read"),
                             core::describe(notes.error()));
        notes = QList<core::NoteSummary>();
    }

    switch (m_viewRef.kind) {
    case View::Kind::All:
        m_list->setEmptyText(QStringLiteral("AllNotes"), tr("No notes yet"),
                             tr("Notes of this library appear here."));
        break;
    case View::Kind::Unfiled:
        m_list->setEmptyText(QStringLiteral("Inbox"), tr("No unfiled notes"),
                             tr("Notes that are not in a collection appear here."));
        break;
    case View::Kind::FavoriteNote:
        m_list->setEmptyText(QStringLiteral("Favorite"), tr("No favourite notes"), {});
        break;
    case View::Kind::Collection:
        m_list->setEmptyText(QStringLiteral("Folder"), tr("This collection is empty"),
                             tr("Notes added to this collection or to its subcollections appear "
                                "here."));
        break;
    case View::Kind::Tag:
        m_list->setEmptyText(QStringLiteral("Tag"), tr("No notes with this tag"), {});
        break;
    case View::Kind::Archive:
        m_list->setEmptyText(QStringLiteral("Archive"), tr("The archive is empty"),
                             tr("Archived notes are kept here, out of the way."));
        break;
    case View::Kind::Trash:
        m_list->setEmptyText(
            QStringLiteral("Delete"), tr("The trash is empty"),
            info.trashRetentionDays
                ? tr("Deleted notes stay here for %n day(s).", nullptr, *info.trashRetentionDays)
                : tr("Deleted notes stay here until you empty the trash."));
        break;
    }
    NoteList::Mode mode = NoteList::Mode::Notes;
    if (m_viewRef.kind == View::Kind::Archive) {
        mode = NoteList::Mode::Archive;
    } else if (m_viewRef.kind == View::Kind::Trash) {
        mode = NoteList::Mode::Trash;
    }
    m_list->setMode(mode, isWritable());
    QSet<QString> favorites;
    for (const core::FavoriteRef &favorite : info.favorites) {
        if (favorite.kind == core::FavoriteRef::Kind::Note) {
            favorites.insert(favorite.id);
        }
    }
    m_list->setFavorites(favorites);
    m_list->setTagColors(info.tagColors);
    m_list->setNotes(titleFor(listed, m_session->collections()), std::move(*notes),
                     m_settings.noteSort());
    m_list->selectNote(m_noteId);
    if (m_list->currentNote().isEmpty()) {
        m_editor->closeNote();
    } else {
        openNote(m_list->currentNote());
    }
}

void MainWindow::setSyncStatus(const SyncStatus &status, const QString &toolTip)
{
    m_sidebar->setSyncStatus(status, toolTip);
}

void MainWindow::announceConflicts(const core::SyncReport &report)
{
    if (!report.skipped.isEmpty()) {
        m_toast->showMessage(tr(
            "Some files changed during synchronisation and were deferred. Retry synchronisation."));
    } else if (!report.noteConflicts.isEmpty()) {
        const QString copy = report.noteConflicts.first();
        m_toast->showMessage(tr("%n note(s) changed on two devices. Your version was kept as a "
                                "conflict copy.",
                                nullptr, static_cast<int>(report.noteConflicts.size())),
                             tr("Open copy"), [this, copy] { revealNote(copy); });
    } else if (!report.fileConflicts.isEmpty()) {
        m_toast->showMessage(tr("%n attachment(s) changed on two devices. Your version was kept "
                                "beside the other one as a conflict copy.",
                                nullptr, static_cast<int>(report.fileConflicts.size())));
    }
}

void MainWindow::openNote(const QString &id)
{
    if (m_editor->openNote(id)) {
        m_noteId = id;
    } else {
        m_list->selectNote(m_editor->noteId());
    }
}

void MainWindow::revealNote(const QString &id)
{
    m_noteId = id;
    m_list->selectNote(id);
    if (m_list->currentNote() != id && m_session != nullptr) {
        // The note is not in this view: the view that holds it shows it.
        m_list->clearSearch();
        View view;
        if (const auto note = m_session->index().note(id); note && note->has_value()) {
            if ((*note)->trashed) {
                view.kind = View::Kind::Trash;
            } else if ((*note)->archived) {
                view.kind = View::Kind::Archive;
            }
        }
        m_sidebar->selectView(view);
        showView(view);
    }
    openNote(id);
    if (m_layout == Layout::Narrow) {
        showPane(Pane::Note);
    }
}

void MainWindow::followLink(const QString &id)
{
    if (m_session == nullptr || !m_session->index().note(id).value_or(std::nullopt)) {
        m_toast->showMessage(tr("Note not found"));
        return;
    }
    if (!m_noteId.isEmpty() && m_noteId != id) {
        m_history.append(m_noteId);
        // A long trail of links keeps only its recent part.
        constexpr qsizetype MaxHistory = 100;
        if (m_history.size() > MaxHistory) {
            m_history.removeFirst();
        }
    }
    revealNote(id);
}

void MainWindow::goBack()
{
    while (m_session != nullptr && !m_history.isEmpty()) {
        const QString previous = m_history.takeLast();
        // Notes deleted since are skipped.
        if (m_session->index().note(previous).value_or(std::nullopt)) {
            revealNote(previous);
            return;
        }
    }
}

void MainWindow::locateNewNote(const QString &noteId)
{
    if (m_settings.addLocation()) {
        m_locator->locate(noteId);
    }
}

void MainWindow::setLocationsEnabled(bool enabled)
{
    m_editor->setLocationsEnabled(enabled);
}

void MainWindow::announceProblems()
{
    const auto problems = m_session->index().problems();
    if (!problems) {
        return;
    }
    QSet<QString> files;
    for (const core::Problem &problem : *problems) {
        files.insert(problem.path);
    }
    const bool fresh = std::ranges::any_of(
        files, [this](const QString &file) { return !m_announcedProblems.contains(file); });
    m_announcedProblems = files;
    if (fresh) {
        m_toast->showMessage(
            tr("%n note file(s) could not be read.", nullptr, static_cast<int>(files.size())),
            tr("Show"), [this] { Q_EMIT problemsRequested(); });
    }
}

bool MainWindow::isWritable() const
{
    return m_session != nullptr && !m_session->library().isReadOnly() && m_session->isAvailable();
}

void MainWindow::createNote()
{
    if (!isWritable()) {
        return;
    }
    // The archive, the trash and the favourites cannot hold a new note: it goes to All notes.
    if (m_viewRef.kind == View::Kind::Archive || m_viewRef.kind == View::Kind::Trash ||
        m_viewRef.kind == View::Kind::FavoriteNote) {
        m_viewRef = View();
        m_sidebar->selectView(m_viewRef);
    }
    // The new note would not match a search, which would hide it at once.
    m_list->clearSearch();
    core::Note note = core::makeNote();
    if (m_viewRef.kind == View::Kind::Collection) {
        note.meta.collections = {m_viewRef.id};
    } else if (m_viewRef.kind == View::Kind::Tag) {
        note.meta.tags = {m_viewRef.id};
    }
    m_noteId = note.meta.id;
    if (auto saved = m_session->saveNote(note); !saved) {
        m_toast->showMessage(core::describe(saved.error()));
        return;
    }
    refreshList();
    openNote(note.meta.id);
    m_editor->markNew();
    locateNewNote(note.meta.id);
    if (m_layout == Layout::Narrow) {
        showPane(Pane::Note);
    }
    m_editor->focusTitle();
}

void MainWindow::perform(editor::NoteAction action, const QStringList &ids)
{
    using editor::NoteAction;
    // Export only reads, so it works in a read-only library too.
    if (ids.isEmpty() || (action != NoteAction::Export && !isWritable())) {
        return;
    }
    // Unsaved typing goes to disk first, so the action works on the latest text.
    if (!m_editor->saveNow()) {
        return;
    }
    if (action == NoteAction::Export) {
        ExportDialog dialog(m_session->library(), ids, this);
        if (dialog.exec() == QDialog::Accepted) {
            m_toast->showMessage(tr("Exported to %1.").arg(dialog.savedName()));
        }
        return;
    }
    const int count = static_cast<int>(ids.size());
    core::LibrarySession *session = m_session;
    if (action == NoteAction::Collections) {
        if (auto result = chooseCollections(ids); !result) {
            m_toast->showMessage(core::describe(result.error()));
        }
        return;
    }
    if (action == NoteAction::Tags) {
        if (auto result = chooseTags(ids); !result) {
            m_toast->showMessage(core::describe(result.error()));
        }
        return;
    }
    if (action == NoteAction::Favorite || action == NoteAction::Unfavorite) {
        auto result = session->updateInfo([&](core::LibraryInfo &info) {
            for (const QString &id : ids) {
                const auto same = [&](const core::FavoriteRef &ref) {
                    return ref.kind == core::FavoriteRef::Kind::Note && ref.id == id;
                };
                if (action == NoteAction::Favorite && !std::ranges::any_of(info.favorites, same)) {
                    info.favorites.append({.kind = core::FavoriteRef::Kind::Note, .id = id});
                } else if (action == NoteAction::Unfavorite) {
                    info.favorites.removeIf(same);
                }
            }
        });
        if (!result) {
            m_toast->showMessage(core::describe(result.error()));
        }
        return;
    }
    if (action == NoteAction::DeletePermanently) {
        if (ui::confirmDestructive(this, tr("Delete %n note(s) permanently?", nullptr, count),
                                   tr("The notes and their attachments are removed from the "
                                      "library. This cannot be undone."),
                                   tr("Delete"))) {
            session->mutate(
                [ids](core::LibrarySession &worker) { return worker.deleteNotes(ids); });
        }
        return;
    }
    const bool archive = action == NoteAction::Archive || action == NoteAction::Unarchive;
    const bool on = action == NoteAction::Archive || action == NoteAction::Trash;
    const auto field = archive ? &core::NoteMeta::archived : &core::NoteMeta::trashed;
    const QPointer<MainWindow> self(this);
    const QPointer<core::LibrarySession> origin(session);
    session->mutate(
        [ids, field, on](core::LibrarySession &worker) {
            return worker.updateNotes(ids, [field, on](core::NoteMeta &meta) {
                meta.*field = on ? std::optional<QDateTime>(core::now()) : std::nullopt;
            });
        },
        [self, origin, ids, field, on, archive, count](const core::Result<void> &result) {
            if (!self || !origin || self->m_session != origin || !result) {
                return;
            }
            if (on) {
                self->m_toast->showMessage(
                    archive ? tr("Archived %n note(s).", nullptr, count)
                            : tr("Moved %n note(s) to the trash.", nullptr, count),
                    tr("Undo"), [self, origin, ids, field] {
                        if (self && origin && self->m_session == origin) {
                            origin->mutate([ids, field](core::LibrarySession &worker) {
                                return worker.updateNotes(
                                    ids, [field](core::NoteMeta &meta) { (meta.*field).reset(); });
                            });
                        }
                    });
            } else if (!archive) {
                self->m_toast->showMessage(tr("Restored %n note(s).", nullptr, count));
            }
        });
}

core::Result<void> MainWindow::chooseCollections(const QStringList &ids)
{
    // Each collection shows whether all, some or none of the notes belong to it.
    QHash<QString, int> counts;
    for (const QString &id : ids) {
        if (const auto note = m_session->index().note(id); note && note->has_value()) {
            for (const QString &collection : (*note)->collections) {
                ++counts[collection];
            }
        }
    }
    QList<MembershipDialog::Entry> entries;
    for (const auto &[collection, depth] : core::collectionTree(m_session->collections())) {
        entries.append({.id = collection->id,
                        .label = collection->name,
                        .iconName = ui::collectionGlyph(collection->icon),
                        .color = collection->color,
                        .depth = depth,
                        .state = membership(counts.value(collection->id), ids.size())});
    }
    if (entries.isEmpty()) {
        m_toast->showMessage(tr("There are no collections yet. Create one in the sidebar."));
        return {};
    }
    MembershipDialog dialog(tr("Collections"), entries, {}, this);
    if (dialog.exec() != QDialog::Accepted) {
        return {};
    }
    m_session->mutate(
        [ids, add = dialog.added(), remove = dialog.removed()](core::LibrarySession &worker) {
            return worker.changeCollections(ids, add, remove);
        });
    return {};
}

core::Result<void> MainWindow::chooseTags(const QStringList &ids)
{
    QHash<QString, int> counts;
    QHash<QString, QString> spelling;
    for (const QString &id : ids) {
        auto note = m_session->index().note(id);
        if (!note) {
            return std::unexpected(note.error());
        }
        if (!*note) {
            continue;
        }
        for (const QString &tag : (*note)->tags) {
            ++counts[core::tagKey(tag)];
            spelling.insert(core::tagKey(tag), tag);
        }
    }
    for (const core::TagCount &tag : m_session->index().tags().value_or(QList<core::TagCount>())) {
        if (!spelling.contains(core::tagKey(tag.name))) {
            spelling.insert(core::tagKey(tag.name), tag.name);
        }
    }
    QStringList keys = spelling.keys();
    std::ranges::sort(keys);
    QList<MembershipDialog::Entry> entries;
    for (const QString &key : keys) {
        entries.append({.id = spelling.value(key),
                        .label = spelling.value(key),
                        .iconName = QStringLiteral("Tag"),
                        .color = std::nullopt,
                        .depth = 0,
                        .state = membership(counts.value(key), ids.size())});
    }
    MembershipDialog dialog(tr("Tags"), entries, tr("New tag"), this);
    if (dialog.exec() != QDialog::Accepted) {
        return {};
    }
    m_session->mutate(
        [ids, add = dialog.added(), remove = dialog.removed()](core::LibrarySession &worker) {
            return worker.changeTags(ids, add, remove);
        });
    return {};
}

void MainWindow::announceRules(const QList<core::RuleChange> &changes)
{
    // The message names the collections the rules filed the notes into, with Undo.
    QStringList names;
    for (const core::RuleChange &change : changes) {
        for (const QString &id : change.collectionsAfter) {
            const auto found =
                std::ranges::find(m_session->collections(), id, &core::Collection::id);
            if (!change.collectionsBefore.contains(id) && found != m_session->collections().end() &&
                !names.contains(found->name)) {
                names.append(found->name);
            }
        }
    }
    const QString into = names.join(QStringLiteral(", "));
    const QString text =
        changes.size() == 1
            ? tr("A rule filed “%1” into %2.")
                  .arg(changes.first().title.isEmpty() ? tr("Untitled") : changes.first().title,
                       into)
            : tr("Rules filed %n note(s) into %1.", nullptr, static_cast<int>(changes.size()))
                  .arg(into);
    core::LibrarySession *session = m_session;
    m_toast->showMessage(text, tr("Undo"), [this, session, changes] {
        if (m_session != session) {
            return;
        }
        // Typing since then goes to disk first, so reading the note back keeps it.
        if (!m_editor->saveNow()) {
            return;
        }
        session->mutate(
            [changes](core::LibrarySession &worker) { return worker.undoRules(changes); });
        m_editor->reload();
    });
}

void MainWindow::emptyTrash()
{
    if (!isWritable()) {
        return;
    }
    const auto trashed =
        m_session->index().notes({.view = core::NoteFilter::View::Trash, .ids = {}, .tag = {}}, {});
    if (!trashed || trashed->isEmpty()) {
        return;
    }
    QStringList ids;
    for (const core::NoteSummary &note : *trashed) {
        ids.append(note.id);
    }
    const int count = static_cast<int>(ids.size());
    if (!ui::confirmDestructive(this, tr("Empty the trash?"),
                                tr("%n note(s) in the trash will be deleted permanently with "
                                   "their attachments. This cannot be undone.",
                                   nullptr, count),
                                tr("Empty trash"))) {
        return;
    }
    m_session->mutate([ids](core::LibrarySession &worker) { return worker.deleteNotes(ids); });
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updateLayout();
}

void MainWindow::updateLayout()
{
    const int w = width();
    Layout layout = Layout::Wide;
    if (w < NarrowWidth) {
        layout = Layout::Narrow;
    } else if (w < MediumWidth) {
        layout = Layout::Medium;
    }
    if (layout != m_layout) {
        m_layout = layout;
        m_sidebarOpen = false;
        m_pane = m_list->currentNote().isEmpty() ? Pane::List : m_pane;
    }
    if (m_focusMode) {
        m_sidebar->hide();
        m_list->hide();
        m_editor->show();
        m_editor->setBackButtonVisible(false);
        return;
    }
    bool sidebarShown = false;
    switch (m_layout) {
    case Layout::Wide:
        sidebarShown = m_wideSidebar;
        m_list->show();
        m_editor->show();
        break;
    case Layout::Medium:
        sidebarShown = m_sidebarOpen;
        m_list->show();
        m_editor->show();
        break;
    case Layout::Narrow:
        sidebarShown = m_pane == Pane::Sidebar;
        m_list->setVisible(m_pane == Pane::List);
        m_editor->setVisible(m_pane == Pane::Note);
        break;
    }
    m_sidebar->setVisible(sidebarShown);
    m_list->setSidebarShown(sidebarShown);
    m_editor->setBackButtonVisible(m_layout == Layout::Narrow);
}

void MainWindow::setFocusMode(bool on)
{
    if (on == m_focusMode) {
        return;
    }
    // Focus mode needs a note to focus on.
    if (on && m_editor->noteId().isEmpty()) {
        return;
    }
    m_focusMode = on;
    m_editor->setFocusMode(on);
    if (m_layout == Layout::Narrow && on) {
        m_pane = Pane::Note;
    }
    updateLayout();
    m_editor->setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && m_focusMode) {
        setFocusMode(false);
        return;
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::showPane(Pane pane)
{
    m_pane = pane;
    updateLayout();
}

void MainWindow::focusNextPane(bool forward)
{
    QList<QWidget *> panes;
    for (QWidget *pane : {static_cast<QWidget *>(m_sidebar), static_cast<QWidget *>(m_list),
                          static_cast<QWidget *>(m_editor)}) {
        if (pane->isVisible()) {
            panes.append(pane);
        }
    }
    if (panes.isEmpty()) {
        return;
    }
    qsizetype current = 0;
    for (qsizetype i = 0; i < panes.size(); ++i) {
        if (panes.at(i)->isAncestorOf(focusWidget())) {
            current = i;
        }
    }
    const qsizetype next = (current + (forward ? 1 : panes.size() - 1)) % panes.size();
    panes.at(next)->setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::saveState()
{
    m_editor->saveNow();
    m_settings.setWindowGeometry(saveGeometry());
    // Pane widths are kept only from the full layout, so a hidden sidebar keeps its width.
    if (m_layout == Layout::Wide && m_wideSidebar) {
        m_settings.setSplitterState(m_splitter->saveState());
    }
    m_settings.setSidebarVisible(m_wideSidebar);
    m_settings.setExpandedItems(m_sidebar->expandedItems());
    m_settings.setLastView(m_viewRef.key());
    m_settings.setLastNote(m_noteId);
}

bool MainWindow::savePending()
{
    if (m_session != nullptr && m_session->isMutating()) {
        m_toast->showMessage(tr("Wait for the library update to finish."));
        return false;
    }
    return m_editor->saveNow();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!savePending()) {
        event->ignore();
        return;
    }
    saveState();
    QMainWindow::closeEvent(event);
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        refreshList();
    }
    QMainWindow::changeEvent(event);
}
