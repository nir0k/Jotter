#include "editor/Commands.h"

#include "core/Numbering.h"
#include "editor/Presenter.h"
#include "editor/Syntax.h"
#include "ui/Theme.h"

#include <QFontDatabase>
#include <QFontMetrics>
#include <QHash>
#include <QPointer>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>

#include <numeric>
#include <utility>

namespace editor {

namespace {

/// Returns the start and end of the cursor's selection without the line breaks at its edges.
/// A selection that starts at the end of a line or ends at the start of one, as after a
/// triple-click or a drag from the end of a line, does not take that line; nor does it take
/// the empty lines at its edges.
std::pair<int, int> selectedRange(const QTextCursor &cursor)
{
    const QTextDocument *document = cursor.document();
    const auto isBreak = [document](int position) {
        const QChar character = document->characterAt(position);
        return character == QChar::ParagraphSeparator || character == QChar::LineSeparator;
    };
    int start = cursor.selectionStart();
    int end = cursor.selectionEnd();
    while (start < end && isBreak(start)) {
        ++start;
    }
    while (end > start && isBreak(end - 1)) {
        --end;
    }
    return {start, end};
}

/// Calls @p action for every block the selection takes (see selectedRange()), in order.
template <typename Action> void forEachBlock(const QTextCursor &cursor, Action action)
{
    const QTextDocument *document = cursor.document();
    const auto [start, end] = selectedRange(cursor);
    const QTextBlock last = document->findBlock(end);
    for (QTextBlock block = document->findBlock(start); block.isValid(); block = block.next()) {
        action(block);
        if (block == last) {
            break;
        }
    }
}

/// Takes a block out of its list and clears the list-only parts of its format.
void leaveList(const QTextBlock &block)
{
    if (QTextList *list = block.textList()) {
        list->remove(block);
    }
    QTextCursor cursor(block);
    QTextBlockFormat format = block.blockFormat();
    format.setIndent(0);
    format.setMarker(QTextBlockFormat::MarkerType::NoMarker);
    format.clearProperty(QTextFormat::ObjectIndex);
    cursor.setBlockFormat(format);
}

/// Puts a block into a list, creating a new list of @p format when no neighbour can take it.
void joinList(const QTextBlock &block, const QTextListFormat &format)
{
    // The previous block's list at the same level continues, so consecutive items stay one
    // list instead of several.
    for (QTextBlock previous = block.previous(); previous.isValid();
         previous = previous.previous()) {
        QTextList *list = previous.textList();
        if (list == nullptr) {
            break;
        }
        if (list->format().indent() == format.indent()) {
            if (listKindOf(list->format()) == listKindOf(format)) {
                list->add(block);
                return;
            }
            break;
        }
        if (list->format().indent() < format.indent()) {
            break;
        }
    }
    QTextCursor(block).createList(format);
}

/// Rewrites the character format of every fragment the selection covers. Merging alone cannot
/// remove a property, so clearing a colour or a link needs this.
template <typename Change> void rewriteSelection(const QTextCursor &cursor, Change change)
{
    const int start = cursor.selectionStart();
    const int end = cursor.selectionEnd();
    QTextDocument *document = cursor.document();
    QTextCursor editor(document);
    editor.beginEditBlock();
    for (QTextBlock block = document->findBlock(start); block.isValid() && block.position() < end;
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const int from = std::max(start, fragment.position());
            const int to = std::min(end, fragment.position() + fragment.length());
            if (from >= to) {
                continue;
            }
            editor.setPosition(from);
            editor.setPosition(to, QTextCursor::KeepAnchor);
            editor.setCharFormat(change(fragment.charFormat()));
        }
    }
    editor.endEditBlock();
}

/// The block format of a divider: an empty block drawn as a full-width rule.
QTextBlockFormat dividerFormat()
{
    QTextBlockFormat divider;
    divider.setProperty(QTextFormat::BlockTrailingHorizontalRulerWidth,
                        QTextLength(QTextLength::PercentageLength, 100));
    return divider;
}

void setChecklistMarker(const QTextBlock &block, bool checklist)
{
    QTextCursor cursor(block);
    QTextBlockFormat format = block.blockFormat();
    format.setMarker(checklist ? QTextBlockFormat::MarkerType::Unchecked
                               : QTextBlockFormat::MarkerType::NoMarker);
    cursor.setBlockFormat(format);
}

/// Prepares the paragraph at the cursor for a new code block or table after it. An empty
/// paragraph gives the frame its place: Qt keeps a paragraph before every frame, so the frame
/// goes after the previous paragraph unless that one is in a frame itself.
///
/// @return The position to insert the frame at.
int placeForFrame(const QTextCursor &cursor)
{
    const QTextFrame *root = cursor.document()->rootFrame();
    QTextBlock block = cursor.block();
    const QTextBlock previous = block.previous();
    if (block.text().isEmpty() && previous.isValid() &&
        QTextCursor(previous).currentFrame() == root) {
        QTextCursor merge(previous);
        merge.movePosition(QTextCursor::EndOfBlock);
        merge.deleteChar();
        // An empty previous paragraph does not survive the merge, so its handle is stale.
        block = merge.block();
    } else if (block.text().isEmpty()) {
        leaveList(block);
        QTextCursor(block).setBlockFormat(QTextBlockFormat());
    }
    return block.position() + block.length() - 1;
}

/// Qt adds an empty paragraph after a new frame; the paragraph that followed takes its place,
/// keeping its format and list, unless it is the end of the document or in a frame itself.
void joinParagraphAfter(const QTextFrame &frame)
{
    const QTextFrame *root = frame.document()->rootFrame();
    const QTextBlock after = frame.lastCursorPosition().block().next();
    const QTextBlock next = after.next();
    if (next.isValid() && QTextCursor(next).currentFrame() == root) {
        QTextCursor merge(after);
        merge.setBlockFormat(next.blockFormat());
        merge.setBlockCharFormat(next.charFormat());
        if (QTextList *list = next.textList()) {
            list->add(after);
        }
        merge.deleteChar();
    }
}

/// Returns the height of an empty paragraph of body text. Qt gives an empty paragraph right
/// before a code block or a table no room and draws the frame over it, so frames keep that much
/// room in their top margin, where such a paragraph and its cursor stay visible.
int emptyLineSpace()
{
    const ui::Theme *theme = ui::Theme::current();
    return theme != nullptr ? QFontMetrics(theme->font(ui::TextRole::Body)).height() : 17;
}

/// Returns the position at the end of a block's text.
int blockEnd(const QTextBlock &block)
{
    return block.position() + block.length() - 1;
}

/// Removes a code block or a table, leaving one paragraph in its place and the cursor in it.
void removeFrame(QTextCursor &cursor, const QTextFrame &frame)
{
    const QTextBlock after = frame.lastCursorPosition().block().next();
    const bool keepAfter = !isEmptyParagraph(after);
    const QTextBlockFormat afterFormat = after.blockFormat();
    const QTextCharFormat afterChar = after.charFormat();
    // Removing the frame joins the paragraphs around it; they are split again with the
    // paragraph that replaces the frame between them.
    cursor.setPosition(frame.firstPosition() - 1);
    cursor.setPosition(frame.lastPosition() + 1, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
    if (keepAfter) {
        cursor.insertBlock(afterFormat, afterChar);
        cursor.movePosition(QTextCursor::PreviousBlock);
    }
}

} // namespace

ListKind listKindOf(const QTextListFormat &format)
{
    if (format.boolProperty(Checklist)) {
        return ListKind::Checklist;
    }
    return format.boolProperty(Numbered) ? ListKind::Numbered : ListKind::Bulleted;
}

QTextListFormat listFormat(ListKind kind, int indent, const QString &styleId)
{
    static constexpr std::array<QTextListFormat::Style, 3> Bullets{
        QTextListFormat::ListDisc, QTextListFormat::ListCircle, QTextListFormat::ListSquare};
    QTextListFormat format;
    format.setIndent(indent);
    switch (kind) {
    case ListKind::Numbered:
        // Qt draws no marker for an undefined style; TextEdit draws the numbers.
        format.setStyle(QTextListFormat::ListStyleUndefined);
        format.setProperty(Numbered, true);
        if (!styleId.isEmpty() && styleId != core::DefaultNumberingStyle) {
            format.setProperty(NumberingStyleId, styleId);
        }
        break;
    case ListKind::Checklist:
        format.setProperty(Checklist, true);
        [[fallthrough]];
    case ListKind::Bulleted:
        format.setStyle(Bullets.at(static_cast<std::size_t>((std::max(indent, 1) - 1) % 3)));
        break;
    }
    return format;
}

void setQuote(QTextBlockFormat &format, bool quote)
{
    if (quote) {
        format.setProperty(QTextFormat::BlockQuoteLevel, 1);
        const ui::Theme *theme = ui::Theme::current();
        format.setLeftMargin(theme != nullptr ? theme->px(1.0) : 16);
    } else {
        format.clearProperty(QTextFormat::BlockQuoteLevel);
        format.setLeftMargin(0);
    }
}

BlockStyle blockStyle(const QTextCursor &cursor)
{
    const QTextBlockFormat format = cursor.blockFormat();
    if (format.intProperty(QTextFormat::BlockQuoteLevel) > 0) {
        return BlockStyle::Quote;
    }
    switch (format.headingLevel()) {
    case 1:
        return BlockStyle::Heading1;
    case 2:
        return BlockStyle::Heading2;
    case 3:
        return BlockStyle::Heading3;
    default:
        return BlockStyle::Paragraph;
    }
}

void setBlockStyle(QTextCursor cursor, BlockStyle style)
{
    cursor.beginEditBlock();
    forEachBlock(cursor, [style](const QTextBlock &block) {
        leaveList(block);
        QTextCursor blockCursor(block);
        QTextBlockFormat format = block.blockFormat();
        format.setHeadingLevel(0);
        setQuote(format, false);
        switch (style) {
        case BlockStyle::Heading1:
            format.setHeadingLevel(1);
            break;
        case BlockStyle::Heading2:
            format.setHeadingLevel(2);
            break;
        case BlockStyle::Heading3:
            format.setHeadingLevel(3);
            break;
        case BlockStyle::Quote:
            setQuote(format, true);
            break;
        case BlockStyle::Paragraph:
            break;
        }
        blockCursor.setBlockFormat(format);
    });
    cursor.endEditBlock();
}

std::optional<ListKind> listKind(const QTextCursor &cursor)
{
    if (const QTextList *list = cursor.currentList()) {
        return listKindOf(list->format());
    }
    return std::nullopt;
}

void toggleList(QTextCursor cursor, ListKind kind)
{
    cursor.beginEditBlock();
    QTextList *current = cursor.currentList();
    if (current != nullptr && listKindOf(current->format()) == kind) {
        forEachBlock(cursor, [](const QTextBlock &block) { leaveList(block); });
    } else if (current != nullptr) {
        // Changing the kind of a list changes all its items at that level; deeper lists keep
        // their own kind.
        const QTextListFormat format = listFormat(kind, std::max(1, current->format().indent()));
        current->setFormat(format);
        for (int i = 0; i < current->count(); ++i) {
            setChecklistMarker(current->item(i), kind == ListKind::Checklist);
        }
    } else {
        forEachBlock(cursor, [kind](const QTextBlock &block) {
            QTextCursor blockCursor(block);
            QTextBlockFormat format = block.blockFormat();
            format.setHeadingLevel(0);
            setQuote(format, false);
            blockCursor.setBlockFormat(format);
            joinList(block, listFormat(kind, 1));
            setChecklistMarker(block, kind == ListKind::Checklist);
        });
    }
    cursor.endEditBlock();
}

void setNumberingStyle(QTextCursor cursor, const QString &styleId)
{
    cursor.beginEditBlock();
    if (listKind(cursor) != ListKind::Numbered) {
        toggleList(cursor, ListKind::Numbered);
    }
    if (QTextList *list = cursor.currentList()) {
        QTextListFormat format = list->format();
        if (styleId.isEmpty() || styleId == core::DefaultNumberingStyle) {
            format.clearProperty(NumberingStyleId);
        } else {
            format.setProperty(NumberingStyleId, styleId);
        }
        list->setFormat(format);
    }
    cursor.endEditBlock();
}

bool setListStart(QTextCursor cursor, int start)
{
    QTextList *list = cursor.currentList();
    if (list == nullptr || listKindOf(list->format()) != ListKind::Numbered) {
        return false;
    }
    QTextListFormat format = list->format();
    format.setStart(std::max(1, start));
    const int index = list->itemNumber(cursor.block());
    cursor.beginEditBlock();
    if (index <= 0) {
        list->setFormat(format);
    } else {
        QList<QTextBlock> items;
        for (int i = index; i < list->count(); ++i) {
            items.append(list->item(i));
        }
        for (const QTextBlock &item : items) {
            list->remove(item);
        }
        QTextList *rest = QTextCursor(items.first()).createList(format);
        for (qsizetype i = 1; i < items.size(); ++i) {
            rest->add(items.at(i));
        }
    }
    cursor.endEditBlock();
    return true;
}

bool changeListLevel(QTextCursor cursor, int delta)
{
    bool changed = false;
    cursor.beginEditBlock();
    forEachBlock(cursor, [&](const QTextBlock &block) {
        const QTextList *list = block.textList();
        if (list == nullptr) {
            return;
        }
        changed = true;
        const QTextListFormat current = list->format();
        const int indent = std::max(1, current.indent()) + delta;
        const bool checked = block.blockFormat().marker() == QTextBlockFormat::MarkerType::Checked;
        if (indent < 1) {
            leaveList(block);
            return;
        }
        if (indent > 9) {
            return; // Lists nest up to nine levels.
        }
        const ListKind kind = listKindOf(current);
        joinList(block, listFormat(kind, indent, current.stringProperty(NumberingStyleId)));
        if (kind == ListKind::Checklist) {
            QTextCursor blockCursor(block);
            QTextBlockFormat blockFormat = block.blockFormat();
            blockFormat.setMarker(checked ? QTextBlockFormat::MarkerType::Checked
                                          : QTextBlockFormat::MarkerType::Unchecked);
            blockCursor.setBlockFormat(blockFormat);
        }
    });
    cursor.endEditBlock();
    return changed;
}

bool toggleChecked(QTextCursor cursor)
{
    const QTextList *list = cursor.currentList();
    if (list == nullptr || !list->format().boolProperty(Checklist)) {
        return false;
    }
    QTextBlockFormat format = cursor.blockFormat();
    format.setMarker(format.marker() == QTextBlockFormat::MarkerType::Checked
                         ? QTextBlockFormat::MarkerType::Unchecked
                         : QTextBlockFormat::MarkerType::Checked);
    cursor.setBlockFormat(format);
    return true;
}

void insertDivider(QTextCursor cursor)
{
    cursor.beginEditBlock();
    cursor.movePosition(QTextCursor::EndOfBlock);
    cursor.insertBlock(dividerFormat(), QTextCharFormat());
    cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
    cursor.endEditBlock();
}

bool applyMarkdownPrefix(QTextCursor cursor)
{
    if (cursor.hasSelection()) {
        return false;
    }
    const QTextBlock block = cursor.block();
    if (codeBlockOf(block) != nullptr) {
        return false;
    }
    const QString prefix = block.text().left(cursor.positionInBlock());
    static const QRegularExpression numbered(QStringLiteral(R"(^([1-9]\d{0,8})\. $)"));
    std::optional<ListKind> kind;
    std::optional<BlockStyle> style;
    int start = 1;
    if (prefix == QLatin1StringView("- ") || prefix == QLatin1StringView("* ")) {
        kind = ListKind::Bulleted;
    } else if (prefix == QLatin1StringView("[] ") || prefix == QLatin1StringView("[ ] ")) {
        kind = ListKind::Checklist;
    } else if (const QRegularExpressionMatch match = numbered.match(prefix); match.hasMatch()) {
        kind = ListKind::Numbered;
        start = match.captured(1).toInt();
    } else if (block.textList() == nullptr && blockStyle(cursor) == BlockStyle::Paragraph &&
               cursor.currentTable() == nullptr) {
        static const QHash<QString, BlockStyle> styles{
            {QStringLiteral("# "), BlockStyle::Heading1},
            {QStringLiteral("## "), BlockStyle::Heading2},
            {QStringLiteral("### "), BlockStyle::Heading3},
            {QStringLiteral("> "), BlockStyle::Quote},
        };
        if (const auto it = styles.constFind(prefix); it != styles.cend()) {
            style = *it;
        }
    }
    // In a list of the same kind the characters are text the person means to keep.
    if ((!kind && !style) || (kind && listKind(cursor) == kind)) {
        return false;
    }

    cursor.beginEditBlock();
    QTextCursor typed(block);
    typed.setPosition(block.position() + static_cast<int>(prefix.size()), QTextCursor::KeepAnchor);
    typed.removeSelectedText();
    if (kind) {
        toggleList(cursor, *kind);
        // A list that continues the one above keeps its numbering.
        QTextList *list = cursor.currentList();
        if (start > 1 && list != nullptr && list->count() == 1) {
            QTextListFormat format = list->format();
            format.setStart(start);
            list->setFormat(format);
        }
    } else {
        setBlockStyle(cursor, *style);
    }
    cursor.endEditBlock();
    return true;
}

bool applyMarkdownLine(QTextCursor &cursor)
{
    const QTextBlock block = cursor.block();
    if (!cursor.atBlockEnd() || block.textList() != nullptr || codeBlockOf(block) != nullptr ||
        cursor.currentTable() != nullptr || blockStyle(cursor) != BlockStyle::Paragraph) {
        return false;
    }
    static const QRegularExpression fence(QStringLiteral(R"(^```(\S*)$)"));
    static const QRegularExpression rule(QStringLiteral("^-{3,}$"));
    const QRegularExpressionMatch match = fence.match(block.text());
    if (!match.hasMatch() && !rule.match(block.text()).hasMatch()) {
        return false;
    }
    cursor.beginEditBlock();
    cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    if (match.hasMatch()) {
        insertCodeBlock(cursor, languageKey(match.captured(1)));
    } else {
        cursor.setBlockFormat(dividerFormat());
        cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
    }
    cursor.endEditBlock();
    return true;
}

bool applyMarkdownCode(QTextCursor &cursor)
{
    const QTextBlock block = cursor.block();
    if (cursor.hasSelection() || codeBlockOf(block) != nullptr) {
        return false;
    }
    const QString text = block.text();
    const qsizetype close = cursor.positionInBlock() - 1;
    if (close < 2 || text.at(close) != u'`') {
        return false;
    }
    const qsizetype open = text.lastIndexOf(u'`', close - 1);
    // A backtick right before the opening one makes a double backtick, which is not ours.
    if (open < 0 || (open > 0 && text.at(open - 1) == u'`')) {
        return false;
    }
    const QString code = text.sliced(open + 1, close - open - 1);
    if (code.trimmed().isEmpty() || code.contains(QChar::LineSeparator) ||
        code.contains(QChar::ObjectReplacementCharacter)) {
        return false;
    }

    const int start = block.position() + static_cast<int>(open);
    cursor.beginEditBlock();
    QTextCursor edit(block);
    edit.setPosition(start + static_cast<int>(close - open));
    edit.deleteChar();
    edit.setPosition(start);
    edit.deleteChar();
    edit.setPosition(start + static_cast<int>(code.size()), QTextCursor::KeepAnchor);
    QTextCharFormat format;
    format.setProperty(InlineCode, true);
    edit.mergeCharFormat(format);
    cursor.endEditBlock();
    QTextCharFormat next = cursor.charFormat();
    next.clearProperty(InlineCode);
    cursor.setCharFormat(next);
    return true;
}

bool leaveInlineCode(QTextCursor &cursor)
{
    // At the start of a block the format is that of the character after the cursor.
    if (cursor.hasSelection() || cursor.positionInBlock() == 0 ||
        !cursor.charFormat().boolProperty(InlineCode)) {
        return false;
    }
    if (!cursor.atBlockEnd()) {
        QTextCursor next = cursor;
        next.movePosition(QTextCursor::NextCharacter);
        if (next.charFormat().boolProperty(InlineCode)) {
            return false;
        }
    }
    QTextCharFormat plain = cursor.charFormat();
    plain.clearProperty(InlineCode);
    cursor.setCharFormat(plain);
    return true;
}

bool wrapSelection(QTextCursor &cursor, QChar typed)
{
    static const QHash<QChar, QChar> pairs{
        {u'(', u')'},   {u'[', u']'}, {u'{', u'}'}, {u'"', u'"'},
        {u'\'', u'\''}, {u'«', u'»'}, {u'`', u'`'},
    };
    const auto close = pairs.constFind(typed);
    if (close == pairs.constEnd() || !cursor.hasSelection() || cursor.hasComplexSelection()) {
        return false;
    }
    const int start = cursor.selectionStart();
    const int end = cursor.selectionEnd();
    if (typed == u'`' && codeBlockOf(cursor.block()) == nullptr) {
        QTextCursor last(cursor.document());
        last.setPosition(end);
        const QString text = cursor.selectedText();
        if (last.block() == cursor.document()->findBlock(start) && !text.trimmed().isEmpty() &&
            !text.contains(QChar::LineSeparator) &&
            !text.contains(QChar::ObjectReplacementCharacter)) {
            QTextCharFormat code;
            code.setProperty(InlineCode, true);
            cursor.mergeCharFormat(code);
            return true;
        }
    }
    cursor.beginEditBlock();
    QTextCursor edit = cursor;
    edit.setPosition(end);
    edit.insertText(QString(*close));
    edit.setPosition(start);
    edit.insertText(QString(typed));
    cursor.endEditBlock();
    cursor.setPosition(start + 1);
    cursor.setPosition(end + 1, QTextCursor::KeepAnchor);
    return true;
}

QTextFrameFormat codeBlockFormat(const QString &language, const QString &title)
{
    QTextFrameFormat format;
    format.setProperty(CodeBlock, true);
    if (!language.isEmpty()) {
        format.setProperty(CodeLanguage, language);
    }
    if (!title.isEmpty()) {
        format.setProperty(CodeTitle, title);
    }
    // The margins and the padding are only how the block looks; the note file stores the code.
    const ui::Theme *theme = ui::Theme::current();
    const auto px = [theme](qreal rem) {
        return theme != nullptr ? theme->px(rem) : static_cast<int>(std::lround(rem * 16));
    };
    format.setBorder(0);
    format.setPadding(px(0.75));
    format.setTopMargin(px(0.5) + emptyLineSpace() + codeHeaderHeight());
    format.setLeftMargin(codeGutterWidth());
    format.setBottomMargin(px(0.5));
    return format;
}

QTextTableFormat tableFormat(int columns, int headerRows, const QList<qreal> &widths, int width)
{
    const ui::Theme *theme = ui::Theme::current();
    const auto px = [theme](qreal rem) {
        return theme != nullptr ? theme->px(rem) : static_cast<int>(std::lround(rem * 16));
    };
    QTextTableFormat format;
    format.setWidth(QTextLength(QTextLength::PercentageLength, width > 0 ? width : 100));
    setColumnWidths(format, columns, widths);
    format.setHeaderRowCount(headerRows);
    format.setBorder(0);
    format.setCellSpacing(0);
    format.setCellPadding(px(0.4));
    format.setTopMargin(px(0.5) + emptyLineSpace());
    format.setBottomMargin(px(0.5));
    return format;
}

int tableWidth(const QTextTableFormat &format)
{
    const QTextLength width = format.width();
    return width.type() == QTextLength::PercentageLength
               ? validWidth(static_cast<int>(std::lround(width.rawValue())))
               : 0;
}

void setTableWidth(QTextTable &table, int width)
{
    QTextTableFormat format = table.format();
    width = validWidth(std::max(width, MinWidth));
    format.setWidth(QTextLength(QTextLength::PercentageLength, width > 0 ? width : 100));
    table.setFormat(format);
}

QList<qreal> columnWidths(const QTextTableFormat &format, int columns)
{
    QList<qreal> widths;
    for (const QTextLength &length : format.columnWidthConstraints()) {
        widths.append(length.type() == QTextLength::PercentageLength ? length.rawValue() : 0);
    }
    if (widths.size() != columns || std::ranges::any_of(widths, [](qreal w) { return w <= 0; })) {
        widths.fill(100.0 / std::max(1, columns), columns);
    }
    return widths;
}

bool hasEqualColumns(const QTextTableFormat &format, int columns)
{
    const QList<qreal> widths = columnWidths(format, columns);
    // Whole percents cannot split 100 equally among three columns: 33, 33, 34 are equal.
    return std::ranges::all_of(
        widths, [&](qreal w) { return std::abs(w - (100.0 / std::max(1, columns))) < 1.0; });
}

void setColumnWidths(QTextTableFormat &format, int columns, QList<qreal> widths)
{
    columns = std::max(1, columns);
    if (widths.size() != columns || std::ranges::any_of(widths, [](qreal w) { return w <= 0; })) {
        widths = QList<qreal>(columns, 1.0);
    }
    const qreal minimum = std::min<qreal>(MinColumnWidth, 100.0 / columns);
    // Scale to 100, raise the narrow columns to the minimum, then take the excess from the
    // others in their proportions.
    qreal total = std::accumulate(widths.begin(), widths.end(), 0.0);
    for (qreal &w : widths) {
        w = std::max(minimum, w * 100.0 / total);
    }
    total = std::accumulate(widths.begin(), widths.end(), 0.0);
    const qreal excess = total - 100.0;
    qreal flexible = 0;
    for (const qreal w : widths) {
        flexible += w - minimum;
    }
    QList<int> whole;
    for (const qreal w : widths) {
        const qreal scaled = flexible > 0 ? w - (excess * (w - minimum) / flexible) : w;
        whole.append(static_cast<int>(std::lround(scaled)));
    }
    // Rounding may leave a percent over or under; the widest column takes it.
    const int sum = std::accumulate(whole.begin(), whole.end(), 0);
    *std::ranges::max_element(whole) += 100 - sum;
    QList<QTextLength> lengths;
    for (const int w : whole) {
        lengths.append(QTextLength(QTextLength::PercentageLength, w));
    }
    format.setColumnWidthConstraints(lengths);
}

void resizeColumns(QTextTable &table, const QList<qreal> &widths)
{
    QTextTableFormat format = table.format();
    setColumnWidths(format, table.columns(), widths);
    table.setFormat(format);
}

QList<qreal> withColumnWidth(QList<qreal> widths, int column, qreal width)
{
    if (column < 0 || column >= widths.size() || widths.size() < 2) {
        return widths;
    }
    const auto others = static_cast<qreal>(widths.size() - 1);
    width = std::clamp<qreal>(width, MinColumnWidth, 100.0 - (MinColumnWidth * others));
    const qreal rest = 100.0 - widths.at(column);
    for (qsizetype i = 0; i < widths.size(); ++i) {
        if (i == column) {
            widths[i] = width;
        } else if (rest > 0) {
            widths[i] = widths.at(i) * (100.0 - width) / rest;
        } else {
            widths[i] = (100.0 - width) / others;
        }
    }
    return widths;
}

int codeHeaderHeight()
{
    const ui::Theme *theme = ui::Theme::current();
    return theme != nullptr ? theme->px(1.75) : 28;
}

int codeGutterWidth()
{
    const ui::Theme *theme = ui::Theme::current();
    const QFontMetrics digits(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    return digits.horizontalAdvance(QStringLiteral("000")) +
           (theme != nullptr ? theme->px(0.75) : 12);
}

bool isEmptyParagraph(const QTextBlock &block)
{
    const QTextBlockFormat format = block.blockFormat();
    return block.text().isEmpty() && block.textList() == nullptr && format.headingLevel() == 0 &&
           format.intProperty(QTextFormat::BlockQuoteLevel) == 0 &&
           !format.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth);
}

QTextFrame *codeBlockOf(const QTextBlock &block)
{
    if (!block.isValid()) {
        return nullptr;
    }
    if (QTextFrame *frame = QTextCursor(block).currentFrame();
        frame != nullptr && frame->frameFormat().boolProperty(CodeBlock)) {
        return frame;
    }
    return nullptr;
}

QString codeText(const QTextFrame &frame)
{
    QStringList lines;
    const QTextBlock last = frame.lastCursorPosition().block();
    for (QTextBlock block = frame.firstCursorPosition().block(); block.isValid();
         block = block.next()) {
        lines.append(block.text().replace(QChar::LineSeparator, u'\n'));
        if (block == last) {
            break;
        }
    }
    return lines.join(u'\n');
}

bool canInsertCodeBlock(const QTextCursor &cursor)
{
    if (codeBlockOf(cursor.block()) != nullptr || cursor.currentTable() != nullptr) {
        return false;
    }
    bool plain = true;
    if (cursor.hasSelection()) {
        forEachBlock(cursor, [&plain](const QTextBlock &block) {
            plain = plain && codeBlockOf(block) == nullptr &&
                    QTextCursor(block).currentTable() == nullptr;
        });
    }
    return plain;
}

bool formatCode(QTextCursor &cursor)
{
    if (!canInsertCodeBlock(cursor)) {
        return false;
    }
    if (cursor.hasSelection()) {
        const auto [start, end] = selectedRange(cursor);
        const QTextBlock first = cursor.document()->findBlock(start);
        if (start < end && first == cursor.document()->findBlock(end)) {
            const QString text = first.text();
            const int from = start - first.position();
            const int to = end - first.position();
            const qsizetype lineStart =
                from > 0 ? text.lastIndexOf(QChar::LineSeparator, from - 1) + 1 : 0;
            const qsizetype nextBreak = text.indexOf(QChar::LineSeparator, from);
            const qsizetype lineEnd = nextBreak >= 0 ? nextBreak : text.size();
            if (to <= lineEnd && (from > lineStart || to < lineEnd)) {
                QTextCursor fragment = cursor;
                fragment.setPosition(start);
                fragment.setPosition(end, QTextCursor::KeepAnchor);
                QTextCharFormat code;
                code.setProperty(InlineCode, true);
                fragment.mergeCharFormat(code);
                return true;
            }
        }
    }
    return insertCodeBlock(cursor) != nullptr;
}

QTextFrame *insertCodeBlock(QTextCursor &cursor, const QString &language)
{
    if (!canInsertCodeBlock(cursor)) {
        return nullptr;
    }
    const QTextDocument *document = cursor.document();
    cursor.beginEditBlock();
    QStringList lines;
    if (cursor.hasSelection()) {
        // Only selected characters move. Edge line breaks keep neighbouring lines outside.
        const auto [start, end] = selectedRange(cursor);
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        lines = cursor.selectedText()
                    .replace(QChar::LineSeparator, QChar::ParagraphSeparator)
                    .split(QChar::ParagraphSeparator);
        cursor.removeSelectedText();
        // Split the remaining prefix and suffix so the frame has a paragraph of its own.
        if (document->characterAt(cursor.position()) == QChar::LineSeparator) {
            cursor.deleteChar();
        }
        if (!cursor.atBlockEnd()) {
            cursor.insertBlock();
            cursor.movePosition(QTextCursor::PreviousBlock);
            cursor.movePosition(QTextCursor::EndOfBlock);
        }
        if (cursor.position() > 0 &&
            document->characterAt(cursor.position() - 1) == QChar::LineSeparator) {
            cursor.deletePreviousChar();
        }
        if (!cursor.atBlockStart()) {
            cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
        }
    }
    cursor.setPosition(placeForFrame(cursor));
    QTextFrame *frame = cursor.insertFrame(codeBlockFormat(language, {}));
    cursor.setBlockCharFormat(QTextCharFormat());
    for (qsizetype i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
        }
        cursor.insertText(lines.at(i), QTextCharFormat());
    }
    joinParagraphAfter(*frame);
    cursor.endEditBlock();
    cursor = frame->lastCursorPosition();
    return frame;
}

QTextTable *insertTable(QTextCursor &cursor, int rows, int columns, bool header)
{
    if (codeBlockOf(cursor.block()) != nullptr || cursor.currentTable() != nullptr || rows < 1 ||
        columns < 1) {
        return nullptr;
    }
    cursor.beginEditBlock();
    cursor.setPosition(cursor.selectionEnd());
    cursor.setPosition(placeForFrame(cursor));
    QTextTable *table = cursor.insertTable(rows, columns, tableFormat(columns, header ? 1 : 0));
    joinParagraphAfter(*table);
    cursor.endEditBlock();
    cursor = table->cellAt(0, 0).firstCursorPosition();
    return table;
}

bool changeTable(QTextCursor &cursor, TableChange change)
{
    QTextTable *table = cursor.currentTable();
    if (table == nullptr) {
        return false;
    }
    const QTextTableCell cell = table->cellAt(cursor);
    const int row = cell.row();
    const int column = cell.column();
    if ((change == TableChange::DeleteRow && table->rows() == 1) ||
        (change == TableChange::DeleteColumn && table->columns() == 1)) {
        change = TableChange::DeleteTable;
    }
    cursor.beginEditBlock();
    QTextTableFormat format = table->format();
    // Equal columns stay equal; otherwise a new column gets an equal share and the others keep
    // their proportions.
    const bool equal = hasEqualColumns(format, table->columns());
    QList<qreal> widths = columnWidths(format, table->columns());
    switch (change) {
    case TableChange::RowAbove:
        table->insertRows(row, 1);
        break;
    case TableChange::RowBelow:
        table->insertRows(row + 1, 1);
        break;
    case TableChange::ColumnLeft:
        table->insertColumns(column, 1);
        widths.insert(column, 0);
        widths = withColumnWidth(widths, column, 100.0 / table->columns());
        break;
    case TableChange::ColumnRight:
        table->insertColumns(column + 1, 1);
        widths.insert(column + 1, 0);
        widths = withColumnWidth(widths, column + 1, 100.0 / table->columns());
        break;
    case TableChange::DeleteRow:
        table->removeRows(row, 1);
        // The header row is the first one; deleting it leaves the table without a header.
        if (row < format.headerRowCount()) {
            format.setHeaderRowCount(0);
        }
        break;
    case TableChange::DeleteColumn:
        table->removeColumns(column, 1);
        widths.removeAt(column);
        break;
    case TableChange::ToggleHeader:
        format.setHeaderRowCount(format.headerRowCount() > 0 ? 0 : 1);
        break;
    case TableChange::DeleteTable:
        removeFrame(cursor, *table);
        cursor.endEditBlock();
        return true;
    }
    setColumnWidths(format, table->columns(), equal ? QList<qreal>() : widths);
    table->setFormat(format);
    rehighlight(*table);
    const int targetRow =
        std::min(change == TableChange::RowBelow ? row + 1 : row, table->rows() - 1);
    const int targetColumn =
        std::min(change == TableChange::ColumnRight ? column + 1 : column, table->columns() - 1);
    cursor.endEditBlock();
    cursor = table->cellAt(targetRow, targetColumn).firstCursorPosition();
    return true;
}

QTextFrame *topFrameOf(const QTextBlock &block)
{
    const QTextFrame *root = block.document()->rootFrame();
    QTextFrame *frame = QTextCursor(block).currentFrame();
    if (frame == root) {
        return nullptr;
    }
    while (frame->parentFrame() != root) {
        frame = frame->parentFrame();
    }
    return frame;
}

int tablePlace(const QTextBlock &block, bool after)
{
    // Qt keeps a paragraph before and after every frame.
    if (const QTextFrame *frame = topFrameOf(block)) {
        return after ? frame->lastCursorPosition().block().next().position()
                     : blockEnd(frame->firstCursorPosition().block().previous());
    }
    if (after) {
        return blockEnd(block);
    }
    const QTextBlock previous = block.previous();
    return previous.isValid() && topFrameOf(previous) == nullptr ? blockEnd(previous)
                                                                 : block.position();
}

int tableMovePlace(const QTextTable &table, int direction)
{
    const bool down = direction > 0;
    const QTextBlock next = down ? table.lastCursorPosition().block().next()
                                 : table.firstCursorPosition().block().previous();
    const QTextBlock beyond = down ? next.next() : next.previous();
    // An empty paragraph between the table and a frame or the document's edge only keeps them
    // apart, so the table passes it with what lies beyond.
    QTextBlock item = next;
    if (isEmptyParagraph(next) && (!beyond.isValid() || topFrameOf(beyond) != nullptr)) {
        item = beyond;
    }
    return item.isValid() ? tablePlace(item, down) : -1;
}

QTextTable *moveTable(QTextTable &table, int position)
{
    QTextDocument *document = table.document();
    const QTextBlock before = table.firstCursorPosition().block().previous();
    const QTextBlock after = table.lastCursorPosition().block().next();
    const QTextBlock target = document->findBlock(position);
    const QTextBlock targetPrevious = target.previous();
    const bool place = target.isValid() && topFrameOf(target) == nullptr &&
                       (position == blockEnd(target) ||
                        (position == target.position() &&
                         (!targetPrevious.isValid() || topFrameOf(targetPrevious) != nullptr)));
    if (!place || position == blockEnd(before) || position == after.position()) {
        return nullptr;
    }
    QTextCursor at(document);
    at.setPosition(position);
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    cursor.setPosition(table.firstPosition() - 1);
    cursor.setPosition(table.lastPosition() + 1, QTextCursor::KeepAnchor);
    const QTextDocumentFragment fragment = cursor.selection();
    // Removing the table joins the paragraphs around it; they are split again unless one of
    // them is empty, which only kept room for the table.
    const bool split = !isEmptyParagraph(before) && !isEmptyParagraph(after);
    const bool takeAfter = !split && isEmptyParagraph(before) && !isEmptyParagraph(after);
    const QTextBlockFormat afterFormat = after.blockFormat();
    const QTextCharFormat afterChar = after.charFormat();
    const QPointer<QTextList> afterList = after.textList();
    cursor.removeSelectedText();
    if (split) {
        cursor.insertBlock(afterFormat, afterChar);
    } else if (takeAfter) {
        cursor.setBlockFormat(afterFormat);
        cursor.setBlockCharFormat(afterChar);
    }
    if ((split || takeAfter) && afterList != nullptr) {
        afterList->add(cursor.block());
    }
    const bool afterParagraph = at.position() == blockEnd(at.block());
    const int start = at.position();
    at.insertFragment(fragment);
    // Qt finds the inserted table once the edit block ends; the paragraph is joined in the same
    // undo step. After a paragraph, Qt adds an empty one after the table, which takes the place
    // of the one that followed; before a paragraph, the table gets the one Qt keeps after a
    // frame.
    cursor.endEditBlock();
    QTextCursor inTable(document);
    inTable.setPosition(start + 1);
    QTextTable *moved = inTable.currentTable();
    if (moved != nullptr && afterParagraph) {
        cursor.joinPreviousEditBlock();
        joinParagraphAfter(*moved);
        cursor.endEditBlock();
    }
    return moved;
}

bool moveToCell(QTextCursor &cursor, int delta)
{
    QTextTable *table = cursor.currentTable();
    if (table == nullptr) {
        return false;
    }
    const QTextTableCell cell = table->cellAt(cursor);
    int index = (cell.row() * table->columns()) + cell.column() + delta;
    if (index < 0) {
        return true;
    }
    if (index >= table->rows() * table->columns()) {
        table->appendRows(1);
        index = (table->rows() - 1) * table->columns();
    }
    const QTextTableCell target = table->cellAt(index / table->columns(), index % table->columns());
    // The cell's text is selected, as in other editors, so typing replaces it.
    cursor = target.firstCursorPosition();
    cursor.setPosition(target.lastCursorPosition().position(), QTextCursor::KeepAnchor);
    return true;
}

void setCodeBlockProperty(QTextFrame &frame, int property, const QString &value)
{
    QTextFrameFormat format = frame.frameFormat();
    if (value.isEmpty()) {
        format.clearProperty(property);
    } else {
        format.setProperty(property, value);
    }
    frame.setFrameFormat(format);
    // The language changes the highlighting and the title the header; both are drawn again.
    rehighlight(frame);
    frame.document()->markContentsDirty(frame.firstPosition(),
                                        frame.lastPosition() - frame.firstPosition());
}

bool leaveCodeBlock(QTextCursor &cursor)
{
    const QTextFrame *frame = codeBlockOf(cursor.block());
    if (frame == nullptr) {
        return false;
    }
    const QTextBlock after = frame->lastCursorPosition().block().next();
    cursor.setPosition(after.position());
    if (!isEmptyParagraph(after)) {
        // Splitting at the start moves the text into a new block with its own format; the
        // block left behind becomes the empty paragraph.
        cursor.beginEditBlock();
        cursor.insertBlock(after.blockFormat(), after.charFormat());
        leaveList(after);
        QTextCursor(after).setBlockFormat(QTextBlockFormat());
        cursor.endEditBlock();
        cursor.setPosition(after.position());
    }
    return true;
}

bool removeEmptyCodeBlock(QTextCursor &cursor)
{
    const QTextFrame *frame = codeBlockOf(cursor.block());
    if (frame == nullptr || frame->firstPosition() != frame->lastPosition()) {
        return false;
    }
    cursor.beginEditBlock();
    removeFrame(cursor, *frame);
    cursor.endEditBlock();
    return true;
}

bool indentCode(QTextCursor cursor, int delta)
{
    if (codeBlockOf(cursor.block()) == nullptr) {
        return false;
    }
    static const QString Indent(4, u' ');
    if (delta > 0 && !cursor.hasSelection()) {
        cursor.insertText(Indent);
        return true;
    }
    cursor.beginEditBlock();
    forEachBlock(cursor, [delta](const QTextBlock &block) {
        if (codeBlockOf(block) == nullptr) {
            return;
        }
        QTextCursor line(block);
        if (delta > 0) {
            line.insertText(Indent);
            return;
        }
        const QString text = block.text();
        int spaces = 0;
        while (spaces < Indent.size() && spaces < text.size() && text.at(spaces) == u' ') {
            ++spaces;
        }
        line.setPosition(block.position() + spaces, QTextCursor::KeepAnchor);
        line.removeSelectedText();
    });
    cursor.endEditBlock();
    return true;
}

void setLink(QTextCursor cursor, const QString &href, const QString &text)
{
    cursor.beginEditBlock();
    QTextCharFormat format;
    format.setAnchor(true);
    format.setAnchorHref(href);
    if (!text.isEmpty()) {
        QTextCharFormat inserted = cursor.charFormat();
        inserted.merge(format);
        cursor.insertText(text, inserted);
    } else {
        cursor.mergeCharFormat(format);
    }
    cursor.endEditBlock();
}

bool selectLink(QTextCursor &cursor)
{
    const QTextBlock block = cursor.block();
    const int position = cursor.position();
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (!fragment.charFormat().isAnchor() || position < fragment.position() ||
            position > fragment.position() + fragment.length()) {
            continue;
        }
        // Neighbouring fragments with the same address (bold inside a link) belong to it too.
        const QString href = fragment.charFormat().anchorHref();
        int start = fragment.position();
        int end = fragment.position() + fragment.length();
        for (auto other = block.begin(); !other.atEnd(); ++other) {
            const QTextFragment piece = other.fragment();
            if (piece.charFormat().anchorHref() != href) {
                continue;
            }
            if (piece.position() + piece.length() == start) {
                start = piece.position();
            }
            if (piece.position() == end) {
                end = piece.position() + piece.length();
            }
        }
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        return true;
    }
    return false;
}

void removeLink(QTextCursor cursor)
{
    if (!cursor.hasSelection()) {
        selectLink(cursor);
    }
    rewriteSelection(cursor, [](QTextCharFormat format) {
        format.setAnchor(false);
        format.clearProperty(QTextFormat::AnchorHref);
        return format;
    });
}

void setCharProperty(QTextEdit &edit, int property, const QVariant &value)
{
    const QTextCursor cursor = edit.textCursor();
    const auto change = [&](QTextCharFormat format) {
        if (!value.isValid() ||
            (value.typeId() == QMetaType::QString && value.toString().isEmpty()) ||
            (value.typeId() == QMetaType::QStringList && value.toStringList().isEmpty())) {
            format.clearProperty(property);
        } else {
            format.setProperty(property, value);
        }
        return format;
    };
    if (!cursor.hasSelection()) {
        edit.setCurrentCharFormat(change(edit.currentCharFormat()));
        return;
    }
    rewriteSelection(cursor, change);
}

} // namespace editor
