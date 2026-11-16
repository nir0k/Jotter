#pragma once

#include <QString>
#include <QStringView>

namespace core {

/// Extracts the readable text of a note body: tags are dropped, block elements and line breaks
/// become line breaks, character references are decoded and runs of spaces collapse.
///
/// The body uses Jotter's restricted vocabulary, so a small scanner is enough; it runs on worker
/// threads, where the text classes of Qt Gui are not available.
///
/// @param html The body HTML.
/// @return The text, with blocks separated by single line breaks.
QString plainTextFromHtml(QStringView html);

/// Shortens text to a preview of at most @p length characters on one line, cutting at a word
/// boundary when one is near and marking the cut with an ellipsis.
QString snippet(const QString &text, qsizetype length);

} // namespace core
