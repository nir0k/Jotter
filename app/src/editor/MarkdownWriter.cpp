#include "core/Attachments.h"
#include "editor/Commands.h"
#include "editor/Formats.h"
#include "editor/Markdown.h"

#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>

#include <algorithm>
#include <array>
#include <ranges>

namespace editor {

namespace {

/// Chooses a delimiter longer than every consecutive backtick run in the code.
QString codeDelimiter(const QString &text, int minimum)
{
    int longest = 0;
    int run = 0;
    for (const QChar character : text) {
        run = character == u'`' ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    return {std::max(minimum, longest + 1), u'`'};
}

/// Writes inline code literally, with a delimiter and padding that keep its backticks and edge
/// spaces; a line break inside it becomes a space, as code spans cannot break lines.
QString codeSpan(QString text)
{
    text.replace(QChar::LineSeparator, u' ');
    const QString delimiter = codeDelimiter(text, 1);
    const bool pad = text.startsWith(u'`') || text.endsWith(u'`') ||
                     (text.startsWith(u' ') && text.endsWith(u' ') && !text.trimmed().isEmpty());
    const QString padding = pad ? QStringLiteral(" ") : QString();
    return delimiter + padding + text + padding + delimiter;
}

/// Escapes the characters of plain text that would start inline Markdown syntax. Underscores
/// inside words and ampersands that do not form an entity stay literal, so prose and
/// identifiers keep reading naturally. Object replacement characters are dropped.
QString escapeText(QStringView text)
{
    QString result;
    result.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar character = text.at(i);
        bool escape = false;
        switch (character.unicode()) {
        case u'\\':
        case u'`':
        case u'*':
        case u'[':
        case u']':
        case u'<':
        case u'~':
            escape = true;
            break;
        case u'_':
            escape = i == 0 || i + 1 == text.size() || !text.at(i - 1).isLetterOrNumber() ||
                     !text.at(i + 1).isLetterOrNumber();
            break;
        case u'&': {
            qsizetype end = i + 1;
            while (end < text.size() && (text.at(end).isLetterOrNumber() || text.at(end) == u'#')) {
                ++end;
            }
            escape = end > i + 1 && end < text.size() && text.at(end) == u';';
            break;
        }
        case QChar::ObjectReplacementCharacter:
            continue;
        default:
            break;
        }
        if (escape) {
            result += u'\\';
        }
        result += character;
    }
    return result;
}

/// Escapes the first character of a written line when it would start a block: a heading, a
/// quote, a list item, a divider or a heading underline.
void escapeLineStart(QString &line)
{
    qsizetype start = 0;
    while (start < line.size() && line.at(start) == u' ') {
        ++start;
    }
    if (start == line.size()) {
        return;
    }
    const QStringView rest = QStringView(line).sliced(start);
    const auto blankAt = [rest](qsizetype i) {
        return i >= rest.size() || rest.at(i) == u' ' || rest.at(i) == u'\t';
    };
    const auto isDigit = [](QChar c) { return c >= u'0' && c <= u'9'; };
    const QChar first = rest.front();
    qsizetype at = -1;
    if (first == u'>') {
        at = 0;
    } else if (first == u'#') {
        qsizetype run = 0;
        while (run < rest.size() && rest.at(run) == u'#') {
            ++run;
        }
        at = run <= 6 && blankAt(run) ? 0 : -1;
    } else if (first == u'-' || first == u'+' || first == u'=') {
        const bool rule =
            std::ranges::all_of(rest, [first](QChar c) { return c == first || c == u' '; });
        at = rule || (first != u'=' && blankAt(1)) ? 0 : -1;
    } else if (isDigit(first)) {
        qsizetype digits = 0;
        while (digits < rest.size() && isDigit(rest.at(digits))) {
            ++digits;
        }
        if (digits <= 9 && digits < rest.size() &&
            (rest.at(digits) == u'.' || rest.at(digits) == u')') && blankAt(digits + 1)) {
            at = digits;
        }
    }
    if (at >= 0) {
        line.insert(start + at, u'\\');
    }
}

/// Writes a link destination, in angle brackets when it holds characters that end a bare one.
QString linkDestination(const QString &href)
{
    const bool bracket = std::ranges::any_of(href, [](QChar c) {
        return c.isSpace() || c == u'(' || c == u')' || c == u'<' || c == u'>';
    });
    if (!bracket) {
        return href;
    }
    QString escaped = href;
    escaped.replace(u'<', QStringLiteral("\\<")).replace(u'>', QStringLiteral("\\>"));
    return u'<' + escaped + u'>';
}

/// An emphasis or a link around inline Markdown: the text that opens it and the text that
/// closes it.
struct Span
{
    QString open;
    QString close;

    bool operator==(const Span &) const = default;
};

/// Returns the Markdown spans of a character format, outermost first. Underline, scripts,
/// colours and fonts have no Markdown and are dropped.
///
/// @param format The format of a text fragment.
/// @param link Whether a link is included; images and attachment links cannot sit in one.
/// @return The link, bold, italic and strikethrough spans the format has.
QList<Span> spansOf(const QTextCharFormat &format, bool link)
{
    QList<Span> spans;
    if (link && format.isAnchor() && !format.anchorHref().isEmpty()) {
        spans.append({.open = QStringLiteral("["),
                      .close = QStringLiteral("](") + linkDestination(format.anchorHref()) + u')'});
    }
    if (format.fontWeight() >= QFont::Bold) {
        spans.append({.open = QStringLiteral("**"), .close = QStringLiteral("**")});
    }
    if (format.fontItalic()) {
        spans.append({.open = QStringLiteral("*"), .close = QStringLiteral("*")});
    }
    if (format.fontStrikeOut()) {
        spans.append({.open = QStringLiteral("~~"), .close = QStringLiteral("~~")});
    }
    return spans;
}

/// Builds the inline Markdown of one block. A span stays open across runs that share it, so
/// partly formatted text nests instead of being split; spans open and close next to the text
/// rather than around its edge spaces, which Markdown would not treat as emphasis, and spans
/// left empty are dropped.
class InlineWriter
{
public:
    /// Appends already escaped text inside the given spans.
    void text(const QString &escaped, const QList<Span> &spans)
    {
        qsizetype lead = 0;
        while (lead < escaped.size() && escaped.at(lead).isSpace()) {
            ++lead;
        }
        closeOthers(spans);
        m_out += QStringView(escaped).first(lead);
        open(spans);
        m_out += QStringView(escaped).sliced(lead);
    }

    /// Appends literal Markdown, such as a code span or an image, inside the given spans.
    void atom(const QString &markdown, const QList<Span> &spans)
    {
        closeOthers(spans);
        open(spans);
        m_out += markdown;
    }

    /// Closes the open spans and returns the block's Markdown, line separators included.
    QString finish()
    {
        closeOthers({});
        return std::move(m_out);
    }

private:
    struct Open
    {
        Span span;
        qsizetype end; ///< The output length right after the opening text.
    };

    /// Closes the innermost open spans down to the outermost one missing from @p spans.
    void closeOthers(const QList<Span> &spans)
    {
        const auto missing = std::ranges::find_if(
            m_open, [&spans](const Open &open) { return !spans.contains(open.span); });
        if (missing == m_open.end()) {
            return;
        }
        const qsizetype keep = missing - m_open.begin();
        qsizetype trimmed = m_out.size();
        while (trimmed > 0 && m_out.at(trimmed - 1).isSpace()) {
            --trimmed;
        }
        const QString tail = m_out.sliced(trimmed);
        m_out.truncate(trimmed);
        while (m_open.size() > keep) {
            const Open &top = m_open.last();
            if (m_out.size() == top.end) {
                m_out.chop(top.span.open.size());
            } else {
                m_out += top.span.close;
            }
            m_open.removeLast();
        }
        m_out += tail;
    }

    /// Opens the spans of @p spans that are not open yet, in their order.
    void open(const QList<Span> &spans)
    {
        for (const Span &span : spans) {
            if (std::ranges::none_of(m_open,
                                     [&span](const Open &open) { return open.span == span; })) {
                m_out += span.open;
                m_open.append({.span = span, .end = m_out.size()});
            }
        }
    }

    QString m_out;
    QList<Open> m_open;
};

/// Writes the inline content of a block, keeping its line separators for the caller to turn
/// into hard breaks. Images and attachments keep their library-relative addresses; pasted
/// objects not stored yet have none and are left out.
QString inlineMarkdown(const QTextBlock &block)
{
    InlineWriter writer;
    QString code;
    QList<Span> codeSpans;
    const auto flushCode = [&] {
        if (!code.isEmpty()) {
            writer.atom(codeSpan(code), codeSpans);
            code.clear();
        }
    };
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        const QTextCharFormat format = fragment.charFormat();
        const QList<Span> spans = spansOf(format, true);
        if (format.boolProperty(InlineCode)) {
            if (spans != codeSpans) {
                flushCode();
                codeSpans = spans;
            }
            code += fragment.text();
            continue;
        }
        flushCode();
        if (format.objectType() == ImageObject || format.objectType() == AttachmentObject) {
            const QString note = format.stringProperty(ObjectNote);
            const QString file = format.stringProperty(ObjectFile);
            if (note.isEmpty() || file.isEmpty()) {
                continue;
            }
            const QString target =
                QStringLiteral("](") + linkDestination(core::attachmentSource(note, file)) + u')';
            const QString markdown =
                format.objectType() == ImageObject
                    ? QStringLiteral("![") + escapeText(file) + target
                    : u'[' + escapeText(format.stringProperty(AttachmentName)) + target;
            // Neighbouring equal objects share a fragment, one character each.
            for (const QChar c : fragment.text()) {
                if (c == QChar::ObjectReplacementCharacter) {
                    writer.atom(markdown, spansOf(format, false));
                }
            }
            continue;
        }
        writer.text(escapeText(fragment.text()), spans);
    }
    flushCode();
    return writer.finish();
}

/// Writes a document as GitHub Markdown frame by frame. Every paragraph, heading and list item
/// stays on one line: wrapping to a width would change the text where it is pasted.
class MarkdownWriter
{
public:
    /// Returns the Markdown of a document, ending with a line feed unless empty.
    QString write(const QTextDocument &document)
    {
        writeItems(document.rootFrame()->begin());
        if (!m_out.isEmpty()) {
            m_out += u'\n';
        }
        return m_out;
    }

private:
    /// How a written unit is separated from the previous one.
    enum class Join : quint8 { Blank, Line, Quote };

    /// A list being written, one per nesting level.
    struct Level
    {
        const QTextList *list;
        int indent;
        qsizetype width; ///< The width of the current item's marker and its space.
        QChar delimiter; ///< The bullet or the number delimiter.
    };

    /// Appends a written unit after the separator its join asks for.
    void append(const QString &markdown, Join join)
    {
        static constexpr std::array<QStringView, 3> Separators{u"\n\n", u"\n", u"\n>\n"};
        if (!m_out.isEmpty()) {
            m_out += Separators.at(static_cast<std::size_t>(join));
        }
        m_out += markdown;
    }

    /// Turns line separators into hard breaks continued with @p continuation, drops trailing
    /// breaks and escapes each line's start.
    static QString lines(const QString &markdown, const QString &continuation)
    {
        QStringList parts = markdown.split(QChar::LineSeparator);
        while (!parts.isEmpty() && parts.last().trimmed().isEmpty()) {
            parts.removeLast();
        }
        for (QString &part : parts) {
            escapeLineStart(part);
        }
        return parts.join(QStringLiteral("\\\n") + continuation);
    }

    /// Writes the blocks and frames of a frame in order; tables and code blocks end the
    /// open lists and quote.
    void writeItems(QTextFrame::iterator it)
    {
        for (; !it.atEnd(); ++it) {
            if (QTextFrame *child = it.currentFrame()) {
                m_levels.clear();
                m_quote = false;
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

    /// Writes a code block as a fence that no line of its code can close.
    void writeCode(const QTextFrame &frame)
    {
        const QString text = codeText(frame);
        const QString fence = codeDelimiter(text, 3);
        append(fence + frame.frameFormat().stringProperty(CodeLanguage) + u'\n' + text + u'\n' +
                   fence,
               Join::Blank);
    }

    /// Writes a pipe table whose first row is the header. Cells hold inline text only, so the
    /// blocks of a cell are joined by spaces; a merged cell is written in its first position.
    void writeTable(const QTextTable &table)
    {
        QStringList rows;
        for (int row = 0; row < table.rows(); ++row) {
            QStringList cells;
            for (int column = 0; column < table.columns(); ++column) {
                const QTextTableCell cell = table.cellAt(row, column);
                QStringList parts;
                if (cell.row() == row && cell.column() == column) {
                    for (auto it = cell.begin(); !it.atEnd(); ++it) {
                        if (const QString part = inlineMarkdown(it.currentBlock()).trimmed();
                            it.currentFrame() == nullptr && !part.isEmpty()) {
                            parts.append(part);
                        }
                    }
                }
                QString text = parts.join(u' ');
                text.replace(QChar::LineSeparator, u' ').replace(u'|', QStringLiteral("\\|"));
                cells.append(text);
            }
            rows.append(QStringLiteral("| ") + cells.join(QStringLiteral(" | ")) +
                        QStringLiteral(" |"));
            if (row == 0) {
                rows.append(
                    u'|' + QStringList(table.columns(), QStringLiteral(" --- ")).join(u'|') + u'|');
            }
        }
        append(rows.join(u'\n'), Join::Blank);
    }

    /// Writes a paragraph, heading, quote or divider. A block without text changes nothing, as
    /// Markdown has no empty paragraphs.
    void writeBlock(const QTextBlock &block)
    {
        if (const QTextList *list = block.textList()) {
            writeListItem(block, *list);
            return;
        }
        const QTextBlockFormat format = block.blockFormat();
        const bool divider = format.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth);
        const bool quoted = format.intProperty(QTextFormat::BlockQuoteLevel) > 0;
        const QString text =
            divider ? QStringLiteral("---")
                    : lines(inlineMarkdown(block), quoted ? QStringLiteral("> ") : QString());
        if (text.isEmpty()) {
            return;
        }
        m_levels.clear();
        const bool afterQuote = std::exchange(m_quote, quoted && !divider);
        const int level = divider ? 0 : std::min(6, format.headingLevel());
        if (m_quote) {
            append(QStringLiteral("> ") + text, afterQuote ? Join::Quote : Join::Blank);
        } else if (level > 0) {
            append(QString(level, u'#') + u' ' + escapedHeadingEnd(text), Join::Blank);
        } else {
            append(text, Join::Blank);
        }
    }

    /// Escapes a run of `#` closing a heading, which Markdown would otherwise drop.
    static QString escapedHeadingEnd(QString text)
    {
        qsizetype run = text.size();
        while (run > 0 && text.at(run - 1) == u'#') {
            --run;
        }
        if (run < text.size() && (run == 0 || text.at(run - 1) == u' ')) {
            text.insert(run, u'\\');
        }
        return text;
    }

    /// Writes a list item, indented under the items of the lists it is nested in. A list that
    /// directly follows another one on its level switches the bullet or delimiter, so Markdown
    /// keeps them apart.
    void writeListItem(const QTextBlock &block, const QTextList &list)
    {
        m_quote = false;
        const Join join = m_levels.isEmpty() ? Join::Blank : Join::Line;
        const QTextListFormat format = list.format();
        const int indent = std::max(1, format.indent());
        QChar sibling;
        while (!m_levels.isEmpty() &&
               (m_levels.last().indent > indent ||
                (m_levels.last().indent == indent && m_levels.last().list != &list))) {
            if (m_levels.last().indent == indent) {
                sibling = m_levels.last().delimiter;
            }
            m_levels.removeLast();
        }
        const ListKind kind = listKindOf(format);
        if (m_levels.isEmpty() || m_levels.last().indent < indent) {
            const bool numbered = kind == ListKind::Numbered;
            QChar delimiter = numbered ? u'.' : u'-';
            if (sibling == delimiter) {
                delimiter = numbered ? u')' : u'*';
            }
            m_levels.append({.list = &list, .indent = indent, .width = 0, .delimiter = delimiter});
        }
        QString pad;
        for (qsizetype i = 0; i + 1 < m_levels.size(); ++i) {
            pad += QString(m_levels.at(i).width, u' ');
        }
        QString marker(m_levels.last().delimiter);
        if (kind == ListKind::Numbered) {
            const int start = format.hasProperty(QTextFormat::ListStart) ? format.start() : 1;
            marker.prepend(QString::number(start + list.itemNumber(block)));
        }
        m_levels.last().width = marker.size() + 1;
        if (kind == ListKind::Checklist) {
            marker += block.blockFormat().marker() == QTextBlockFormat::MarkerType::Checked
                          ? QStringLiteral(" [x]")
                          : QStringLiteral(" [ ]");
        }
        const QString text =
            lines(inlineMarkdown(block), pad + QString(m_levels.last().width, u' '));
        append(pad + marker + (text.isEmpty() ? QString() : u' ' + text), join);
    }

    QString m_out;
    QList<Level> m_levels;
    bool m_quote = false;
};

} // namespace

QString writeMarkdown(const QTextDocument &document)
{
    return MarkdownWriter().write(document);
}

} // namespace editor
