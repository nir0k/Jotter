#include "editor/FindBar.h"

#include "editor/TextEdit.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextBlock>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>

#include <ranges>

namespace editor {

namespace {

/// Highlights beyond this many matches are not drawn; the count still includes them.
constexpr int MaxHighlights = 1000;

} // namespace

FindBar::FindBar(TextEdit *edit, QWidget *parent)
    : QWidget(parent)
    , m_edit(edit)
    , m_find(new QLineEdit(this))
    , m_count(new QLabel(this))
    , m_previous(new QToolButton(this))
    , m_next(new QToolButton(this))
    , m_close(new QToolButton(this))
    , m_replaceRow(new QWidget(this))
    , m_replace(new QLineEdit(m_replaceRow))
    , m_replaceOne(new QPushButton(m_replaceRow))
    , m_replaceAll(new QPushButton(m_replaceRow))
{
    setProperty("findBar", true);
    setAttribute(Qt::WA_StyledBackground);
    const ui::Theme &theme = *ui::Theme::current();
    m_find->addAction(ui::icon(QStringLiteral("Search")), QLineEdit::LeadingPosition);
    m_find->setClearButtonEnabled(true);
    ui::setTextRole(m_count, ui::TextRole::Hint);
    m_previous->setIcon(ui::icon(QStringLiteral("ChevronUp")));
    m_next->setIcon(ui::icon(QStringLiteral("ChevronDown")));
    m_close->setIcon(ui::icon(QStringLiteral("Close")));

    const int pad = theme.px(1.5);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(pad, theme.px(0.4), pad, theme.px(0.4));
    layout->setSpacing(theme.px(0.4));
    auto *findRow = new QHBoxLayout;
    findRow->setContentsMargins(0, 0, 0, 0);
    findRow->addWidget(m_find, 1);
    findRow->addWidget(m_count);
    findRow->addWidget(m_previous);
    findRow->addWidget(m_next);
    findRow->addWidget(m_close);
    layout->addLayout(findRow);
    auto *replaceRow = new QHBoxLayout(m_replaceRow);
    replaceRow->setContentsMargins(0, 0, 0, 0);
    replaceRow->addWidget(m_replace, 1);
    replaceRow->addWidget(m_replaceOne);
    replaceRow->addWidget(m_replaceAll);
    layout->addWidget(m_replaceRow);

    m_find->installEventFilter(this);
    m_replace->installEventFilter(this);
    connect(m_find, &QLineEdit::textEdited, this, [this] { search(true); });
    connect(m_previous, &QToolButton::clicked, this, [this] { step(false); });
    connect(m_next, &QToolButton::clicked, this, [this] { step(true); });
    connect(m_close, &QToolButton::clicked, this, &FindBar::dismiss);
    connect(m_replaceOne, &QPushButton::clicked, this, &FindBar::replaceOne);
    connect(m_replaceAll, &QPushButton::clicked, this, &FindBar::replaceAll);
    m_refresh.setSingleShot(true);
    m_refresh.setInterval(150);
    connect(&m_refresh, &QTimer::timeout, this, &FindBar::refresh);
    connect(m_edit, &QTextEdit::textChanged, this, [this] {
        if (!isHidden() || m_terms) {
            m_refresh.start();
        }
    });
    retranslate();
    hide();
}

void FindBar::open(bool replace)
{
    m_replaceShown = replace;
    m_replaceRow->setVisible(replace && !m_readOnly);
    show();
    // A selection on one line is what the person wants to find.
    const QString selected = m_edit->textCursor().selectedText();
    if (!selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator)) {
        m_find->setText(selected);
    }
    m_find->setFocus();
    m_find->selectAll();
    search(false);
}

void FindBar::dismiss()
{
    hide();
    // The words of the note list's search are highlighted again, or nothing.
    search(false);
    m_edit->setFocus();
}

void FindBar::setReadOnly(bool readOnly)
{
    m_readOnly = readOnly;
    m_replaceRow->setVisible(m_replaceShown && !readOnly);
}

void FindBar::refresh()
{
    if (!isHidden() || m_terms) {
        search(false);
    }
}

void FindBar::setSearchTerms(const QList<core::SearchTerm> &terms)
{
    if (terms == m_termList) {
        return;
    }
    m_termList = terms;
    m_terms = std::make_unique<core::TermMatcher>(terms);
    if (m_terms->isEmpty()) {
        m_terms.reset();
    }
    if (isHidden()) {
        search(false);
    }
}

bool FindBar::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        const bool shift = (key->modifiers() & Qt::ShiftModifier) != 0;
        if (key->key() == Qt::Key_Escape) {
            dismiss();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (watched == m_replace) {
                replaceOne();
            } else {
                step(!shift);
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FindBar::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QWidget::changeEvent(event);
}

void FindBar::retranslate()
{
    m_find->setPlaceholderText(tr("Find in note"));
    m_replace->setPlaceholderText(tr("Replace with"));
    for (const auto &[button, text] :
         {std::pair{m_previous, tr("Previous match (Shift+F3)")},
          std::pair{m_next, tr("Next match (F3)")}, std::pair{m_close, tr("Close (Escape)")}}) {
        button->setToolTip(text);
        button->setAccessibleName(text);
    }
    m_replaceOne->setText(tr("Replace"));
    m_replaceAll->setText(tr("Replace all"));
    refresh();
}

void FindBar::step(bool forward)
{
    const QList<QTextCursor> found = matches();
    if (found.isEmpty()) {
        return;
    }
    // The next match after the selection, or the previous one before it, wrapping around.
    const QTextCursor cursor = m_edit->textCursor();
    QTextCursor target = forward ? found.first() : found.last();
    if (forward) {
        const auto next = std::ranges::find_if(found, [&](const QTextCursor &match) {
            return match.selectionStart() >= cursor.selectionEnd();
        });
        if (next != found.end()) {
            target = *next;
        }
    } else {
        const auto previous =
            std::ranges::find_if(found | std::views::reverse, [&](const QTextCursor &match) {
                return match.selectionEnd() <= cursor.selectionStart();
            });
        if (previous != (found | std::views::reverse).end()) {
            target = *previous;
        }
    }
    m_edit->setTextCursor(target);
    search(false);
}

QList<QTextCursor> FindBar::matches() const
{
    QList<QTextCursor> found;
    const QTextDocument *document = m_edit->document();
    if (!isHidden()) {
        const QString text = m_find->text();
        if (text.isEmpty()) {
            return found;
        }
        for (QTextCursor match = document->find(text, 0); !match.isNull();
             match = document->find(text, match)) {
            found.append(match);
        }
        return found;
    }
    if (m_terms) {
        for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
            for (const core::TextSpan &span : m_terms->find(block.text())) {
                QTextCursor match(m_edit->document());
                match.setPosition(block.position() + static_cast<int>(span.start));
                match.setPosition(block.position() + static_cast<int>(span.start + span.length),
                                  QTextCursor::KeepAnchor);
                found.append(match);
            }
        }
    }
    return found;
}

void FindBar::search(bool select)
{
    const QList<QTextCursor> found = matches();
    if (select && !found.isEmpty()) {
        const int from = m_edit->textCursor().selectionStart();
        const auto first = std::ranges::find_if(
            found, [from](const QTextCursor &match) { return match.selectionStart() >= from; });
        m_edit->setTextCursor(first != found.end() ? *first : found.first());
    }
    const ui::Roles &r = ui::Theme::current()->roles();
    QTextCharFormat highlight;
    highlight.setBackground(ui::mix(r.primary, 0.25F, r.surface));
    const QTextCursor current = m_edit->textCursor();
    QList<QTextEdit::ExtraSelection> selections;
    int index = 0;
    for (qsizetype i = 0; i < found.size(); ++i) {
        const QTextCursor &match = found.at(i);
        if (match.selectionStart() == current.selectionStart() &&
            match.selectionEnd() == current.selectionEnd()) {
            index = static_cast<int>(i) + 1;
        }
        if (selections.size() < MaxHighlights) {
            selections.append({.cursor = match, .format = highlight});
        }
    }
    m_edit->setExtraSelections(selections);
    const int total = static_cast<int>(found.size());
    if (m_find->text().isEmpty()) {
        m_count->clear();
    } else if (total == 0) {
        m_count->setText(tr("No matches"));
    } else if (index > 0) {
        m_count->setText(tr("%1 of %2").arg(index).arg(total));
    } else {
        m_count->setText(tr("%n match(es)", nullptr, total));
    }
    const bool any = total > 0;
    m_previous->setEnabled(any);
    m_next->setEnabled(any);
    m_replaceOne->setEnabled(any);
    m_replaceAll->setEnabled(any);
}

void FindBar::replaceOne()
{
    const QString text = m_find->text();
    if (m_readOnly || text.isEmpty()) {
        return;
    }
    QTextCursor cursor = m_edit->textCursor();
    if (cursor.selectedText().compare(text, Qt::CaseInsensitive) == 0) {
        cursor.insertText(m_replace->text());
        m_edit->setTextCursor(cursor);
    }
    step(true);
}

void FindBar::replaceAll()
{
    const QString text = m_find->text();
    if (m_readOnly || text.isEmpty()) {
        return;
    }
    QTextDocument *document = m_edit->document();
    QTextCursor edit(document);
    int replaced = 0;
    // One undo step for all replacements.
    edit.beginEditBlock();
    for (QTextCursor found = document->find(text, 0); !found.isNull();
         found = document->find(text, found)) {
        found.insertText(m_replace->text());
        ++replaced;
    }
    edit.endEditBlock();
    search(false);
    m_count->setText(tr("%n replaced", nullptr, replaced));
}

} // namespace editor
