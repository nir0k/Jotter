#pragma once

#include <QTextFormat>

/// Properties of the editor's document model beyond Qt's own.
///
/// The document holds meaning only: emphasis, a colour key, a link, inline code, a heading
/// level, a quote, a checklist, a numbering style, an image's width and wrapping. How those look in
/// the current theme (heading sizes, link and colour shades, tag chips, struck-through checked
/// items, list numbers, table lines, images and chips) is drawn by editor::Presenter,
/// editor::TextEdit and editor::ObjectHandler and never reaches the note file.
namespace editor {

enum Property : int {
    /// Character property: the tag colour key of the text colour.
    TextColorKey = QTextFormat::UserProperty + 1,
    /// Character property: the tag colour key of the highlight.
    HighlightKey,
    /// Character property: true for inline code.
    InlineCode,
    /// List property: true for a checklist; its items carry checked or unchecked markers.
    Checklist,
    /// Frame property: true for a code block, a frame of plain text lines.
    CodeBlock,
    /// Frame property: the language of a code block, the lower-case name of a syntax
    /// definition; empty for plain text.
    CodeLanguage,
    /// Frame property: the title of a code block; empty for none.
    CodeTitle,
    /// List property: true for a numbered list. Its markers are drawn by the editor, as Qt cannot
    /// draw every numbering style.
    Numbered,
    /// List property: the numbering style of a numbered list; absent for `1.`.
    NumberingStyleId,
    /// Object property: the note whose attachment folder holds the file of an image or an
    /// attachment chip.
    ObjectNote,
    /// Object property: the file name of an image or an attachment chip.
    ObjectFile,
    /// Image property: the width in percent of the text column (25, 50 or 75); absent for full
    /// width.
    ImageWidth,
    /// Attachment property: the file size in bytes.
    AttachmentSize,
    /// Attachment property: the name the chip shows.
    AttachmentName,
    /// Object property of pasted content not stored yet: a local file path, or a `data:` URL.
    PendingSource,
    /// Image property: how text flows around the image, an ImageWrap value; absent in line with
    /// text.
    ImageWrapMode,
};

/// How text flows around an image.
enum class ImageWrap : int {
    /// The image stands on its line like a character.
    Inline,
    /// The image floats at the left of the text column or of its table column, and text flows
    /// along its right side.
    Square,
    /// The image takes a line of its own; text continues above and below it.
    TopAndBottom,
};

/// The object types of images and attachment chips, characters drawn by editor::ObjectHandler.
enum ObjectType : int {
    ImageObject = QTextFormat::UserObject + 1,
    AttachmentObject,
};

/// The narrowest an image or a table is made, in percent of its column; the widest is full
/// width, stored as no width.
inline constexpr int MinWidth = 10;

/// Returns the width of an image or a table read from a note: 10 to 99 percent, or 0 for full
/// width.
inline int validWidth(int width)
{
    return width >= MinWidth && width < 100 ? width : 0;
}

/// The block kinds of the paragraph style control.
enum class BlockStyle { Paragraph, Heading1, Heading2, Heading3, Quote };

/// Marks a block as a quote, or clears the mark, together with the left margin its bar is
/// drawn in. The margin is only how a quote looks; the note file stores the quote alone.
void setQuote(QTextBlockFormat &format, bool quote);

} // namespace editor
