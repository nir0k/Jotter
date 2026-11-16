#pragma once

#include <QList>
#include <QString>
#include <QStringView>

#include <utility>

namespace core::html {

/// One piece of a note body: a start tag, an end tag or decoded text.
struct Token
{
    enum class Kind { Start, End, Text };

    Kind kind = Kind::Text;
    /// The element name in lower case, for tags.
    QString name;
    /// Attributes of a start tag with decoded values, in their order.
    QList<std::pair<QString, QString>> attributes;
    /// Decoded text, for text tokens.
    QString text;

    /// Returns the value of an attribute, or an empty string when the tag has none.
    QString attribute(QStringView attributeName) const;

    /// Tells whether a start tag has an attribute, even an empty one.
    bool hasAttribute(QStringView attributeName) const;
};

/// Splits note body HTML into tokens.
///
/// The body uses Jotter's restricted vocabulary, so a small tolerant scanner
/// is enough: comments and declarations are skipped, character references are decoded, and
/// a stray `<` that does not start a tag is kept as text. It runs on worker threads, where the
/// text classes of Qt Gui are not available.
///
/// @param html The body HTML.
/// @return The tokens in document order.
QList<Token> tokenize(QStringView html);

/// Returns the first unsupported element, attribute or structural form, or an empty string.
/// Unknown identifiers within supported data attributes remain valid and round-trip unchanged.
QString bodyProblem(QStringView body);

/// Tells whether an element separates blocks of text (paragraphs, headings, list items, cells).
bool isBlockElement(QStringView name);

/// Escapes text for element content: `&`, `<` and `>`.
QString escapeText(QStringView text);

/// Escapes text for a double-quoted attribute value: `&`, `<`, `>` and `"`.
QString escapeAttribute(QStringView text);

} // namespace core::html
