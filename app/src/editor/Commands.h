#pragma once

#include "editor/Formats.h"

#include <QString>
#include <QTextCharFormat>

class QTextBlock;
class QTextCursor;
class QTextEdit;
class QTextFrame;
class QTextListFormat;
class QTextTable;
class QTextTableFormat;

/// Editing commands shared by the toolbar, the shortcuts and the text widget. Each works on the
/// cursor's selection, or on the current block and the text typed next when nothing is selected.
namespace editor {

/// The kinds of list.
enum class ListKind { Bulleted, Numbered, Checklist };

/// Returns the kind of a list.
ListKind listKindOf(const QTextListFormat &format);

/// Returns the list format of a kind at a nesting level. Bullets change shape by level: disc,
/// circle, square.
///
/// @param kind The list kind.
/// @param indent The nesting level, from 1.
/// @param styleId The numbering style of a numbered list, empty for `1.`.
QTextListFormat listFormat(ListKind kind, int indent, const QString &styleId = {});

/// Returns the paragraph style of a cursor's block.
BlockStyle blockStyle(const QTextCursor &cursor);

/// Sets the paragraph style of the selected blocks; list items leave their list first.
void setBlockStyle(QTextCursor cursor, BlockStyle style);

/// Returns the list kind of a cursor's block, or no value outside lists.
std::optional<ListKind> listKind(const QTextCursor &cursor);

/// Makes the selected blocks a list of @p kind. Blocks already in a list of that kind leave it;
/// a list of another kind changes its kind, items at other levels keep theirs.
void toggleList(QTextCursor cursor, ListKind kind);

/// Gives the numbered list at the cursor a numbering style, or makes the selected blocks a
/// numbered list of that style.
///
/// @param cursor The cursor.
/// @param styleId The style's identifier; `decimal` or empty for `1.`.
void setNumberingStyle(QTextCursor cursor, const QString &styleId);

/// Makes the numbered list at the cursor count from @p start at the cursor's item (Restart
/// numbering is 1, Start at… any number). An item after the first starts a list of its own
/// with the following items, so the items before keep their numbers.
///
/// @return false when the cursor is not in a numbered list.
bool setListStart(QTextCursor cursor, int start);

/// Moves the selected list items one level deeper (@p delta 1) or shallower (-1). An item at
/// the first level that moves shallower leaves the list.
///
/// @return false when no selected block is a list item, so Tab can do its ordinary work.
bool changeListLevel(QTextCursor cursor, int delta);

/// Switches the checklist item at the cursor between checked and unchecked.
///
/// @return false when the block is not a checklist item.
bool toggleChecked(QTextCursor cursor);

/// Inserts a divider after the current block, followed by an empty paragraph.
void insertDivider(QTextCursor cursor);

/// Applies the Markdown shortcut typed at the start of the cursor's block once a space follows
/// it: `#` to `###` headings and `>` quote in a paragraph; `-` or `*`
/// bulleted list, `N.` numbered list starting at N, `[]` or `[ ]` checklist in a paragraph or
/// in a list item of another kind. Table cells take the list shortcuts only. The change is one
/// undo step after the typing, so undo brings the typed characters back.
///
/// @param cursor A cursor right after the typed space.
/// @return true when a shortcut was applied.
bool applyMarkdownPrefix(QTextCursor cursor);

/// Applies the Markdown shortcut that is a whole paragraph, when Return is pressed at its end:
/// three or more `-` become a divider, and ` ``` ` followed by an optional language name becomes
/// a code block. Neither applies in a table cell. Undo brings the typed characters back.
///
/// @param cursor A cursor at the end of the paragraph; moved to the paragraph after the change.
/// @return true when a shortcut was applied and Return needs no further handling.
bool applyMarkdownLine(QTextCursor &cursor);

/// Applies the inline code shortcut once the closing backtick is typed:
/// text between two backticks on one line becomes inline code and the backticks go. Empty or
/// blank text, a double backtick and code blocks are left alone. The change is one undo step
/// after the typing, so undo brings the backticks back.
///
/// @param cursor A cursor right after the typed backtick; on success it stays after the code,
///     with a character format that types plain text again.
/// @return true when the shortcut was applied.
bool applyMarkdownCode(QTextCursor &cursor);

/// Leaves inline code at its right edge (Right arrow): when text typed at the
/// cursor would continue the code and the code ends there, the cursor stays and types plain
/// text instead.
///
/// @param cursor A cursor without a selection; on success its character format drops the code.
/// @return false when the cursor is not at the right edge of inline code, or has already left
///     it, so the key moves the cursor as usual.
bool leaveInlineCode(QTextCursor &cursor);

/// Encloses the selection in the pair a typed character opens: `(`, `[`, `{`,
/// `"`, `'` and `«`, as one undo step, keeping the inner text selected. A backtick makes a
/// selection within one line inline code, as the backtick shortcut would, and encloses it in
/// backticks in a code block or across lines.
///
/// @param cursor A cursor with a selection; on success it selects the enclosed text.
/// @param typed The typed character.
/// @return false when @p typed opens no pair or the selection spans table cells, so the
///     character replaces the selection as usual.
bool wrapSelection(QTextCursor &cursor, QChar typed);

/// Returns the frame format of a code block; its top margin leaves room for
/// the header with the title, the language and the copy button.
///
/// @param language The lower-case syntax definition name, empty for plain text.
/// @param title The block's title, empty for none.
QTextFrameFormat codeBlockFormat(const QString &language, const QString &title);

/// Returns the format of a table: its width shared by the columns, equally
/// unless widths are given. Its lines are drawn by TextEdit, so they follow the theme.
///
/// @param columns The number of columns.
/// @param headerRows The number of header rows, 0 or 1.
/// @param widths The columns' widths in percent; empty or of another count for equal widths.
/// @param width The table's width in percent of the text column, or 0 for full width.
QTextTableFormat tableFormat(int columns, int headerRows, const QList<qreal> &widths = {},
                             int width = 0);

/// Returns a table's width in percent of the text column: 10 to 99, or 0 for full width.
int tableWidth(const QTextTableFormat &format);

/// Changes a table's width as one undo step; its columns keep their proportions.
///
/// @param table The table.
/// @param width The width in percent of the text column, at least MinWidth; 100 or more for
/// full width.
void setTableWidth(QTextTable &table, int width);

/// The narrowest a table column gets, in percent of the table.
inline constexpr int MinColumnWidth = 5;

/// Returns the widths of a table's columns in percent of the table; they add up to 100.
QList<qreal> columnWidths(const QTextTableFormat &format, int columns);

/// Tells whether a table's columns share its width equally, which the note file does not spell
/// out.
bool hasEqualColumns(const QTextTableFormat &format, int columns);

/// Sets the column widths of a table format: whole percents adding up to 100, each at least
/// MinColumnWidth.
///
/// @param format The format to change.
/// @param columns The number of columns.
/// @param widths The wanted widths in any scale; empty or of another count for equal widths.
void setColumnWidths(QTextTableFormat &format, int columns, QList<qreal> widths);

/// Changes the widths of a table's columns as one undo step.
///
/// @param table The table.
/// @param widths The wanted widths, one per column; empty for equal widths.
void resizeColumns(QTextTable &table, const QList<qreal> &widths);

/// Returns column widths with one column set to @p width percent and the others scaled to fill
/// the rest in their proportions, for Column width….
QList<qreal> withColumnWidth(QList<qreal> widths, int column, qreal width);

/// Returns the height of a code block's header, drawn in the frame's top margin.
int codeHeaderHeight();

/// Returns the width of the line numbers' column, the left margin of a code block: three digits
/// of the code font and the header's padding before them. The numbers of longer blocks reach
/// into the block's padding.
int codeGutterWidth();

/// Returns whether a block is an empty plain paragraph: no text, list, heading, quote or rule.
bool isEmptyParagraph(const QTextBlock &block);

/// Returns the code block a document block belongs to, or nullptr outside code blocks.
QTextFrame *codeBlockOf(const QTextBlock &block);

/// Returns the text of a code block: its lines joined with line feeds.
QString codeText(const QTextFrame &frame);

/// Returns whether insertCodeBlock() applies at the cursor: not in a code block or a table, and
/// with no code block or table among the selected lines.
bool canInsertCodeBlock(const QTextCursor &cursor);

/// Applies the toolbar's code command: a partial logical line becomes inline code; complete
/// lines, multiline selections and an empty selection use insertCodeBlock(). Logical lines
/// end at paragraph or Shift+Enter breaks, independently of visual wrapping.
///
/// @param cursor The selection; kept for inline code, moved into a new code block otherwise.
/// @return Whether formatting was applied; code blocks and tables are left alone.
bool formatCode(QTextCursor &cursor);

/// Inserts a code block after the cursor's paragraph, or in its place when the paragraph is
/// empty. Only the selected text moves into the block as plain text; unselected text
/// before and after it stays in separate paragraphs. Edge line breaks are not taken.
///
/// @param cursor The cursor; moved to the end of the code block's text.
/// @param language The language of the block, empty for plain text.
/// @return The new code block, or nullptr when canInsertCodeBlock() is false.
QTextFrame *insertCodeBlock(QTextCursor &cursor, const QString &language = {});

/// Inserts an empty table after the cursor's paragraph, or in its place when the paragraph is
/// empty, and moves the cursor into its first cell. Tables do not nest and
/// do not go into code blocks.
///
/// @param cursor The cursor; moved to the first cell.
/// @param rows The number of rows, from 1.
/// @param columns The number of columns, from 1.
/// @param header Whether the first row is a header row.
/// @return The new table, or nullptr when the cursor is in a table or a code block.
QTextTable *insertTable(QTextCursor &cursor, int rows, int columns, bool header);

/// The changes of a table's context menu.
enum class TableChange {
    RowAbove,
    RowBelow,
    ColumnLeft,
    ColumnRight,
    DeleteRow,
    DeleteColumn,
    ToggleHeader,
    DeleteTable
};

/// Changes the table at the cursor as one undo step; the columns keep sharing the width
/// equally. Deleting the last row or column deletes the table.
///
/// @param cursor A cursor in a table; moved to where editing continues.
/// @param change The change.
/// @return false when the cursor is not in a table.
bool changeTable(QTextCursor &cursor, TableChange change);

/// Returns the top-level frame, a table or a code block, that a block is in; nullptr for a
/// top-level paragraph.
QTextFrame *topFrameOf(const QTextBlock &block);

/// Returns where moveTable() puts a table next to a top-level paragraph or frame: the end of
/// the paragraph before it, or the start of the paragraph after it when a frame or the start
/// of the document comes before.
///
/// @param block A top-level paragraph, or a block of a top-level frame for the frame.
/// @param after Whether the table goes after the paragraph or the frame rather than before.
/// @return The place, a position for moveTable().
int tablePlace(const QTextBlock &block, bool after);

/// Returns where Move up (@p direction -1) or Move down (1) puts a table: past the paragraph or
/// the frame next to it. An empty paragraph that only separates the table from a frame or from
/// the document's start or end is passed together with what lies beyond it.
///
/// @return A place for moveTable(), or -1 when nothing lies in that direction.
int tableMovePlace(const QTextTable &table, int direction);

/// Moves a table to a place made by tablePlace() as one undo step. The
/// paragraphs around the table's old place stay apart unless one of them is an empty paragraph.
///
/// @param table The table; it is replaced by a copy at the new place.
/// @param position The place.
/// @return The moved table, or nullptr when @p position is not a place or is next to the table.
QTextTable *moveTable(QTextTable &table, int position);

/// Moves to the next (@p delta 1) or previous (-1) cell and selects its text; moving on from
/// the last cell adds a row (Tab).
///
/// @param cursor A cursor in a table; moved to the cell.
/// @return false when the cursor is not in a table.
bool moveToCell(QTextCursor &cursor, int delta);

/// Sets the language or the title of a code block (@p property CodeLanguage or CodeTitle) as
/// one undo step; an empty @p value clears it. The block is drawn again.
void setCodeBlockProperty(QTextFrame &frame, int property, const QString &value);

/// Moves the cursor to the paragraph after its code block (Ctrl+Enter); a paragraph with text
/// gets an empty one before it.
///
/// @return false when the cursor is not in a code block.
bool leaveCodeBlock(QTextCursor &cursor);

/// Replaces an empty code block with an empty paragraph (Backspace in an empty block).
///
/// @param cursor A cursor in the code block; moved to the paragraph.
/// @return false when the cursor is not in an empty code block.
bool removeEmptyCodeBlock(QTextCursor &cursor);

/// Indents (@p delta 1) or outdents (-1) code by four spaces: every selected line, or the
/// cursor's line when outdenting. Indenting without a selection inserts four spaces.
///
/// @return false when the cursor is not in a code block.
bool indentCode(QTextCursor cursor, int delta);

/// Replaces the selection with linked text, or links the selection when @p text is empty.
void setLink(QTextCursor cursor, const QString &href, const QString &text);

/// Removes the link around the cursor or in the selection, keeping the text.
void removeLink(QTextCursor cursor);

/// Selects the whole link the cursor is in, so it can be edited.
///
/// @return false when the cursor is not in a link.
bool selectLink(QTextCursor &cursor);

/// Sets or clears (with an empty @p value) a character property over the selection, or for
/// the text typed next. Used for colours, highlights and fonts.
void setCharProperty(QTextEdit &edit, int property, const QVariant &value);

} // namespace editor
