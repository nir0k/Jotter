#include "editor/Decorations.h"

#include "editor/Commands.h"
#include "editor/Formats.h"
#include "ui/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextTable>

#include <cmath>

namespace editor {

QList<TableGeometry> tableGeometries(const QTextDocument &document, QPointF offset, qreal bottom)
{
    QList<TableGeometry> tables;
    const QAbstractTextDocumentLayout *layout = document.documentLayout();
    for (QTextFrame *frame : document.rootFrame()->childFrames()) {
        auto *table = qobject_cast<QTextTable *>(frame);
        if (table == nullptr) {
            continue;
        }
        const QRectF bounds = layout->frameBoundingRect(frame).translated(offset);
        if (bounds.bottom() < 0) {
            continue;
        }
        if (bounds.top() > bottom) {
            break;
        }
        const QTextTableFormat format = table->format();
        const qreal padding = format.cellPadding();
        // A cell starts its padding before its first block. The table's place comes from its
        // first cell: Qt's bounding rectangle of a table is offset by the padding, though its
        // size, margins included, is right.
        const auto cellCorner = [&](int row, int column) {
            const QTextBlock first = table->cellAt(row, column).firstCursorPosition().block();
            return layout->blockBoundingRect(first).translated(offset).topLeft() -
                   QPointF(padding, padding);
        };
        TableGeometry geometry{
            .table = table,
            .box = QRectF(cellCorner(0, 0),
                          QSizeF(bounds.width() - format.leftMargin() - format.rightMargin(),
                                 bounds.height() - format.topMargin() - format.bottomMargin())),
            .columnLines = {},
            .rowLines = {}};
        for (int column = 1; column < table->columns(); ++column) {
            geometry.columnLines.append(std::round(cellCorner(0, column).x()));
        }
        for (int row = 1; row < table->rows(); ++row) {
            geometry.rowLines.append(std::round(cellCorner(row, 0).y()));
        }
        tables.append(geometry);
    }
    return tables;
}

void paintTables(QPainter &painter, const QList<TableGeometry> &tables, const ui::Roles &roles)
{
    painter.save();
    for (const TableGeometry &geometry : tables) {
        const QRectF &box = geometry.box;
        const int header =
            std::min(geometry.table->format().headerRowCount(), geometry.table->rows());
        if (header > 0) {
            const qreal bottom = header <= geometry.rowLines.size()
                                     ? geometry.rowLines.at(header - 1)
                                     : box.bottom();
            painter.fillRect(QRectF(box.left(), box.top(), box.width(), bottom - box.top()),
                             roles.surfaceContainerHighest);
        }
        painter.setPen(QPen(roles.outline, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box.adjusted(0.5, 0.5, -0.5, -0.5));
        for (const qreal x : geometry.columnLines) {
            painter.drawLine(QPointF(x + 0.5, box.top()), QPointF(x + 0.5, box.bottom()));
        }
        for (const qreal y : geometry.rowLines) {
            painter.drawLine(QPointF(box.left(), y + 0.5), QPointF(box.right(), y + 0.5));
        }
    }
    painter.restore();
}

QList<CodeHeader> codeHeaders(const QTextDocument &document, QPointF offset, qreal bottom,
                              const KSyntaxHighlighting::Repository &repository)
{
    QList<CodeHeader> headers;
    const ui::Theme &theme = *ui::Theme::current();
    const QAbstractTextDocumentLayout *layout = document.documentLayout();
    const int pad = theme.px(0.75);
    const int button = theme.px(1.5);
    const int height = codeHeaderHeight();
    const QFontMetrics hint(theme.font(ui::TextRole::Hint));
    for (QTextFrame *frame : document.rootFrame()->childFrames()) {
        const QTextFrameFormat format = frame->frameFormat();
        if (!format.boolProperty(CodeBlock)) {
            continue;
        }
        // The frame's bounding rectangle includes its margins; the header sits in the top one.
        const QRectF bounds = layout->frameBoundingRect(frame).translated(offset);
        if (bounds.bottom() < 0) {
            continue;
        }
        if (bounds.top() > bottom) {
            break;
        }
        CodeHeader header;
        header.frame = frame;
        header.box = bounds.adjusted(0, format.topMargin() - height, 0, -format.bottomMargin())
                         .toAlignedRect();
        const int top = header.box.top();
        header.copy = QRect(header.box.right() - (pad / 2) - button, top + ((height - button) / 2),
                            button, button);
        header.languageText = languageName(format.stringProperty(CodeLanguage), repository);
        if (header.languageText.isEmpty()) {
            header.languageText = QCoreApplication::translate("editor::TextEdit", "Plain text");
        }
        // The language comes first, then the title; the copy button keeps the right end.
        const int languageWidth =
            hint.horizontalAdvance(header.languageText) + theme.px(0.3) + theme.px(0.75);
        header.language = QRect(header.box.left() + pad, top, languageWidth, height);
        const int titleLeft = header.language.right() + pad;
        header.title =
            QRect(titleLeft, top, std::max(0, header.copy.left() - pad - titleLeft), height);
        headers.append(header);
    }
    return headers;
}

void paintCodeBackgrounds(QPainter &painter, const QList<CodeHeader> &headers,
                          const ui::Roles &roles)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(roles.surfaceContainerHighest);
    const qreal radius = ui::Theme::current()->px(0.5);
    for (const CodeHeader &header : headers) {
        painter.drawRoundedRect(header.box, radius, radius);
    }
    painter.restore();
}

void paintCodeLabels(QPainter &painter, const CodeHeader &header, const ui::Roles &roles,
                     bool drawTitle)
{
    const ui::Theme &theme = *ui::Theme::current();
    const QString title = header.frame->frameFormat().stringProperty(CodeTitle);
    if (drawTitle && !title.isEmpty()) {
        painter.setFont(theme.font(ui::TextRole::Label));
        painter.setPen(roles.onSurface);
        painter.drawText(
            header.title, Qt::AlignLeft | Qt::AlignVCenter,
            painter.fontMetrics().elidedText(title, Qt::ElideRight, header.title.width()));
    }
    painter.setFont(theme.font(ui::TextRole::Hint));
    painter.setPen(roles.onSurfaceVariant);
    const QRect text = header.language.adjusted(0, 0, -(theme.px(0.75) + theme.px(0.3)), 0);
    painter.drawText(text, Qt::AlignLeft | Qt::AlignVCenter, header.languageText);
}

void paintCodeLineNumbers(QPainter &painter, const QList<CodeHeader> &headers,
                          const ui::Roles &roles, QPointF offset, qreal bottom)
{
    if (headers.isEmpty()) {
        return;
    }
    const QTextDocument &document = *headers.first().frame->document();
    const QAbstractTextDocumentLayout *layout = document.documentLayout();
    // The numbers take the code's font and end where the block's padding starts.
    QFont font = document.defaultFont();
    font.setFamilies({QFontDatabase::systemFont(QFontDatabase::FixedFont).family()});
    const QFontMetricsF metrics(font);
    painter.save();
    painter.setFont(font);
    painter.setPen(ui::mix(roles.onSurfaceVariant, 0.6F, roles.surfaceContainerHighest));
    for (const CodeHeader &header : headers) {
        const qreal padding = header.frame->frameFormat().padding();
        int number = 0;
        for (auto it = header.frame->begin(); !it.atEnd(); ++it) {
            const QTextBlock block = it.currentBlock();
            ++number;
            const QRectF rect = layout->blockBoundingRect(block).translated(offset);
            if (rect.top() > bottom) {
                break;
            }
            if (rect.bottom() < 0 || block.layout()->lineCount() == 0) {
                continue;
            }
            const QTextLine line = block.layout()->lineAt(0);
            const QString text = QString::number(number);
            painter.drawText(QPointF(rect.left() - padding - metrics.horizontalAdvance(text),
                                     rect.top() + line.y() + line.ascent()),
                             text);
        }
    }
    painter.restore();
}

void paintInlineCode(QPainter &painter, const QTextBlock &first, const ui::Roles &roles,
                     QPointF offset, qreal bottom)
{
    if (!first.isValid()) {
        return;
    }
    const QAbstractTextDocumentLayout *layout = first.document()->documentLayout();
    const ui::Theme &theme = *ui::Theme::current();
    const qreal padding = theme.px(0.2);
    const qreal radius = theme.px(0.3);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(roles.surfaceContainerHighest);
    for (QTextBlock block = first; block.isValid(); block = block.next()) {
        const QRectF rect = layout->blockBoundingRect(block).translated(offset);
        if (rect.top() > bottom) {
            break;
        }
        const QTextLayout *text = block.layout();
        const QString content = block.text();
        // Runs of code, as positions in the block; formatting inside code splits fragments.
        int start = -1;
        int end = -1;
        const auto paintRun = [&] {
            for (int i = 0; i < text->lineCount(); ++i) {
                const QTextLine line = text->lineAt(i);
                const int from = std::max(start, line.textStart());
                const int to = std::min(end, line.textStart() + line.textLength());
                if (from >= to) {
                    continue;
                }
                const qreal left = line.cursorToX(from);
                const qreal right = line.cursorToX(to);
                // Keep at least half of neighbouring spaces outside the code background.
                // Padding must not visually join a code run to the words around it.
                const qreal before = from > line.textStart() && content.at(from - 1).isSpace()
                                         ? std::max(qreal{0}, left - line.cursorToX(from - 1)) / 2
                                         : 0;
                const qreal after = to < line.textStart() + line.textLength() &&
                                            to < content.size() && content.at(to).isSpace()
                                        ? std::max(qreal{0}, line.cursorToX(to + 1) - right) / 2
                                        : 0;
                const qreal leftPadding = std::min(padding, before);
                const qreal rightPadding = std::min(padding, after);
                painter.drawRoundedRect(QRectF(rect.left() + left - leftPadding,
                                               rect.top() + line.y(),
                                               right - left + leftPadding + rightPadding,
                                               line.ascent() + line.descent()),
                                        radius, radius);
            }
        };
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const int from = fragment.position() - block.position();
            if (!fragment.charFormat().boolProperty(InlineCode)) {
                continue;
            }
            if (from != end && start >= 0) {
                paintRun();
                start = -1;
            }
            if (start < 0) {
                start = from;
            }
            end = from + fragment.length();
        }
        if (start >= 0) {
            paintRun();
        }
    }
    painter.restore();
}

void paintBlockDecorations(QPainter &painter, const QTextBlock &first,
                           const QHash<int, QString> &markers, const ui::Roles &roles,
                           QPointF offset, qreal bottom)
{
    if (!first.isValid()) {
        return;
    }
    const QTextDocument &document = *first.document();
    const int width = std::max(2, ui::Theme::current()->px(0.2));
    const QAbstractTextDocumentLayout *layout = document.documentLayout();
    const QFont font = document.defaultFont();
    const QFontMetricsF metrics(font);
    painter.save();
    painter.setFont(font);
    painter.setPen(roles.onSurface);
    for (QTextBlock block = first; block.isValid(); block = block.next()) {
        const QRectF rect = layout->blockBoundingRect(block).translated(offset);
        if (rect.top() > bottom) {
            break;
        }
        if (block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel) > 0) {
            painter.fillRect(QRectF(rect.left(), rect.top(), width, rect.height()), roles.outline);
        }
        const auto marker = markers.constFind(block.blockNumber());
        const QTextLine line =
            block.layout()->lineCount() > 0 ? block.layout()->lineAt(0) : QTextLine();
        if (marker == markers.constEnd() || !line.isValid()) {
            continue;
        }
        // The marker ends a space before the text, on the first line's baseline, as Qt draws
        // the markers of bulleted lists.
        const qreal right = rect.left() + line.x() - metrics.horizontalAdvance(u' ');
        painter.drawText(QPointF(right - metrics.horizontalAdvance(*marker),
                                 rect.top() + line.y() + line.ascent()),
                         *marker);
    }
    painter.restore();
}

} // namespace editor
