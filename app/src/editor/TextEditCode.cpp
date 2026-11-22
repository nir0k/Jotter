#include "editor/Commands.h"
#include "editor/Formats.h"
#include "editor/LanguagePopup.h"
#include "editor/Objects.h"
#include "editor/Presenter.h"
#include "editor/Syntax.h"
#include "editor/TextEdit.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QLineEdit>
#include <QPainter>
#include <QScrollBar>
#include <QTextFrame>

namespace editor {

QList<CodeHeader> TextEdit::codeHeaders() const
{
    return editor::codeHeaders(
        *document(), QPointF(-horizontalScrollBar()->value(), -verticalScrollBar()->value()),
        viewport()->height(), syntaxRepository());
}

std::optional<CodeHeader> TextEdit::codeHeaderAt(const QPoint &viewportPosition) const
{
    for (const CodeHeader &header : codeHeaders()) {
        if (header.box.contains(viewportPosition)) {
            return header;
        }
    }
    return std::nullopt;
}

void TextEdit::paintCodeHeaders(QPainter &painter, const QList<CodeHeader> &headers) const
{
    const ui::Theme &theme = *ui::Theme::current();
    const ui::Roles &r = theme.roles();
    const QTextFrame *cursorFrame = hasFocus() ? codeBlockOf(textCursor().block()) : nullptr;
    const int icon = theme.px(1.0);
    const int chevron = theme.px(0.75);
    for (const CodeHeader &header : headers) {
        // The title field covers the title while it is edited.
        const bool editing = header.frame == m_titleFrame;
        paintCodeLabels(painter, header, r, !editing);
        const bool active = header.frame == m_hoverCode || header.frame == cursorFrame;
        if (!editing && active && !isReadOnly() &&
            header.frame->frameFormat().stringProperty(CodeTitle).isEmpty()) {
            painter.setFont(theme.font(ui::TextRole::Label));
            painter.setPen(ui::mix(r.onSurfaceVariant, 0.6F, r.surfaceContainerHighest));
            painter.drawText(header.title, Qt::AlignLeft | Qt::AlignVCenter, tr("Title"));
        }
        if (!isReadOnly()) {
            ui::icon(QStringLiteral("ChevronDown"))
                .paint(&painter,
                       QRect(header.language.right() - chevron,
                             header.language.center().y() - (chevron / 2), chevron, chevron));
        }
        ui::icon(QStringLiteral("Copy"),
                 header.frame == m_hoverCode ? ui::IconTone::Primary : ui::IconTone::Idle)
            .paint(&painter, QRect(header.copy.center().x() - (icon / 2),
                                   header.copy.center().y() - (icon / 2), icon, icon));
    }
}

bool TextEdit::clickCodeHeader(const QPoint &viewportPosition)
{
    const auto header = codeHeaderAt(viewportPosition);
    if (!header) {
        return false;
    }
    if (header->copy.contains(viewportPosition)) {
        copyCode(*header->frame);
        return true;
    }
    if (isReadOnly()) {
        return false;
    }
    if (header->language.contains(viewportPosition)) {
        chooseLanguage(header->frame, viewport()->mapToGlobal(header->language.bottomLeft()));
        return true;
    }
    if (header->title.contains(viewportPosition)) {
        editTitle(header->frame);
        return true;
    }
    return false;
}

void TextEdit::copyCode(const QTextFrame &frame)
{
    QGuiApplication::clipboard()->setText(codeText(frame));
    Q_EMIT message(tr("Code copied"));
}

void TextEdit::chooseLanguage(QTextFrame *frame, const QPoint &globalPosition)
{
    if (frame == nullptr || isReadOnly()) {
        return;
    }
    m_languageFrame = frame;
    m_languages->open(globalPosition, frame->frameFormat().stringProperty(CodeLanguage));
}

void TextEdit::editTitle(QTextFrame *frame)
{
    if (frame == nullptr || isReadOnly()) {
        return;
    }
    const auto headers = codeHeaders();
    const auto it = std::ranges::find(headers, frame, &CodeHeader::frame);
    if (it == headers.end()) {
        ensureCursorVisible();
        return;
    }
    m_titleFrame = frame;
    m_titleEdit->setFont(ui::Theme::current()->font(ui::TextRole::Label));
    m_titleEdit->setPlaceholderText(tr("Title"));
    m_titleEdit->setText(frame->frameFormat().stringProperty(CodeTitle));
    m_titleEdit->setGeometry(it->title);
    m_titleEdit->show();
    m_titleEdit->setFocus();
    m_titleEdit->selectAll();
    viewport()->update();
}

void TextEdit::finishTitle(bool keep)
{
    if (!m_titleEdit->isVisible()) {
        return;
    }
    const QPointer<QTextFrame> frame = m_titleFrame;
    m_titleFrame = nullptr;
    m_titleEdit->hide();
    if (frame == nullptr) {
        return;
    }
    const QString title = m_titleEdit->text().simplified();
    if (keep && title != frame->frameFormat().stringProperty(CodeTitle)) {
        setCodeBlockProperty(*frame, CodeTitle, title);
    }
    setTextCursor(frame->firstCursorPosition());
    setFocus();
    viewport()->update();
}

} // namespace editor
