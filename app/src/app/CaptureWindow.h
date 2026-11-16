#pragma once

#include <QDialog>

namespace core {
class LibrarySession;
}
namespace editor {
class NoteEditor;
}

/// The new note window: a small window with the title field and the editor
/// with a compact toolbar. It asks for the dialog role, so tiling compositors float it. The note
/// is created when the window opens and saved like any note; left empty, it is discarded when
/// the window closes. `Ctrl+Enter` and `Escape` close the window.
class CaptureWindow : public QDialog
{
    Q_OBJECT

public:
    /// @param session The open library the note goes to; it must outlive the window.
    explicit CaptureWindow(core::LibrarySession &session, QWidget *parent = nullptr);

    /// Creates a new note and focuses its title.
    ///
    /// @return Whether the note could be created; the reason is shown in the window otherwise.
    bool start();

    /// The note the window edits, or empty before start().
    QString noteId() const;

    /// Saves pending typing before a library switch, leaving this window open on failure.
    bool savePending();

    /// Saves the note, or discards it when it is still empty, before the window closes.
    void done(int result) override;

protected:
    void changeEvent(QEvent *event) override;
    /// Keeps the window open if its note cannot be saved.
    void closeEvent(QCloseEvent *event) override;

private:
    core::LibrarySession &m_session;
    editor::NoteEditor *m_editor;
};
