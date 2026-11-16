#pragma once

#include <QString>

class QTextDocument;

namespace editor {

/// Parses GitHub Markdown into the note model without interpreting embedded HTML.
///
/// @param document The document to replace when Markdown formatting is found.
/// @param text The clipboard text; ordinary text is left for literal insertion.
/// @param requireExplicitSyntax Whether bare automatic links alone must leave HTML intact.
/// @return Whether the text contains recognised Markdown formatting.
bool readMarkdown(QTextDocument &document, const QString &text, bool requireExplicitSyntax = false);

/// Writes note content as GitHub Markdown, simplifying colours, fonts and other rich styling.
/// Images and attachments keep their library-relative addresses; their files are not copied.
///
/// @param document The selected content, independent of the live note.
/// @return Markdown ready for a text-only clipboard.
QString writeMarkdown(const QTextDocument &document);

} // namespace editor
