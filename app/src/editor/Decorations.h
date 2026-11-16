#pragma once

#include "editor/Syntax.h"
#include "ui/Palette.h"

#include <QHash>
#include <QList>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QString>

class QPainter;
class QTextBlock;
class QTextDocument;
class QTextFrame;
class QTextTable;

/// What the document format cannot draw and the editor draws around the text: code block boxes
/// and headers, table lines, quote bars and list numbers. The editor paints them on its viewport
/// and an export on its pages, so both look the same; nothing here reads the current theme's
/// colours, so an export can paint on a worker thread.
///
/// Positions are the document layout's, moved by an offset: the negated scroll position in the
/// editor, the page's place in an export.
namespace editor {

/// A top-level table and the places of its lines.
struct TableGeometry
{
    QTextTable *table = nullptr;
    QRectF box;
    /// The left edges of the columns after the first.
    QList<qreal> columnLines;
    /// The top edges of the rows after the first.
    QList<qreal> rowLines;
};

/// Returns the top-level tables that reach into a range of heights, top to bottom.
///
/// @param document The laid-out document.
/// @param offset Added to layout positions.
/// @param bottom The lowest height of interest; tables starting below it are left out, as are
///        those ending above 0.
/// @return The tables with their lines.
QList<TableGeometry> tableGeometries(const QTextDocument &document, QPointF offset, qreal bottom);

/// Draws tables' header backgrounds and lines, under the text.
void paintTables(QPainter &painter, const QList<TableGeometry> &tables, const ui::Roles &roles);

/// A top-level code block: its box, drawn under the text, and the parts of its header row.
struct CodeHeader
{
    QTextFrame *frame = nullptr;
    QRect box;
    QRect title;
    QRect language;
    /// The Copy button, at the right end of the header.
    QRect copy;
    /// The language's name as shown.
    QString languageText;
};

/// Returns the top-level code blocks that reach into a range of heights, top to bottom.
///
/// @param document The laid-out document.
/// @param offset Added to layout positions.
/// @param bottom The lowest height of interest; blocks starting below it are left out, as are
///        those ending above 0.
/// @param repository The syntax definitions that name the languages.
/// @return The blocks with their header parts.
QList<CodeHeader> codeHeaders(const QTextDocument &document, QPointF offset, qreal bottom,
                              const KSyntaxHighlighting::Repository &repository);

/// Draws code blocks' boxes, under the text.
void paintCodeBackgrounds(QPainter &painter, const QList<CodeHeader> &headers,
                          const ui::Roles &roles);

/// Draws the text of a code block's header: the language, leaving room for a chevron after it,
/// and the title when the block has one.
///
/// @param painter The painter.
/// @param header The block's header.
/// @param roles The colours.
/// @param drawTitle false while a title field covers the title.
void paintCodeLabels(QPainter &painter, const CodeHeader &header, const ui::Roles &roles,
                     bool drawTitle);

/// Draws the line numbers of code blocks in the column at their left, one per line of code on
/// its first visual line, so wrapped lines have none. They are drawn, not text, so they are never
/// selected, copied or saved.
///
/// @param painter The painter.
/// @param headers The code blocks, from codeHeaders().
/// @param roles The colours.
/// @param offset Added to layout positions.
/// @param bottom The height below which nothing is drawn.
void paintCodeLineNumbers(QPainter &painter, const QList<CodeHeader> &headers,
                          const ui::Roles &roles, QPointF offset, qreal bottom);

/// Draws the rounded boxes of inline code under the text, one per line the code runs on, a
/// little wider than the code; the document format draws only rectangles.
///
/// @param painter The painter.
/// @param first The first block to draw.
/// @param roles The colours.
/// @param offset Added to layout positions.
/// @param bottom The height below which nothing is drawn.
void paintInlineCode(QPainter &painter, const QTextBlock &first, const ui::Roles &roles,
                     QPointF offset, qreal bottom);

/// Draws the quote bars and the markers of numbered list items, after the text; the document
/// format can draw neither.
///
/// @param painter The painter.
/// @param first The first block to draw.
/// @param markers The markers by block number, from listMarkers().
/// @param roles The colours.
/// @param offset Added to layout positions.
/// @param bottom The height below which nothing is drawn.
void paintBlockDecorations(QPainter &painter, const QTextBlock &first,
                           const QHash<int, QString> &markers, const ui::Roles &roles,
                           QPointF offset, qreal bottom);

} // namespace editor
