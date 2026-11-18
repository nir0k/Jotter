#pragma once

#include "app/View.h"
#include "core/Error.h"
#include "editor/NoteAction.h"

#include <QMainWindow>
#include <QSet>

namespace core {
class LibrarySession;
struct RuleChange;
struct SyncReport;
} // namespace core
class Locator;
class NoteList;
namespace editor {
class NoteEditor;
}
class Settings;
class Sidebar;
struct SyncStatus;
class QSplitter;
namespace ui {
class Toast;
}

/// The main window: the sidebar, the note list and the editor pane side by
/// side, collapsing for narrow tiled windows.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(Settings &settings, QWidget *parent = nullptr);

    /// Shows the library of @p session, restoring the last view and note.
    bool setSession(core::LibrarySession *session);

    /// Saves all pending edits; a failure keeps the window and session open.
    bool savePending();

    /// Asks for the position of a note just created, when the device preference adds geotags;
    /// the geotag is saved when the position comes.
    void locateNewNote(const QString &noteId);

    /// Offers Add location in the editor while geotags are on.
    void setLocationsEnabled(bool enabled);

    /// Shows the state of synchronisation in the sidebar's footer; an empty status hides it.
    void setSyncStatus(const SyncStatus &status, const QString &toolTip);

    /// Tells about the conflict copies a sync run made, offering to open
    /// the first conflicting note.
    void announceConflicts(const core::SyncReport &report);

Q_SIGNALS:
    void settingsRequested();
    /// Emitted on Show in the notice about note files that cannot be read, to list them in
    /// Settings → Library.
    void problemsRequested();
    /// Emitted on Sync now (`F5`) and on the sidebar's sync status.
    void syncRequested();

protected:
    void closeEvent(QCloseEvent *event) override;
    /// Leaves focus mode on an Escape no other control took.
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    /// How the panes fit the window width.
    enum class Layout { Wide, Medium, Narrow };
    /// The single pane shown in the narrow layout.
    enum class Pane { Sidebar, List, Note };

    void showView(const View &view);
    /// Fills the list for the current view; safe to call while a refresh is running.
    void refreshList();
    /// Lists the notes of the current view at once, or starts its search on a worker thread,
    /// so a search through many notes never blocks the window.
    void fillList();
    /// Runs a search on a worker thread with an index of its own and shows its notes when it
    /// ends, unless the list was filled again meanwhile: then the list is filled anew.
    void startSearch(const View &listed, const core::NoteFilter &filter, const core::Search &search,
                     quint64 generation);
    /// Shows the notes of a view, or of a search in it, and the editor's highlights of the
    /// search's words.
    void showNotes(const View &listed, core::Result<QList<core::NoteSummary>> notes,
                   const core::Search &search);
    /// Opens a note in the editor pane without changing which pane a narrow window shows.
    void openNote(const QString &id);
    /// Selects and opens a note, switching to a view that holds it when the current one does
    /// not: the trash for a trashed note, the archive for an archived one, else All notes.
    void revealNote(const QString &id);
    /// Opens the target of a note link, remembering the note it was followed from for
    /// Alt+Left.
    void followLink(const QString &id);
    /// Returns to the note a link was followed from.
    void goBack();
    void updateLayout();
    /// Enters or leaves focus mode: only the editor shows.
    void setFocusMode(bool on);
    void showPane(Pane pane);
    void saveState();
    void focusNextPane(bool forward);
    /// Creates a note in the current collection or with the current tag, and opens it with the
    /// title focused.
    void createNote();
    /// Carries out an action of the editor or the list on notes, with Undo where it can be
    /// undone and a confirmation where it cannot.
    void perform(editor::NoteAction action, const QStringList &ids);
    /// Asks for the collections of notes and applies the choice.
    core::Result<void> chooseCollections(const QStringList &ids);
    /// Asks for the tags of notes and applies the choice.
    core::Result<void> chooseTags(const QStringList &ids);
    /// Tells which notes rules filed and where, with Undo.
    void announceRules(const QList<core::RuleChange> &changes);
    /// Deletes every note in the trash after a confirmation.
    void emptyTrash();
    /// Tells whether notes can be created and changed now.
    bool isWritable() const;
    /// Tells about note files the index could not read when files appear among them that were
    /// not announced yet, offering to list them.
    void announceProblems();

    Settings &m_settings;
    core::LibrarySession *m_session = nullptr;
    QSplitter *m_splitter;
    Sidebar *m_sidebar;
    NoteList *m_list;
    editor::NoteEditor *m_editor;
    ui::Toast *m_toast;
    Locator *m_locator;
    /// The notes links were followed from, the latest last.
    QStringList m_history;
    /// The unreadable note files already announced for the open library.
    QSet<QString> m_announcedProblems;
    View m_viewRef;
    QString m_noteId;
    Layout m_layout = Layout::Wide;
    Pane m_pane = Pane::List;
    /// Whether the sidebar is shown in the wide layout; the person's choice, kept between runs.
    bool m_wideSidebar = true;
    /// Whether the sidebar was opened in the medium layout, where it starts hidden.
    bool m_sidebarOpen = false;
    bool m_focusMode = false;
    bool m_refreshing = false;
    bool m_refreshPending = false;
    /// Counts the fills of the list; a search that ends after a newer fill is not shown.
    quint64 m_listGeneration = 0;
    /// Whether a search runs on a worker thread; a newer one starts once it ends.
    bool m_searchRunning = false;
};
