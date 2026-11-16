#pragma once

#include <QString>
#include <QStringView>

class QTextDocument;

namespace editor {

/// How readBody() treats its HTML.
enum class ReadMode {
    /// A note body in Jotter's vocabulary, read without loss.
    Note,
    /// HTML pasted from another program: structure and basic formatting
    /// are kept, including formatting given in `style` attributes; fonts, sizes and colours
    /// are dropped, links keep only web and mail addresses, and pasted images become pending
    /// objects (editor::PendingSource) to be stored as attachments.
    Paste,
    /// HTML converted from pasted Markdown (editor::readMarkdown()): read like pasted HTML, but
    /// its text keeps its white space, which Markdown has already resolved.
    Markdown,
};

/// Fills a document from HTML.
///
/// Elements outside the vocabulary are read as their text, so nothing typed is lost; the
/// contents of `head`, `style`, `script` and `title` are skipped. In a note body line breaks in
/// the source are layout and are dropped; `<br>` is a line break inside a block.
///
/// @param document The document to replace.
/// @param html The HTML.
/// @param mode Whether the HTML is a note body or pasted from elsewhere.
void readBody(QTextDocument &document, QStringView html, ReadMode mode = ReadMode::Note);

/// Writes a document as a note body in Jotter's HTML vocabulary: one block per line, lists
/// nested inside their parent items, table rows on lines of their own and inline formatting as
/// nested elements in a fixed order, so equal documents give equal text.
///
/// @param document The document to write.
/// @return The body HTML; reading it back gives the same document.
QString writeBody(const QTextDocument &document);

} // namespace editor
