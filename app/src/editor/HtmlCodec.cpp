#include "editor/HtmlCodec.h"

#include "core/Attachments.h"
#include "core/Html.h"
#include "core/NoteLinks.h"
#include "core/Numbering.h"
#include "core/TagColors.h"
#include "editor/Commands.h"
#include "editor/Formats.h"
#include "editor/Objects.h"

#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>

#include <ranges>

namespace editor {

using namespace Qt::StringLiterals;

namespace {

using core::html::Token;

bool is(const QString &name, std::initializer_list<const char *> names)
{
    return std::ranges::any_of(
        names, [&](const char *candidate) { return name == QLatin1StringView(candidate); });
}

/// Reads the family from a `font-family: 'Name'` style.
QString fontFamilyFromStyle(const QString &style)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"re(font-family\s*:\s*(?:'([^']*)'|"([^"]*)"|([^;]+)))re"));
    const QRegularExpressionMatch match = pattern.match(style);
    if (!match.hasMatch()) {
        return {};
    }
    for (int i = 1; i <= 3; ++i) {
        if (!match.captured(i).isEmpty()) {
            return match.captured(i).trimmed();
        }
    }
    return {};
}

/// Applies the basic formatting that pasted HTML gives in `style` attributes, as word
/// processors and web editors do (`<span style="font-weight:700">`). A declaration can also
/// undo its element: Google Docs wraps everything in `<b style="font-weight:normal">`.
void applyPastedStyle(const QString &style, QTextCharFormat &format)
{
    for (const QString &declaration : style.split(u';', Qt::SkipEmptyParts)) {
        const qsizetype colon = declaration.indexOf(u':');
        if (colon < 0) {
            continue;
        }
        const QString property = declaration.left(colon).trimmed().toLower();
        const QString value = declaration.mid(colon + 1).trimmed().toLower();
        if (property == QLatin1StringView("font-weight")) {
            const int weight = value.toInt();
            const bool bold = value == QLatin1StringView("bold") ||
                              value == QLatin1StringView("bolder") || weight >= 600;
            format.setFontWeight(bold ? QFont::Bold : QFont::Normal);
        } else if (property == QLatin1StringView("font-style")) {
            format.setFontItalic(value != QLatin1StringView("normal"));
        } else if (property == QLatin1StringView("text-decoration") ||
                   property == QLatin1StringView("text-decoration-line")) {
            format.setFontUnderline(value.contains(QLatin1StringView("underline")));
            format.setFontStrikeOut(value.contains(QLatin1StringView("line-through")));
        } else if (property == QLatin1StringView("vertical-align")) {
            QTextCharFormat::VerticalAlignment alignment = QTextCharFormat::AlignNormal;
            if (value == QLatin1StringView("super")) {
                alignment = QTextCharFormat::AlignSuperScript;
            } else if (value == QLatin1StringView("sub")) {
                alignment = QTextCharFormat::AlignSubScript;
            }
            format.setVerticalAlignment(alignment);
        }
    }
}

/// Applies an inline element to the character format it opens.
void applyInline(const Token &token, QTextCharFormat &format, ReadMode mode)
{
    const QString &name = token.name;
    if (is(name, {"strong", "b"})) {
        format.setFontWeight(QFont::Bold);
    } else if (is(name, {"em", "i"})) {
        format.setFontItalic(true);
    } else if (name == QLatin1StringView("u")) {
        format.setFontUnderline(true);
    } else if (is(name, {"s", "del", "strike"})) {
        format.setFontStrikeOut(true);
    } else if (name == QLatin1StringView("sup")) {
        format.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
    } else if (name == QLatin1StringView("sub")) {
        format.setVerticalAlignment(QTextCharFormat::AlignSubScript);
    } else if (name == QLatin1StringView("code")) {
        format.setProperty(InlineCode, true);
    } else if (name == QLatin1StringView("a")) {
        // Links keep only the addresses a note accepts; others become text. A note file may come
        // from another program, so its links are checked as well as pasted ones.
        QString href = token.attribute(u"href");
        if (mode != ReadMode::Note) {
            href = href.contains(u':') ? core::normaliseLink(href) : QString();
        } else if (!core::noteLinkTarget(href) && !core::isWebLink(href)) {
            href.clear();
        }
        if (!href.isEmpty()) {
            format.setAnchor(true);
            format.setAnchorHref(href);
        }
    } else if (name == QLatin1StringView("mark") && mode == ReadMode::Note) {
        format.setProperty(HighlightKey, token.attribute(u"data-color"));
    } else if (name == QLatin1StringView("span") && mode == ReadMode::Note) {
        if (token.hasAttribute(u"data-color")) {
            format.setProperty(TextColorKey, token.attribute(u"data-color"));
        }
        const QString family = fontFamilyFromStyle(token.attribute(u"style"));
        if (!family.isEmpty()) {
            format.setFontFamilies({family});
        }
    }
    if (mode != ReadMode::Note && token.hasAttribute(u"style")) {
        applyPastedStyle(token.attribute(u"style"), format);
    }
}

bool isInlineElement(const QString &name)
{
    return is(name, {"strong", "b", "em", "i", "u", "s", "del", "strike", "sup", "sub", "code", "a",
                     "mark", "span"});
}

/// The size of a table and how many of its rows are header rows.
struct TableShape
{
    int rows = 0;
    int columns = 0;
    int headerRows = 0;
    /// The column widths in percent from `<col style="width: 30%">`, when the table has them.
    QList<qreal> widths;
    /// The table's width in percent of the text column, or 0 for full width.
    int width = 0;
};

/// Reads the percent of a `width: 30%` style; returns 0 for anything else.
qreal widthFromStyle(const QString &style)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"(width\s*:\s*([0-9]+(?:\.[0-9]+)?)\s*%)"));
    const QRegularExpressionMatch match = pattern.match(style);
    return match.hasMatch() ? match.captured(1).toDouble() : 0;
}

/// Measures the table that starts at @p start by looking ahead to its end tag; the cells of
/// tables nested in it count as text of their cell.
TableShape measureTable(const QList<Token> &tokens, qsizetype start, ReadMode mode)
{
    TableShape shape;
    // Pasted tables take the text column's width, like their columns share it equally.
    if (mode == ReadMode::Note) {
        shape.width = validWidth(tokens.at(start).attribute(u"data-width").toInt());
    }
    int depth = 0;
    int cells = 0;
    bool header = false;
    // Without a `<thead>`, a first row of `<th>` cells is the header, as pasted tables have it.
    bool firstRowHeads = false;
    for (qsizetype i = start; i < tokens.size(); ++i) {
        const Token &token = tokens.at(i);
        if (token.name == QLatin1StringView("table")) {
            depth += token.kind == Token::Kind::Start ? 1 : -1;
            if (depth == 0) {
                break;
            }
            continue;
        }
        if (depth != 1) {
            continue;
        }
        if (token.name == QLatin1StringView("thead")) {
            header = token.kind == Token::Kind::Start;
        } else if (token.kind == Token::Kind::Start && token.name == QLatin1StringView("col") &&
                   mode == ReadMode::Note) {
            // Pasted tables share the width equally; a note keeps the widths it was given.
            shape.widths.append(widthFromStyle(token.attribute(u"style")));
        } else if (token.kind == Token::Kind::Start && token.name == QLatin1StringView("tr")) {
            ++shape.rows;
            shape.headerRows += header && shape.headerRows == shape.rows - 1 ? 1 : 0;
            cells = 0;
        } else if (token.kind == Token::Kind::Start && is(token.name, {"td", "th"})) {
            shape.columns = std::max(shape.columns, ++cells);
            if (shape.rows == 1) {
                firstRowHeads =
                    (cells == 1 || firstRowHeads) && token.name == QLatin1StringView("th");
            }
        }
    }
    if (shape.headerRows == 0 && firstRowHeads) {
        shape.headerRows = 1;
    }
    return shape;
}

/// Builds documents block by block from tokens.
class Reader
{
public:
    Reader(QTextDocument &document, ReadMode mode)
        : m_cursor(&document)
        , m_mode(mode)
    {
        document.clear();
        m_formats.append(QTextCharFormat());
    }

    void read(QStringView html)
    {
        m_tokens = core::html::tokenize(html);
        for (m_index = 0; m_index < m_tokens.size(); ++m_index) {
            const Token &token = m_tokens.at(m_index);
            if (is(token.name, {"head", "style", "script", "title", "template"})) {
                m_skip = std::max(0, m_skip + (token.kind == Token::Kind::Start ? 1 : -1));
                continue;
            }
            if (m_skip > 0) {
                continue;
            }
            switch (token.kind) {
            case Token::Kind::Start:
                start(token);
                break;
            case Token::Kind::End:
                end(token);
                break;
            case Token::Kind::Text:
                text(token.text);
                break;
            }
        }
    }

private:
    struct ListContext
    {
        QTextListFormat format;
        QTextList *list = nullptr;
    };

    void start(const Token &token)
    {
        const QString &name = token.name;
        if (m_code != nullptr) {
            // A newline inside the canonical <code> is data, not layout after <pre>.
            if (name == QLatin1StringView("code")) {
                m_codeStart = false;
            }
            return; // A code block is plain text; elements inside it are dropped.
        }
        if (m_attachment) {
            return; // An attachment chip shows its name only.
        }
        if (name == QLatin1StringView("table")) {
            startTable();
            return;
        }
        if (m_table != nullptr && m_nestedTables > 0) {
            // The structure of a nested table is dropped; its cells become paragraphs.
            if (is(name, {"tr", "td", "th"})) {
                m_inBlock = false;
                return;
            }
        } else if (m_table != nullptr) {
            if (name == QLatin1StringView("tr")) {
                ++m_row;
                m_column = -1;
                return;
            }
            if (is(name, {"td", "th"})) {
                startCell();
                return;
            }
        }
        // Cells hold paragraphs, inline formatting and lists only.
        const bool inCell = m_table != nullptr;
        if (name == QLatin1StringView("pre") && !inCell) {
            // The frame goes after the current block; Qt adds an empty block after it, which the
            // next block reuses.
            m_code = m_cursor.insertFrame(codeBlockFormat(
                token.attribute(u"data-language").toLower(), token.attribute(u"data-title")));
            m_cursor.setBlockCharFormat(QTextCharFormat());
            m_reuse = false;
            m_codeStart = true;
        } else if (is(name, {"p", "div", "pre"})) {
            QTextBlockFormat block;
            if (m_quote) {
                setQuote(block, true);
            }
            beginBlock(block);
        } else if (name.size() == 2 && name.at(0) == u'h' && name.at(1) >= u'1' &&
                   name.at(1) <= u'6') {
            QTextBlockFormat block;
            if (!inCell) {
                block.setHeadingLevel(std::min(3, name.at(1).digitValue()));
            }
            beginBlock(block);
        } else if (name == QLatin1StringView("blockquote")) {
            m_quote = !inCell;
        } else if (is(name, {"ul", "ol"})) {
            const int indent = static_cast<int>(m_lists.size()) + 1;
            const bool checklist = token.attribute(u"class") == QLatin1StringView("checklist");
            ListKind kind = ListKind::Bulleted;
            if (name == QLatin1StringView("ol")) {
                kind = ListKind::Numbered;
            } else if (checklist) {
                kind = ListKind::Checklist;
            }
            QTextListFormat format =
                listFormat(kind, indent,
                           m_mode == ReadMode::Note ? token.attribute(u"data-style") : QString());
            if (kind == ListKind::Numbered) {
                const int start = token.attribute(u"start").toInt();
                if (start > 1) {
                    format.setStart(start);
                }
            }
            m_lists.append({.format = format, .list = nullptr});
        } else if (name == QLatin1StringView("li")) {
            QTextBlockFormat block;
            if (!m_lists.isEmpty() && m_lists.last().format.boolProperty(Checklist)) {
                block.setMarker(token.attribute(u"data-checked") == QLatin1StringView("true")
                                    ? QTextBlockFormat::MarkerType::Checked
                                    : QTextBlockFormat::MarkerType::Unchecked);
            }
            beginBlock(block);
            if (!m_lists.isEmpty()) {
                ListContext &context = m_lists.last();
                if (context.list == nullptr) {
                    context.list = m_cursor.createList(context.format);
                } else {
                    context.list->add(m_cursor.block());
                }
            }
        } else if (name == QLatin1StringView("hr")) {
            if (!inCell) {
                QTextBlockFormat block;
                block.setProperty(QTextFormat::BlockTrailingHorizontalRulerWidth,
                                  QTextLength(QTextLength::PercentageLength, 100));
                beginBlock(block);
            }
            m_inBlock = false;
        } else if (name == QLatin1StringView("br")) {
            if (m_inBlock) {
                m_cursor.insertText(QString(QChar::LineSeparator), m_formats.last());
            }
        } else if (name == QLatin1StringView("img")) {
            image(token);
        } else if (name == QLatin1StringView("a") &&
                   token.attribute(u"class") == QLatin1StringView("attachment") &&
                   m_mode == ReadMode::Note) {
            if (const auto ref = core::parseAttachmentSource(token.attribute(u"href"))) {
                m_attachment = attachmentFormat(ref->noteId, ref->fileName, ref->fileName,
                                                token.attribute(u"data-size").toLongLong());
                m_attachmentName.clear();
            } else {
                m_formats.append(m_formats.last());
            }
        } else if (isInlineElement(name)) {
            QTextCharFormat format = m_formats.last();
            applyInline(token, format, m_mode);
            m_formats.append(format);
        }
    }

    void end(const Token &token)
    {
        const QString &name = token.name;
        if (m_code != nullptr) {
            if (name == QLatin1StringView("pre")) {
                m_cursor = m_code->lastCursorPosition();
                m_cursor.movePosition(QTextCursor::NextBlock);
                m_code = nullptr;
                m_reuse = true;
                m_inBlock = false;
            }
            return;
        }
        if (m_attachment) {
            if (name == QLatin1StringView("a")) {
                QTextCharFormat chip = *m_attachment;
                m_attachment.reset();
                if (const QString shown = m_attachmentName.simplified(); !shown.isEmpty()) {
                    chip.setProperty(AttachmentName, shown);
                }
                insertObject(chip);
            }
            return;
        }
        if (name == QLatin1StringView("table")) {
            endTable();
            return;
        }
        if (m_table != nullptr && is(name, {"td", "th"})) {
            m_lists.clear();
            m_inBlock = false;
            return;
        }
        if (isInlineElement(name)) {
            if (m_formats.size() > 1) {
                m_formats.removeLast();
            }
        } else if (is(name, {"ul", "ol"})) {
            if (!m_lists.isEmpty()) {
                m_lists.removeLast();
            }
            m_inBlock = false;
        } else if (name == QLatin1StringView("blockquote")) {
            m_quote = false;
        } else if (core::html::isBlockElement(name)) {
            m_inBlock = false;
        }
    }

    void text(QString text)
    {
        if (m_code != nullptr) {
            addCodeText(std::move(text));
            return;
        }
        if (m_attachment) {
            m_attachmentName += text;
            return;
        }
        if (m_table != nullptr && m_column < 0) {
            return; // Text between the rows of a table belongs to no cell.
        }
        if (m_mode == ReadMode::Paste) {
            // In pasted HTML line breaks and runs of white space are one space, as in a browser.
            static const QRegularExpression space(QStringLiteral("[ \\t\\r\\n]+"));
            text.replace(space, QStringLiteral(" "));
            if (!m_inBlock || m_cursor.atBlockStart()) {
                while (text.startsWith(u' ')) {
                    text.remove(0, 1);
                }
            }
        } else {
            // Line breaks in a note's source only lay out the HTML; `<br>` is the line break.
            text.remove(u'\n');
            text.remove(u'\r');
        }
        if (text.isEmpty()) {
            return;
        }
        if (!m_inBlock) {
            if (text.trimmed().isEmpty()) {
                return;
            }
            QTextBlockFormat block;
            if (m_quote) {
                setQuote(block, true);
            }
            beginBlock(block);
        }
        m_cursor.insertText(text, m_formats.last());
    }

    /// Adds the text of a code block: every line feed starts a new line. A line feed right
    /// after `<pre>` only lays out the HTML, as in browsers; inside `<code>` it is data.
    void addCodeText(QString text)
    {
        text.remove(u'\r');
        if (m_codeStart && text.startsWith(u'\n')) {
            text.remove(0, 1);
        }
        m_codeStart = false;
        const QStringList lines = text.split(u'\n');
        for (qsizetype i = 0; i < lines.size(); ++i) {
            if (i > 0) {
                m_cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
            }
            m_cursor.insertText(lines.at(i), QTextCharFormat());
        }
    }

    /// Reads an image: one of the note's files, or in pasted HTML a picture to store. A picture
    /// on the web is not downloaded; its description stays as text.
    void image(const Token &token)
    {
        const QString source = token.attribute(u"src");
        if (m_mode == ReadMode::Note) {
            if (const auto ref = core::parseAttachmentSource(source)) {
                const int width = token.attribute(u"data-width").toInt();
                const QString wrapName = token.attribute(u"data-wrap");
                ImageWrap wrap = ImageWrap::Inline;
                if (wrapName == QLatin1StringView("square")) {
                    wrap = ImageWrap::Square;
                } else if (wrapName == QLatin1StringView("top-bottom")) {
                    wrap = ImageWrap::TopAndBottom;
                }
                insertObject(imageFormat(ref->noteId, ref->fileName, validWidth(width), wrap));
            }
            return;
        }
        if (source.startsWith(QLatin1StringView("data:image/")) ||
            source.startsWith(QLatin1StringView("file:"))) {
            insertObject(pendingFormat(ImageObject, source));
        } else if (const QString alt = token.attribute(u"alt").simplified(); !alt.isEmpty()) {
            text(alt);
        }
    }

    /// Inserts an image or a chip at the cursor, starting a paragraph when outside one.
    void insertObject(const QTextCharFormat &format)
    {
        if (m_table != nullptr && m_column < 0) {
            return;
        }
        if (!m_inBlock) {
            QTextBlockFormat block;
            if (m_quote) {
                setQuote(block, true);
            }
            beginBlock(block);
        }
        editor::insertObject(m_cursor, format);
    }

    void startTable()
    {
        if (m_table != nullptr) {
            ++m_nestedTables;
            return;
        }
        const TableShape shape = measureTable(m_tokens, m_index, m_mode);
        if (shape.rows == 0 || shape.columns == 0) {
            return;
        }
        m_lists.clear();
        m_quote = false;
        // Like a code block, the table goes after the current block, and the block Qt adds after
        // it is reused by the next one.
        m_table = m_cursor.insertTable(
            shape.rows, shape.columns,
            tableFormat(shape.columns, shape.headerRows, shape.widths, shape.width));
        m_row = -1;
        m_column = -1;
        m_inBlock = false;
    }

    void startCell()
    {
        ++m_column;
        if (m_row < 0 || m_row >= m_table->rows() || m_column >= m_table->columns()) {
            m_column = -1;
            return;
        }
        m_cursor = m_table->cellAt(m_row, m_column).firstCursorPosition();
        m_lists.clear();
        m_formats.resize(1);
        m_reuse = true;
        m_inBlock = false;
    }

    void endTable()
    {
        if (m_nestedTables > 0) {
            --m_nestedTables;
            m_inBlock = false;
            return;
        }
        if (m_table == nullptr) {
            return;
        }
        m_cursor = m_table->lastCursorPosition();
        m_cursor.movePosition(QTextCursor::NextBlock);
        m_table = nullptr;
        m_lists.clear();
        m_reuse = true;
        m_inBlock = false;
    }

    /// Starts a block. An empty block the document already has (its first block, the block Qt
    /// keeps after a code block or a table, the first block of a cell) is reused rather than
    /// followed by another.
    void beginBlock(const QTextBlockFormat &format)
    {
        if (m_reuse) {
            m_cursor.setBlockFormat(format);
            m_cursor.setBlockCharFormat(QTextCharFormat());
            m_reuse = false;
        } else {
            m_cursor.insertBlock(format, QTextCharFormat());
        }
        m_formats.resize(1);
        m_inBlock = true;
    }

    QTextCursor m_cursor;
    ReadMode m_mode;
    QList<Token> m_tokens;
    qsizetype m_index = 0;
    QList<QTextCharFormat> m_formats;
    QList<ListContext> m_lists;
    /// The code block being read, or nullptr.
    QTextFrame *m_code = nullptr;
    /// Whether no text of the code block has been read yet.
    bool m_codeStart = false;
    /// The table being read, or nullptr; tables inside it are counted, not built.
    QTextTable *m_table = nullptr;
    int m_nestedTables = 0;
    int m_row = -1;
    /// The cell being read, or -1 outside cells.
    int m_column = -1;
    /// The chip of an attachment element being read, and the name it shows.
    std::optional<QTextCharFormat> m_attachment;
    QString m_attachmentName;
    /// The depth of elements whose content is not part of the body, such as `<style>`.
    int m_skip = 0;
    bool m_reuse = true;
    bool m_inBlock = false;
    bool m_quote = false;
};

/// The inline elements of a run of text, outermost first, as a comparable key.
QStringList wrappersOf(const QTextCharFormat &format)
{
    QStringList open;
    if (format.isAnchor() && !format.anchorHref().isEmpty()) {
        // A link to another note carries its class.
        const bool noteLink = core::noteLinkTarget(format.anchorHref()).has_value();
        open.append((noteLink ? QStringLiteral("<a class=\"note-link\" href=\"%1\">")
                              : QStringLiteral("<a href=\"%1\">"))
                        .arg(core::html::escapeAttribute(format.anchorHref())));
    }
    const bool code = format.boolProperty(InlineCode);
    const QStringList families = format.fontFamilies().toStringList();
    if (!code && !families.isEmpty() && !families.first().isEmpty()) {
        open.append(QStringLiteral("<span style=\"font-family: '%1'\">")
                        .arg(core::html::escapeAttribute(families.first())));
    }
    // A custom colour has no rule in the note style sheet, so it carries its own style for
    // browsers; reading takes the colour from data-color only.
    if (const QString key = format.stringProperty(TextColorKey); !key.isEmpty()) {
        if (const auto custom = core::customColorValue(key)) {
            open.append(QStringLiteral("<span data-color=\"%1\" style=\"color: #%2\">")
                            .arg(key)
                            .arg(*custom, 6, 16, QLatin1Char('0')));
        } else {
            open.append(
                QStringLiteral("<span data-color=\"%1\">").arg(core::html::escapeAttribute(key)));
        }
    }
    if (const QString key = format.stringProperty(HighlightKey); !key.isEmpty()) {
        if (const auto custom = core::customColorValue(key)) {
            open.append(QStringLiteral("<mark data-color=\"%1\" style=\"background: #%2\">")
                            .arg(key)
                            .arg(core::paperHighlight(*custom), 6, 16, QLatin1Char('0')));
        } else {
            open.append(
                QStringLiteral("<mark data-color=\"%1\">").arg(core::html::escapeAttribute(key)));
        }
    }
    if (format.fontWeight() >= QFont::Bold) {
        open.append(QStringLiteral("<strong>"));
    }
    if (format.fontItalic()) {
        open.append(QStringLiteral("<em>"));
    }
    if (format.fontUnderline()) {
        open.append(QStringLiteral("<u>"));
    }
    if (format.fontStrikeOut()) {
        open.append(QStringLiteral("<s>"));
    }
    if (format.verticalAlignment() == QTextCharFormat::AlignSuperScript) {
        open.append(QStringLiteral("<sup>"));
    } else if (format.verticalAlignment() == QTextCharFormat::AlignSubScript) {
        open.append(QStringLiteral("<sub>"));
    }
    if (code) {
        open.append(QStringLiteral("<code>"));
    }
    return open;
}

/// Returns the end tag of a start tag written by wrappersOf().
QString closingOf(const QString &openTag)
{
    const qsizetype nameEnd = openTag.indexOf(QRegularExpression(QStringLiteral("[ >]")));
    return QStringLiteral("</") + openTag.mid(1, nameEnd - 1) + u'>';
}

/// Writes an image or an attachment chip; pasted objects not stored yet have no HTML.
QString objectHtml(const QTextCharFormat &format)
{
    const QString note = format.stringProperty(ObjectNote);
    const QString file = format.stringProperty(ObjectFile);
    if (note.isEmpty() || file.isEmpty()) {
        return {};
    }
    const QString source = core::html::escapeAttribute(core::attachmentSource(note, file));
    if (format.objectType() == ImageObject) {
        QString html = QStringLiteral("<img src=\"%1\"").arg(source);
        if (const int width = format.intProperty(ImageWidth); width > 0) {
            html += QStringLiteral(" data-width=\"%1\"").arg(width);
        }
        switch (imageWrap(format)) {
        case ImageWrap::Square:
            html += QStringLiteral(" data-wrap=\"square\"");
            break;
        case ImageWrap::TopAndBottom:
            html += QStringLiteral(" data-wrap=\"top-bottom\"");
            break;
        case ImageWrap::Inline:
            break;
        }
        return html + u'>';
    }
    return QStringLiteral("<a class=\"attachment\" href=\"%1\" data-size=\"%2\">%3</a>")
        .arg(source)
        .arg(format.property(AttachmentSize).toLongLong())
        .arg(core::html::escapeText(format.stringProperty(AttachmentName)));
}

/// Writes the inline content of a block; runs with the same elements are joined, and images
/// and chips stand outside any formatting.
QString writeInline(const QTextBlock &block)
{
    struct Run
    {
        QStringList wrappers;
        QString html;
        bool object = false;
    };
    QList<Run> runs;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (!fragment.isValid()) {
            continue;
        }
        const QTextCharFormat format = fragment.charFormat();
        if (format.objectType() == ImageObject || format.objectType() == AttachmentObject) {
            // Neighbouring equal objects share a fragment, one character each.
            const QString html = objectHtml(format);
            for (const QChar c : fragment.text()) {
                if (c == QChar::ObjectReplacementCharacter && !html.isEmpty()) {
                    runs.append({.wrappers = {}, .html = html, .object = true});
                }
            }
            continue;
        }
        QString text = core::html::escapeText(fragment.text());
        text.replace(QChar::LineSeparator, QStringLiteral("<br>"));
        text.remove(QChar::ObjectReplacementCharacter);
        const QStringList wrappers = wrappersOf(format);
        if (!runs.isEmpty() && !runs.last().object && runs.last().wrappers == wrappers) {
            runs.last().html += text;
        } else {
            runs.append({.wrappers = wrappers, .html = text, .object = false});
        }
    }
    QString html;
    for (const Run &run : runs) {
        if (run.html.isEmpty()) {
            continue;
        }
        html += run.wrappers.join(QString());
        html += run.html;
        for (const QString &open : std::views::reverse(run.wrappers)) {
            html += closingOf(open);
        }
    }
    return html;
}

/// Writes a document frame by frame: the body, code blocks and tables, whose cells hold
/// blocks written like the body's.
class Writer
{
public:
    QString write(const QTextDocument &document)
    {
        m_root = document.rootFrame();
        writeItems(m_root->begin());
        closeContext();
        if (m_html.endsWith(u'\n')) {
            m_html.chop(1);
        }
        return m_html;
    }

private:
    struct OpenList
    {
        const QTextList *list;
        int indent;
        bool ordered;
        bool itemOpen;
    };

    void closeLists(qsizetype keep)
    {
        while (m_lists.size() > keep) {
            if (m_lists.last().itemOpen) {
                m_html += QStringLiteral("</li>\n");
            }
            m_html +=
                m_lists.last().ordered ? QStringLiteral("</ol>\n") : QStringLiteral("</ul>\n");
            m_lists.removeLast();
        }
    }

    /// Closes the open lists and quote, before a frame and at the end of a cell.
    void closeContext()
    {
        closeLists(0);
        if (m_quote) {
            m_html += QStringLiteral("</blockquote>\n");
            m_quote = false;
        }
    }

    void writeItems(QTextFrame::iterator it)
    {
        for (; !it.atEnd(); ++it) {
            if (QTextFrame *child = it.currentFrame()) {
                closeContext();
                if (const auto *table = qobject_cast<QTextTable *>(child)) {
                    writeTable(*table);
                } else if (child->frameFormat().boolProperty(CodeBlock)) {
                    writeCode(*child);
                } else {
                    writeItems(child->begin());
                }
            } else {
                writeBlock(it.currentBlock());
            }
        }
    }

    void writeCode(const QTextFrame &code)
    {
        m_html += QStringLiteral("<pre");
        const QTextFrameFormat frame = code.frameFormat();
        for (const auto &[property, attribute] :
             {std::pair{CodeLanguage, u"data-language"_s}, std::pair{CodeTitle, u"data-title"_s}}) {
            if (const QString value = frame.stringProperty(property); !value.isEmpty()) {
                m_html +=
                    QStringLiteral(" %1=\"%2\"").arg(attribute, core::html::escapeAttribute(value));
            }
        }
        m_html += QStringLiteral("><code>");
        m_html += core::html::escapeText(codeText(code));
        m_html += QStringLiteral("</code></pre>\n");
    }

    void writeTable(const QTextTable &table)
    {
        const QTextTableFormat format = table.format();
        const int headerRows = std::min(format.headerRowCount(), table.rows());
        if (const int width = tableWidth(format); width > 0) {
            m_html += QStringLiteral("<table data-width=\"%1\">\n").arg(width);
        } else {
            m_html += QStringLiteral("<table>\n");
        }
        // Widths are written only when the columns do not share the width equally.
        if (!hasEqualColumns(format, table.columns())) {
            m_html += QStringLiteral("<colgroup>");
            for (const qreal width : columnWidths(format, table.columns())) {
                m_html += QStringLiteral("<col style=\"width: %1%\">").arg(std::lround(width));
            }
            m_html += QStringLiteral("</colgroup>\n");
        }
        for (int row = 0; row < table.rows(); ++row) {
            const bool header = row < headerRows;
            if (row == 0 && header) {
                m_html += QStringLiteral("<thead>\n");
            } else if (row == headerRows) {
                m_html += QStringLiteral("<tbody>\n");
            }
            const QString cellTag = header ? QStringLiteral("th") : QStringLiteral("td");
            m_html += QStringLiteral("<tr>");
            for (int column = 0; column < table.columns(); ++column) {
                // A cell's blocks are written like the body's, then form the cell's content.
                const QString outer = std::exchange(m_html, QString());
                writeItems(table.cellAt(row, column).begin());
                closeContext();
                if (m_html.endsWith(u'\n')) {
                    m_html.chop(1);
                }
                m_html = outer + QStringLiteral("<%1>%2</%1>").arg(cellTag, m_html);
            }
            m_html += QStringLiteral("</tr>\n");
            if (header && row == headerRows - 1) {
                m_html += QStringLiteral("</thead>\n");
            }
        }
        if (table.rows() > headerRows) {
            m_html += QStringLiteral("</tbody>\n");
        }
        m_html += QStringLiteral("</table>\n");
    }

    void writeBlock(const QTextBlock &block)
    {
        const QTextBlockFormat format = block.blockFormat();
        const QTextList *list = block.textList();
        const bool quoted = list == nullptr && format.intProperty(QTextFormat::BlockQuoteLevel) > 0;
        if (m_quote && !quoted) {
            m_html += QStringLiteral("</blockquote>\n");
            m_quote = false;
        }
        // Qt keeps a paragraph before a frame that starts the document and after one that ends
        // it; left empty, it is not part of the note.
        if (isEmptyParagraph(block) && QTextCursor(block).currentFrame() == m_root) {
            const QTextBlock next = block.next();
            const QTextBlock previous = block.previous();
            if ((!previous.isValid() && next.isValid() &&
                 QTextCursor(next).currentFrame() != m_root) ||
                (!next.isValid() && previous.isValid() &&
                 QTextCursor(previous).currentFrame() != m_root)) {
                return;
            }
        }

        if (list != nullptr) {
            writeListItem(block, *list);
            return;
        }
        closeLists(0);
        if (quoted) {
            if (!m_quote) {
                m_html += QStringLiteral("<blockquote>\n");
                m_quote = true;
            }
            m_html += QStringLiteral("<p>%1</p>\n").arg(writeInline(block));
        } else if (format.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)) {
            m_html += QStringLiteral("<hr>\n");
        } else if (const int level = format.headingLevel(); level >= 1 && level <= 3) {
            m_html += QStringLiteral("<h%1>%2</h%1>\n").arg(level).arg(writeInline(block));
        } else {
            m_html += QStringLiteral("<p>%1</p>\n").arg(writeInline(block));
        }
    }

    void writeListItem(const QTextBlock &block, const QTextList &list)
    {
        const QTextListFormat listFormat = list.format();
        const int indent = std::max(1, listFormat.indent());
        while (!m_lists.isEmpty() &&
               (m_lists.last().indent > indent ||
                (m_lists.last().indent == indent && m_lists.last().list != &list))) {
            closeLists(m_lists.size() - 1);
        }
        const ListKind kind = listKindOf(listFormat);
        if (m_lists.isEmpty() || m_lists.last().indent < indent) {
            if (!m_lists.isEmpty()) {
                m_html += u'\n'; // A nested list goes inside its parent item.
            }
            if (kind == ListKind::Numbered) {
                m_html += QStringLiteral("<ol");
                if (const QString style = listFormat.stringProperty(NumberingStyleId);
                    !style.isEmpty()) {
                    m_html += QStringLiteral(" data-style=\"%1\"")
                                  .arg(core::html::escapeAttribute(style));
                }
                if (listFormat.start() > 1) {
                    m_html += QStringLiteral(" start=\"%1\"").arg(listFormat.start());
                }
                m_html += QStringLiteral(">\n");
            } else {
                m_html += kind == ListKind::Checklist ? QStringLiteral("<ul class=\"checklist\">\n")
                                                      : QStringLiteral("<ul>\n");
            }
            m_lists.append({.list = &list,
                            .indent = indent,
                            .ordered = kind == ListKind::Numbered,
                            .itemOpen = false});
        } else if (m_lists.last().itemOpen) {
            m_html += QStringLiteral("</li>\n");
        }
        if (kind == ListKind::Checklist) {
            m_html += block.blockFormat().marker() == QTextBlockFormat::MarkerType::Checked
                          ? QStringLiteral("<li data-checked=\"true\">")
                          : QStringLiteral("<li data-checked=\"false\">");
        } else {
            m_html += QStringLiteral("<li>");
        }
        m_html += writeInline(block);
        m_lists.last().itemOpen = true;
    }

    const QTextFrame *m_root = nullptr;
    QString m_html;
    QList<OpenList> m_lists;
    bool m_quote = false;
};

} // namespace

void readBody(QTextDocument &document, QStringView html, ReadMode mode)
{
    Reader reader(document, mode);
    // Qt rebuilds the whole frame tree when an edit that added a frame ends, so a body with
    // many code blocks or tables would take time growing with the square of their number. As
    // one edit block, the body is read with a single rebuild at its end.
    QTextCursor edit(&document);
    edit.beginEditBlock();
    reader.read(html);
    edit.endEditBlock();
}

QString writeBody(const QTextDocument &document)
{
    return Writer().write(document);
}

} // namespace editor
