#pragma once

#include "core/Note.h"
#include "core/Search.h"
#include "editor/NoteAction.h"

#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QWidget>

#include <array>
#include <memory>
#include <optional>

namespace core {
class LibrarySession;
}
class QAction;
class QLineEdit;
class QMenu;
class QStackedWidget;
class QToolBar;
class QToolButton;
namespace ui {
class EmptyState;
class IconPicker;
class Notice;
class TableSizePopup;
} // namespace ui

namespace editor {

struct NoteBuffer;
class FindBar;
class InfoPanel;
class NotePicker;
class Presenter;
class TagStrip;
class TextEdit;

/// The editor pane: the title field, the tags, the body and the
/// formatting toolbar, saved automatically.
///
/// A note is saved one second after typing stops, when another note is opened and when the
/// window closes. A failed save keeps the text and shows the error over the editor. Archived
/// and trashed notes, and notes of a read-only or unreachable library, open read-only.
class NoteEditor : public QWidget
{
    Q_OBJECT

public:
    explicit NoteEditor(QWidget *parent = nullptr);
    ~NoteEditor() override;

    /// Sets the library the notes come from; nullptr closes the note after saving it.
    bool setSession(core::LibrarySession *session);

    /// Saves the open note, then opens another one.
    /// @return Whether navigation was allowed; a failed save retains the current note.
    bool openNote(const QString &id);

    /// Saves and closes the open note, showing the empty state.
    /// @return Whether the view closed; a failed save leaves it attached.
    bool closeNote();

    /// Writes the open note when it has unsaved changes.
    /// @return Whether edits are durable or there were none; false keeps the buffer open.
    bool saveNow();

    /// The identifier of the open note, or empty.
    QString noteId() const;

    /// Moves the keyboard focus to the title, for a new note.
    void focusTitle();

    /// Reads the open note again from its file, without saving: after an action such as
    /// archiving changed it on disk.
    void reload();

    /// Marks the open note as just created: left empty (no title, text or icon), it is
    /// deleted when another note opens.
    void markNew();

    /// Reacts to the library changing on disk: reloads the open note when another program
    /// changed it, keeping local edits as a conflict copy.
    void checkOutsideChanges();

    /// Shows or hides the Back button of the stacked narrow layout.
    void setBackButtonVisible(bool visible);

    /// Turns focus mode on or off: the text keeps the pane's width and
    /// margins, and the focus button offers to leave.
    void setFocusMode(bool on);

    /// Makes this the editor of the new note window: without the icon, the
    /// focus button, the ⋮ menu and the info panel, and with a toolbar of the common formatting.
    void setCompact();

    /// Offers Add location in the ⋮ menu while the device preference asks for geotags.
    void setLocationsEnabled(bool enabled);

    /// Records that the position for a new note could not be found, which its info then says.
    void setLocationUnavailable(const QString &noteId);

    /// Highlights the words of the note list's search in every note opened;
    /// `F3` and `Shift+F3` step through them while find in note is closed.
    ///
    /// @param terms The query's terms; empty removes the highlights.
    void setSearchTerms(const QList<core::SearchTerm> &terms);

    /// Opens the toolbar's overflow menu upwards, as the toolbar runs along the bottom.
    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    void backRequested();
    /// Emitted when the person asks to open another note, such as a conflict copy.
    void openNoteRequested(const QString &id);
    /// Emitted when the open note's title changes, for the window title.
    void titleChanged(const QString &title);
    /// Emitted when the person chooses an action of the ⋮ menu or of a read-only notice.
    void actionRequested(editor::NoteAction action, const QStringList &ids);
    /// Emitted with a confirmation to show as a transient message, such as copied code.
    void message(const QString &text);
    /// Emitted by the focus button, to enter or leave focus mode.
    void focusModeRequested();
    /// Emitted when the open note closes and the pane shows no note, such as when the list no
    /// longer holds it or it was deleted outside Jotter.
    void noteClosed();
    /// Emitted when a link to a note or a backlink is activated.
    void noteLinkActivated(const QString &noteId);
    /// Emitted by Add location in the ⋮ menu.
    void locationRequested(const QString &noteId);

protected:
    void changeEvent(QEvent *event) override;

private:
    void buildToolbar();
    void retranslate();
    void load(const core::Note &note);
    void updateReadOnly();
    void updateToolbar();
    void scheduleSave();
    bool isModified() const;
    void editLink();
    /// Writes @p note with the current title and text; a failure keeps everything here.
    bool writeNote(core::Note note);
    /// Changes the open note's metadata and saves at once (icon and colour are choices).
    void changeMeta(const std::function<void(core::NoteMeta &)> &change);
    /// Deletes the open note when it was just created and is still empty.
    void discardEmptyNew();
    void updateHeader();
    void showMoreMenu();
    /// Fills the numbered list's style menu: the numbering styles, Restart numbering and
    /// Start at….
    void fillNumberingMenu();
    /// Asks for files and inserts them as images or attachments.
    void chooseFiles(bool images);
    /// Shows or hides the info panel (Ctrl+Shift+I).
    void setInfoVisible(bool visible);
    /// Brings the info panel up to date with the open note.
    void updateInfo();
    /// Looks up the notes the text links to, so the tooltips and the links to missing notes
    /// follow the library.
    void updateLinkedNotes();
    /// Replaces the `[[` before the cursor with a link to a note.
    void insertNoteLink(const QString &noteId, const QString &title);

    /// Binds a view to a shared document without changing its contents or undo history.
    void attachBuffer(std::shared_ptr<NoteBuffer> buffer);
    /// Refreshes every view of the buffer after a metadata or title change.
    void refreshViews();
    QPointer<core::LibrarySession> m_session;
    std::shared_ptr<NoteBuffer> m_buffer;
    bool m_readOnly = false;
    bool m_focusMode = false;

    QToolButton *m_back;
    QStackedWidget *m_stack;
    ui::EmptyState *m_empty;
    QWidget *m_page;
    QWidget *m_header;
    QToolButton *m_iconButton;
    QLineEdit *m_title;
    QToolButton *m_focus;
    QToolButton *m_more;
    TagStrip *m_tags;
    ui::IconPicker *m_picker;
    /// The bottom strip that holds the formatting toolbar.
    QWidget *m_formatBar;
    QToolBar *m_toolbar;
    ui::Notice *m_notice;
    TextEdit *m_text;
    Presenter *m_presenter = nullptr;
    FindBar *m_find;
    InfoPanel *m_info;
    NotePicker *m_notePicker;
    bool m_locationsEnabled = false;
    /// Notes whose position was asked for and not found.
    QSet<QString> m_locationUnavailable;
    ui::TableSizePopup *m_tableSize;
    /// Counts the words again shortly after typing, while the info panel is open.
    QTimer m_infoTimer;

    QToolButton *m_blockStyle = nullptr;
    /// The paragraph styles of the menu, in the order of BlockStyle; Quote has its own button.
    std::array<QAction *, 4> m_styles{};
    QAction *m_quote = nullptr;
    QAction *m_bold = nullptr;
    QAction *m_italic = nullptr;
    QAction *m_underline = nullptr;
    QAction *m_strike = nullptr;
    QAction *m_superscript = nullptr;
    QAction *m_subscript = nullptr;
    QAction *m_inlineCode = nullptr;
    QToolButton *m_textColor = nullptr;
    QToolButton *m_highlight = nullptr;
    QAction *m_link = nullptr;
    QAction *m_bulleted = nullptr;
    QAction *m_numbered = nullptr;
    QAction *m_checklist = nullptr;
    QAction *m_outdent = nullptr;
    QAction *m_indent = nullptr;
    QAction *m_divider = nullptr;
    QAction *m_codeBlock = nullptr;
    QToolButton *m_numberedButton = nullptr;
    QMenu *m_numberingMenu = nullptr;
    QAction *m_table = nullptr;
    QAction *m_image = nullptr;
    QAction *m_attachment = nullptr;
    QAction *m_findAction = nullptr;
    QAction *m_replaceAction = nullptr;
    QAction *m_findNext = nullptr;
    QAction *m_findPrevious = nullptr;
    QAction *m_infoAction = nullptr;
};

} // namespace editor
