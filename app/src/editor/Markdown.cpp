#include "editor/Markdown.h"

#include "core/Html.h"
#include "core/NoteLinks.h"
#include "editor/HtmlCodec.h"
#include "editor/Syntax.h"

#include <QTextDocument>
#include <QTextDocumentFragment>

#include <algorithm>
#include <md4c.h>
#include <string_view>

namespace editor {

namespace {

/// The md4c extensions of GitHub Markdown that notes hold, with embedded HTML read as text.
/// They are named one by one because newer md4c releases add extensions to
/// MD_DIALECT_GITHUB, such as footnotes, that notes have no place for.
constexpr unsigned GitHubFlags = MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_TABLES |
                                 MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_NOHTML;

/// Returns the text of a UTF-8 chunk reported by md4c.
QString textOf(const MD_CHAR *text, MD_SIZE size)
{
    return QString::fromUtf8(text, static_cast<qsizetype>(size));
}

/// Decodes an HTML entity md4c reports, such as `&copy;` or `&#8212;`, with Qt's entity
/// table; one Qt does not know stays as typed.
QString decodeEntity(const QString &entity)
{
    const QString decoded = QTextDocumentFragment::fromHtml(entity).toPlainText();
    return decoded.isEmpty() ? entity : decoded;
}

/// Returns the decoded value of an md4c attribute, such as a link destination.
QString attributeOf(const MD_ATTRIBUTE &attribute)
{
    // md4c leaves the attributes of an indented code block zeroed, without substrings.
    if (attribute.size == 0) {
        return {};
    }
    QString value;
    // The offsets end with one equal to the size, after the last substring.
    for (std::size_t i = 0; attribute.substr_offsets[i] < attribute.size; ++i) {
        const MD_OFFSET begin = attribute.substr_offsets[i];
        const QString part =
            textOf(attribute.text + begin, attribute.substr_offsets[i + 1] - begin);
        switch (attribute.substr_types[i]) {
        case MD_TEXT_NULLCHAR:
            value += QChar(QChar::ReplacementCharacter);
            break;
        case MD_TEXT_ENTITY:
            value += decodeEntity(part);
            break;
        default:
            value += part;
            break;
        }
    }
    return value;
}

/// Returns whether a heading is underlined with `-` or `=` (setext) rather than opened with
/// `#` (ATX). md4c reports both kinds alike, so the source decides: the text of an ATX
/// heading follows a `#` run once spaces, escapes and the opening delimiters of emphasis,
/// code, links and images before it are skipped.
///
/// @param begin The start of the parsed source.
/// @param text Where the heading's first text chunk starts in the source.
/// @return Whether the heading is underlined.
bool isUnderlined(const MD_CHAR *begin, const MD_CHAR *text)
{
    static constexpr std::string_view skipped = " \t\\*_~`[!";
    while (text != begin && skipped.contains(text[-1])) {
        --text;
    }
    return text == begin || text[-1] != '#';
}

/// Converts Markdown into HTML in the note vocabulary, which readBody()
/// turns into the note model, and notes whether the text has Markdown formatting at all.
///
/// md4c reports the parsed structure; the conversion keeps what notes hold and simplifies
/// the rest: headings deeper than three become level three, and an item's later paragraphs
/// join it after a line break, as a list item is one paragraph in a note. Every line break
/// is kept, and a line of `-` or `=` under text is a divider below it rather than a heading
/// underline, as plain text uses such lines to separate parts.
class MarkdownToHtml
{
public:
    /// The HTML of the converted text.
    QString html;
    /// Whether the text has block structure, emphasis, code or images.
    bool formatted = false;
    /// Whether the text has a link to a web or mail address.
    bool linked = false;

    /// Parses Markdown with the given md4c flags.
    ///
    /// @param markdown The text.
    /// @param flags The md4c extensions to recognise.
    /// @return Whether md4c parsed the text; it fails only when out of memory.
    bool convert(const QString &markdown, unsigned flags)
    {
        const QByteArray utf8 = markdown.toUtf8();
        m_source = utf8.constData();
        m_sourceEnd = m_source + utf8.size();
        const MD_PARSER parser{
            .abi_version = 0,
            .flags = flags,
            .enter_block = &enterBlock,
            .leave_block = &leaveBlock,
            .enter_span = &enterSpan,
            .leave_span = &leaveSpan,
            .text = &text,
            .debug_log = nullptr,
            .syntax = nullptr,
        };
        return md_parse(utf8.constData(), static_cast<MD_SIZE>(utf8.size()), &parser, this) == 0;
    }

private:
    /// A list being converted. A bulleted list is written at its first item, which tells
    /// whether it is a checklist.
    struct List
    {
        bool ordered = false;
        bool checklist = false;
        bool written = false;
    };

    /// Returns the converter the md4c callbacks were given.
    static MarkdownToHtml &self(void *userdata) { return *static_cast<MarkdownToHtml *>(userdata); }

    // The md4c callbacks forward to the converter; returning 0 continues parsing.

    /// Forwards the start of a block.
    static int enterBlock(MD_BLOCKTYPE type, void *detail, void *userdata)
    {
        self(userdata).enter(type, detail);
        return 0;
    }

    /// Forwards the end of a block.
    static int leaveBlock(MD_BLOCKTYPE type, void * /*detail*/, void *userdata)
    {
        self(userdata).leave(type);
        return 0;
    }

    /// Forwards the start of an inline span.
    static int enterSpan(MD_SPANTYPE type, void *detail, void *userdata)
    {
        self(userdata).enterSpan(type, detail);
        return 0;
    }

    /// Forwards the end of an inline span.
    static int leaveSpan(MD_SPANTYPE type, void * /*detail*/, void *userdata)
    {
        self(userdata).leaveSpan(type);
        return 0;
    }

    /// Forwards a chunk of text.
    static int text(MD_TEXTTYPE type, const MD_CHAR *chunk, MD_SIZE size, void *userdata)
    {
        self(userdata).addText(type, chunk, textOf(chunk, size));
        return 0;
    }

    /// The block that holds the current one, or MD_BLOCK_DOC at the top.
    [[nodiscard]] MD_BLOCKTYPE parent() const
    {
        return m_blocks.isEmpty() ? MD_BLOCK_DOC : m_blocks.last();
    }

    /// Opens the element of a block; a code block collects its text until it ends.
    void enter(MD_BLOCKTYPE type, void *detail)
    {
        const MD_BLOCKTYPE container = parent();
        m_blocks.append(type);
        switch (type) {
        case MD_BLOCK_QUOTE:
            formatted = true;
            html += QStringLiteral("<blockquote>");
            break;
        case MD_BLOCK_UL:
            formatted = true;
            m_lists.append(List{});
            break;
        case MD_BLOCK_OL: {
            formatted = true;
            const unsigned start = static_cast<const MD_BLOCK_OL_DETAIL *>(detail)->start;
            html += start == 1 ? QStringLiteral("<ol>")
                               : QStringLiteral("<ol start=\"%1\">").arg(start);
            m_lists.append(List{.ordered = true, .checklist = false, .written = true});
            break;
        }
        case MD_BLOCK_LI:
            enterItem(*static_cast<const MD_BLOCK_LI_DETAIL *>(detail));
            break;
        case MD_BLOCK_HR:
            formatted = true;
            html += QStringLiteral("<hr>");
            break;
        case MD_BLOCK_H:
            formatted = true;
            m_headingAt = html.size();
            m_headingText = nullptr;
            html += QStringLiteral("<h%1>").arg(headingLevel(detail));
            break;
        case MD_BLOCK_CODE: {
            formatted = true;
            m_codeLanguage =
                languageKey(attributeOf(static_cast<const MD_BLOCK_CODE_DETAIL *>(detail)->lang));
            m_code.clear();
            break;
        }
        case MD_BLOCK_P:
            if (container == MD_BLOCK_LI) {
                if (std::exchange(m_itemText.last(), true)) {
                    html += QStringLiteral("<br>");
                }
            } else {
                html += QStringLiteral("<p>");
            }
            break;
        case MD_BLOCK_TABLE:
            formatted = true;
            html += QStringLiteral("<table>");
            break;
        case MD_BLOCK_THEAD:
            html += QStringLiteral("<thead>");
            break;
        case MD_BLOCK_TBODY:
            html += QStringLiteral("<tbody>");
            break;
        case MD_BLOCK_TR:
            html += QStringLiteral("<tr>");
            break;
        case MD_BLOCK_TH:
            html += QStringLiteral("<th><p>");
            break;
        case MD_BLOCK_TD:
            html += QStringLiteral("<td><p>");
            break;
        default:
            break;
        }
    }

    /// Closes the element of a block and writes a finished code block.
    void leave(MD_BLOCKTYPE type)
    {
        m_blocks.removeLast();
        switch (type) {
        case MD_BLOCK_QUOTE:
            html += QStringLiteral("</blockquote>");
            break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            if (m_lists.last().written) {
                html += m_lists.last().ordered ? QStringLiteral("</ol>") : QStringLiteral("</ul>");
            }
            m_lists.removeLast();
            break;
        case MD_BLOCK_LI:
            html += QStringLiteral("</li>");
            m_itemText.removeLast();
            break;
        case MD_BLOCK_H:
            // A list item is one paragraph and holds no divider, so its headings stay.
            if (m_headingText != nullptr && parent() != MD_BLOCK_LI &&
                isUnderlined(m_source, m_headingText)) {
                html.replace(m_headingAt, QStringLiteral("<h%1>").arg(m_headingLevel).size(),
                             QStringLiteral("<p>"));
                html += QStringLiteral("</p><hr>");
            } else {
                html += QStringLiteral("</h%1>").arg(m_headingLevel);
            }
            break;
        case MD_BLOCK_CODE:
            // Every code line ends with a line feed, the last one included.
            if (m_code.endsWith(u'\n')) {
                m_code.chop(1);
            }
            html += m_codeLanguage.isEmpty()
                        ? QStringLiteral("<pre>")
                        : QStringLiteral("<pre data-language=\"%1\">")
                              .arg(core::html::escapeAttribute(m_codeLanguage));
            html += QStringLiteral("<code>") + core::html::escapeText(m_code) +
                    QStringLiteral("</code></pre>");
            m_code.clear();
            break;
        case MD_BLOCK_P:
            if (parent() != MD_BLOCK_LI) {
                html += QStringLiteral("</p>");
            }
            break;
        case MD_BLOCK_TABLE:
            html += QStringLiteral("</table>");
            break;
        case MD_BLOCK_THEAD:
            html += QStringLiteral("</thead>");
            break;
        case MD_BLOCK_TBODY:
            html += QStringLiteral("</tbody>");
            break;
        case MD_BLOCK_TR:
            html += QStringLiteral("</tr>");
            break;
        case MD_BLOCK_TH:
            html += QStringLiteral("</p></th>");
            break;
        case MD_BLOCK_TD:
            html += QStringLiteral("</p></td>");
            break;
        default:
            break;
        }
    }

    /// Returns the note heading level of an md4c heading and remembers it for its end tag.
    int headingLevel(void *detail)
    {
        const auto level = static_cast<int>(static_cast<const MD_BLOCK_H_DETAIL *>(detail)->level);
        m_headingLevel = std::clamp(level, 1, 3);
        return m_headingLevel;
    }

    /// Starts a list item. A bulleted list whose first item is a task is a checklist; its
    /// other items are unchecked tasks, and task marks in other lists are dropped.
    void enterItem(const MD_BLOCK_LI_DETAIL &detail)
    {
        List &list = m_lists.last();
        if (!list.written) {
            list.checklist = detail.is_task != 0;
            list.written = true;
            html += list.checklist ? QStringLiteral("<ul class=\"checklist\">")
                                   : QStringLiteral("<ul>");
        }
        if (list.checklist) {
            const bool checked =
                detail.is_task != 0 && (detail.task_mark == 'x' || detail.task_mark == 'X');
            html += checked ? QStringLiteral("<li data-checked=\"true\">")
                            : QStringLiteral("<li data-checked=\"false\">");
        } else {
            html += QStringLiteral("<li>");
        }
        m_itemText.append(false);
    }

    /// Opens the element of an inline span. A link to an address notes do not accept opens
    /// nothing, and an image collects its description until it ends.
    void enterSpan(MD_SPANTYPE type, void *detail)
    {
        if (m_image > 0) {
            m_image += type == MD_SPAN_IMG ? 1 : 0;
            return; // An image's description is plain text.
        }
        QString open;
        QString close;
        switch (type) {
        case MD_SPAN_EM:
            open = QStringLiteral("<em>");
            close = QStringLiteral("</em>");
            break;
        case MD_SPAN_STRONG:
            open = QStringLiteral("<strong>");
            close = QStringLiteral("</strong>");
            break;
        case MD_SPAN_DEL:
            open = QStringLiteral("<s>");
            close = QStringLiteral("</s>");
            break;
        case MD_SPAN_CODE:
            open = QStringLiteral("<code>");
            close = QStringLiteral("</code>");
            break;
        case MD_SPAN_A:
            // Links keep only web and mail addresses; others become text.
            if (const QString href = core::normaliseLink(
                    attributeOf(static_cast<const MD_SPAN_A_DETAIL *>(detail)->href));
                !href.isEmpty()) {
                linked = true;
                open = QStringLiteral("<a href=\"%1\">").arg(core::html::escapeAttribute(href));
                close = QStringLiteral("</a>");
            }
            break;
        case MD_SPAN_IMG:
            formatted = true;
            m_image = 1;
            m_imageSource = attributeOf(static_cast<const MD_SPAN_IMG_DETAIL *>(detail)->src);
            m_imageText.clear();
            return;
        default:
            break;
        }
        formatted = formatted || type != MD_SPAN_A;
        html += open;
        m_spanEnds.append(close);
    }

    /// Closes the element of an inline span, writing an image once its description ends.
    void leaveSpan(MD_SPANTYPE type)
    {
        if (m_image > 0) {
            m_image -= type == MD_SPAN_IMG ? 1 : 0;
            if (m_image == 0) {
                html += QStringLiteral("<img src=\"%1\" alt=\"%2\">")
                            .arg(core::html::escapeAttribute(m_imageSource),
                                 core::html::escapeAttribute(m_imageText));
            }
            return;
        }
        html += m_spanEnds.takeLast();
    }

    /// Adds text to the current code block, image description or inline content. A line
    /// break of either kind becomes a line break, or a space in an image description.
    ///
    /// @param type The md4c kind of the text.
    /// @param source Where md4c reports the chunk; mostly within the parsed source.
    /// @param chunk The text.
    void addText(MD_TEXTTYPE type, const MD_CHAR *source, QString chunk)
    {
        if (m_headingText == nullptr && parent() == MD_BLOCK_H && source >= m_source &&
            source < m_sourceEnd) {
            m_headingText = source;
        }
        if (!m_blocks.isEmpty() && m_blocks.last() == MD_BLOCK_CODE) {
            m_code += chunk;
            return;
        }
        switch (type) {
        case MD_TEXT_NULLCHAR:
            chunk = QChar(QChar::ReplacementCharacter);
            break;
        case MD_TEXT_ENTITY:
            chunk = decodeEntity(chunk);
            break;
        case MD_TEXT_CODE:
            // GitHub escapes a pipe in a table's inline code, where it would end the cell.
            if (parent() == MD_BLOCK_TD || parent() == MD_BLOCK_TH) {
                chunk.replace(QStringLiteral("\\|"), QStringLiteral("|"));
            }
            break;
        default:
            break;
        }
        if (m_image > 0) {
            m_imageText +=
                type == MD_TEXT_BR || type == MD_TEXT_SOFTBR ? QStringLiteral(" ") : chunk;
            return;
        }
        if (!m_itemText.isEmpty() && parent() == MD_BLOCK_LI) {
            m_itemText.last() = true;
        }
        html += type == MD_TEXT_BR || type == MD_TEXT_SOFTBR ? QStringLiteral("<br>")
                                                             : core::html::escapeText(chunk);
    }

    QList<MD_BLOCKTYPE> m_blocks;
    QList<List> m_lists;
    QList<bool> m_itemText; ///< Per open item: whether it has text yet.
    QStringList m_spanEnds;
    int m_headingLevel = 1;
    qsizetype m_headingAt = 0;              ///< Where the open heading's tag starts in html.
    const MD_CHAR *m_headingText = nullptr; ///< Where its first text starts in the source.
    const MD_CHAR *m_source = nullptr;
    const MD_CHAR *m_sourceEnd = nullptr;
    QString m_code;
    QString m_codeLanguage;
    int m_image = 0; ///< The depth of images; md4c nests images in descriptions.
    QString m_imageSource;
    QString m_imageText;
};

} // namespace

bool readMarkdown(QTextDocument &document, const QString &text, bool requireExplicitSyntax)
{
    MarkdownToHtml converter;
    if (!converter.convert(text, GitHubFlags)) {
        return false;
    }
    bool recognised = converter.formatted;
    if (!recognised && converter.linked) {
        // CommonMark accepts explicit links but not GitHub's bare URLs.
        MarkdownToHtml commonMark;
        recognised = !requireExplicitSyntax ||
                     (commonMark.convert(text, MD_FLAG_NOHTML) && commonMark.linked);
    }
    if (!recognised) {
        return false;
    }
    readBody(document, converter.html, ReadMode::Markdown);
    return true;
}

} // namespace editor
