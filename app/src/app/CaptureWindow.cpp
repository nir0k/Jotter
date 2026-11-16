#include "app/CaptureWindow.h"

#include "core/LibrarySession.h"
#include "editor/NoteEditor.h"
#include "ui/Theme.h"

#include <QCloseEvent>
#include <QEvent>
#include <QMessageBox>
#include <QShortcut>
#include <QVBoxLayout>

CaptureWindow::CaptureWindow(core::LibrarySession &session, QWidget *parent)
    : QDialog(parent)
    , m_session(session)
    , m_editor(new editor::NoteEditor(this))
{
    const ui::Theme &theme = *ui::Theme::current();
    setProperty("pane", QStringLiteral("editor"));
    m_editor->setCompact();
    m_editor->setSession(&session);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_editor);
    resize(theme.px(32), theme.px(24));
    // Ctrl+Enter closes the window wherever the focus is, also in a checklist or a code block,
    // where the editor would otherwise use it.
    auto *close = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), this);
    close->setContext(Qt::WindowShortcut);
    connect(close, &QShortcut::activated, this, &QDialog::accept);
    auto *closeEnter = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Enter")), this);
    closeEnter->setContext(Qt::WindowShortcut);
    connect(closeEnter, &QShortcut::activated, this, &QDialog::accept);
    setWindowTitle(tr("New note"));
}

bool CaptureWindow::start()
{
    core::Note note = core::makeNote();
    if (auto saved = m_session.saveNote(note); !saved) {
        QMessageBox::warning(this, tr("New note"), core::describe(saved.error()));
        return false;
    }
    m_editor->openNote(note.meta.id);
    m_editor->markNew();
    m_editor->focusTitle();
    return true;
}

QString CaptureWindow::noteId() const
{
    return m_editor->noteId();
}

void CaptureWindow::done(int result)
{
    if (m_editor->closeNote()) {
        QDialog::done(result);
    }
}

bool CaptureWindow::savePending()
{
    return m_editor->saveNow();
}

void CaptureWindow::closeEvent(QCloseEvent *event)
{
    if (!savePending()) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void CaptureWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        setWindowTitle(tr("New note"));
    }
    QDialog::changeEvent(event);
}
