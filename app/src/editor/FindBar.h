#pragma once

#include "core/Search.h"

#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QWidget>

#include <memory>

class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;

namespace editor {

class TextEdit;

/// Find and replace in the open note: `Ctrl+F` finds, `Ctrl+H` also
/// replaces. Every match is highlighted without changing the document, the current one is
/// selected, and the count shows where it is. `Enter` and `F3` go to the next match,
/// `Shift+Enter` and `Shift+F3` to the previous one, `Escape` closes the bar.
///
/// While the bar is closed, it highlights the words of the note list's search instead, in every
/// word form, and `F3` and `Shift+F3` step through them.
class FindBar : public QWidget
{
    Q_OBJECT

public:
    /// Creates the bar for @p edit; it starts hidden.
    FindBar(TextEdit *edit, QWidget *parent = nullptr);

    /// Shows the bar with the find field focused, prefilled with the selected text.
    ///
    /// @param replace Whether the replace field shows too.
    void open(bool replace);

    /// Hides the bar and its highlights and returns the focus to the text.
    void dismiss();

    /// Selects the next (@p forward) or previous match, wrapping around the note.
    void step(bool forward);

    /// Hides the replace field while the note is read-only.
    void setReadOnly(bool readOnly);

    /// Finds again in the note's current text, after it was replaced or edited.
    void refresh();

    /// Sets the terms of the note list's search, highlighted while the bar is closed.
    ///
    /// @param terms The query's terms; empty removes the highlights.
    void setSearchTerms(const QList<core::SearchTerm> &terms);

    /// Tells whether the words of a search are highlighted while the bar is closed.
    bool hasSearchTerms() const { return m_terms != nullptr; }

    bool eventFilter(QObject *watched, QEvent *event) override;

protected:
    void changeEvent(QEvent *event) override;

private:
    void retranslate();
    void replaceOne();
    void replaceAll();
    /// Highlights the matches and updates the count; with @p select, selects the first match
    /// from the cursor on.
    void search(bool select);
    /// Lists the matches: of the find field while the bar is open, otherwise of the search
    /// terms.
    QList<QTextCursor> matches() const;

    TextEdit *m_edit;
    QLineEdit *m_find;
    QLabel *m_count;
    QToolButton *m_previous;
    QToolButton *m_next;
    QToolButton *m_close;
    QWidget *m_replaceRow;
    QLineEdit *m_replace;
    QPushButton *m_replaceOne;
    QPushButton *m_replaceAll;
    /// Finds again shortly after the note changes while the bar is open.
    QTimer m_refresh;
    bool m_readOnly = false;
    bool m_replaceShown = false;
    QList<core::SearchTerm> m_termList;
    std::unique_ptr<core::TermMatcher> m_terms;
};

} // namespace editor
