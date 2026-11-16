#include "editor/Presenter.h"

#include "core/NoteLinks.h"
#include "core/Numbering.h"
#include "editor/Commands.h"
#include "editor/Formats.h"
#include "editor/Syntax.h"
#include "ui/TagColor.h"
#include "ui/Theme.h"

#include <QFontDatabase>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>

#include <KSyntaxHighlighting/AbstractHighlighter>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/State>
#include <KSyntaxHighlighting/Theme>

#include <utility>

namespace editor {

/// Runs a syntax definition over one line of code at a time and collects the formats.
class CodeHighlighter : public KSyntaxHighlighting::AbstractHighlighter
{
public:
    struct Run
    {
        int offset;
        int length;
        KSyntaxHighlighting::Format format;
    };

    /// Highlights one line after the state the previous line ended in.
    ///
    /// @return The state this line ends in.
    KSyntaxHighlighting::State highlight(QStringView line, const KSyntaxHighlighting::State &state)
    {
        m_runs.clear();
        return highlightLine(line, state);
    }

    /// The formats of the line highlighted last.
    const QList<Run> &runs() const { return m_runs; }

protected:
    void applyFormat(int offset, int length, const KSyntaxHighlighting::Format &format) override
    {
        m_runs.append({.offset = offset, .length = length, .format = format});
    }

private:
    QList<Run> m_runs;
};

namespace {

/// The state a line of code ends in, kept with its block so the next line continues from it.
class CodeLineState : public QTextBlockUserData
{
public:
    KSyntaxHighlighting::State state;
};

/// Heading sizes relative to the body text (headings provide larger text).
qreal headingScale(int level)
{
    switch (level) {
    case 1:
        return 1.6;
    case 2:
        return 1.35;
    case 3:
        return 1.15;
    default:
        return 1.0;
    }
}

/// The device preference for checked checklist items: struck through and
/// muted, or drawn like other text.
bool &strikeChecked()
{
    static bool on = true;
    return on;
}

/// The presenters that follow the theme, all on the interface thread, redrawn when the
/// preference changes.
QList<Presenter *> &livePresenters()
{
    static QList<Presenter *> presenters;
    return presenters;
}

} // namespace

void rehighlight(const QTextFrame &frame)
{
    // The document's highlighter is its child; a document without one has nothing to redraw.
    auto *highlighter = frame.document()->findChild<QSyntaxHighlighter *>();
    if (highlighter == nullptr) {
        return;
    }
    const QTextBlock end = frame.lastCursorPosition().block().next();
    for (QTextBlock block = frame.firstCursorPosition().block(); block.isValid() && block != end;
         block = block.next()) {
        highlighter->rehighlightBlock(block);
    }
}

QHash<int, QString> listMarkers(const QTextDocument &document)
{
    struct Level
    {
        const QTextList *list;
        int indent;
        int number;
    };
    const core::NumberingStyle &fallback = *core::findNumberingStyle(core::DefaultNumberingStyle);
    QHash<int, QString> markers;
    QList<Level> levels;
    // Lists never continue from one frame or table cell into another.
    std::pair<const QTextFrame *, int> context{nullptr, -1};
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const QTextFrame *frame = QTextCursor(block).currentFrame();
        int cell = -1;
        if (const auto *table = qobject_cast<const QTextTable *>(frame)) {
            const QTextTableCell at = table->cellAt(block.position());
            cell = (at.row() * table->columns()) + at.column();
        }
        if (std::pair{frame, cell} != context) {
            context = {frame, cell};
            levels.clear();
        }
        const QTextList *list = block.textList();
        if (list == nullptr) {
            levels.clear();
            continue;
        }
        const QTextListFormat format = list->format();
        const int indent = std::max(1, format.indent());
        while (!levels.isEmpty() &&
               (levels.last().indent > indent ||
                (levels.last().indent == indent && levels.last().list != list))) {
            levels.removeLast();
        }
        if (levels.isEmpty() || levels.last().indent < indent) {
            const bool numbered = listKindOf(format) == ListKind::Numbered;
            levels.append(
                {.list = list, .indent = indent, .number = numbered ? format.start() - 1 : 0});
        }
        ++levels.last().number;
        if (listKindOf(format) != ListKind::Numbered) {
            continue;
        }
        const core::NumberingStyle *style =
            core::findNumberingStyle(format.stringProperty(NumberingStyleId));
        QList<int> numbers;
        for (const Level &level : levels) {
            numbers.append(level.number);
        }
        markers.insert(block.blockNumber(),
                       core::numberingMarker(style != nullptr ? *style : fallback, numbers));
    }
    return markers;
}

Presenter::Presenter(QTextDocument *document)
    : QSyntaxHighlighter(document)
{
    connect(ui::Theme::current(), &ui::Theme::changed, this, &QSyntaxHighlighter::rehighlight);
    livePresenters().append(this);
}

Presenter::Presenter(QTextDocument *document, const ui::Roles &roles,
                     const KSyntaxHighlighting::Repository &repository)
    : QSyntaxHighlighter(document)
    , m_roles(roles)
    , m_repository(&repository)
{}

Presenter::~Presenter()
{
    livePresenters().removeOne(this);
}

void Presenter::setStrikeChecked(bool on)
{
    if (std::exchange(strikeChecked(), on) == on) {
        return;
    }
    for (Presenter *presenter : std::as_const(livePresenters())) {
        presenter->rehighlight();
    }
}

void Presenter::setMissingNotes(QSet<QString> ids)
{
    if (ids != m_missingNotes) {
        m_missingNotes = std::move(ids);
        rehighlight();
    }
}

void Presenter::highlightBlock(const QString &text)
{
    const ui::Roles &r = m_roles ? *m_roles : ui::Theme::current()->roles();
    const QTextBlock block = currentBlock();
    // Each character collects the formats of every layer; runs are applied once at the end,
    // because a later setFormat() on the same range would replace rather than merge.
    QList<QTextCharFormat> overlay(text.size());
    const auto apply = [&](qsizetype start, qsizetype length, const QTextCharFormat &format) {
        for (qsizetype i = std::max<qsizetype>(0, start); i < std::min(text.size(), start + length);
             ++i) {
            overlay[i].merge(format);
        }
    };

    if (const int level = block.blockFormat().headingLevel(); level > 0) {
        QTextCharFormat heading;
        heading.setFontWeight(QFont::Bold);
        // The body font is set in pixels (it follows the theme's root size), so headings are too.
        const QFont base = document()->defaultFont();
        if (base.pixelSize() > 0) {
            heading.setProperty(
                QTextFormat::FontPixelSize,
                static_cast<int>(std::lround(base.pixelSize() * headingScale(level))));
        } else {
            heading.setFontPointSize(base.pointSizeF() * headingScale(level));
        }
        apply(0, text.size(), heading);
    }
    if (const QTextFrame *code = codeBlockOf(block)) {
        QTextCharFormat fixed;
        fixed.setFontFamilies({QFontDatabase::systemFont(QFontDatabase::FixedFont).family()});
        apply(0, text.size(), fixed);
        const QString language = code->frameFormat().stringProperty(CodeLanguage);
        const KSyntaxHighlighting::Definition definition =
            m_repository != nullptr ? syntaxDefinition(language, *m_repository)
                                    : syntaxDefinition(language);
        if (definition.isValid()) {
            if (!m_code) {
                m_code = std::make_unique<CodeHighlighter>();
            }
            // The highlighting theme is the palette's own, or follows the mode; a
            // theme missing from the installed KDE SyntaxHighlighting falls back too.
            const KSyntaxHighlighting::Repository &repository =
                m_repository != nullptr ? *m_repository : syntaxRepository();
            KSyntaxHighlighting::Theme syntaxTheme = repository.theme(r.syntaxTheme);
            if (!syntaxTheme.isValid()) {
                syntaxTheme =
                    repository.defaultTheme(r.dark ? KSyntaxHighlighting::Repository::DarkTheme
                                                   : KSyntaxHighlighting::Repository::LightTheme);
            }
            m_code->setDefinition(definition);
            m_code->setTheme(syntaxTheme);
            KSyntaxHighlighting::State previous;
            if (block != code->firstCursorPosition().block()) {
                if (const auto *data =
                        dynamic_cast<const CodeLineState *>(block.previous().userData())) {
                    previous = data->state;
                }
            }
            const KSyntaxHighlighting::State state = m_code->highlight(text, previous);
            for (const CodeHighlighter::Run &run : m_code->runs()) {
                const KSyntaxHighlighting::Format &format = run.format;
                if (format.isDefaultTextStyle(syntaxTheme)) {
                    continue;
                }
                QTextCharFormat look;
                if (format.hasTextColor(syntaxTheme)) {
                    look.setForeground(format.textColor(syntaxTheme));
                }
                if (format.isBold(syntaxTheme)) {
                    look.setFontWeight(QFont::Bold);
                }
                if (format.isItalic(syntaxTheme)) {
                    look.setFontItalic(true);
                }
                apply(run.offset, run.length, look);
            }
            auto *data = dynamic_cast<CodeLineState *>(currentBlockUserData());
            if (data == nullptr) {
                data = new CodeLineState;
                setCurrentBlockUserData(data);
            }
            // A changed end state makes QSyntaxHighlighter go on to the next line.
            if (data->state != state) {
                data->state = state;
                setCurrentBlockState(currentBlockState() ^ 1);
            }
        }
    }
    if (const QTextTable *table = QTextCursor(block).currentTable();
        table != nullptr &&
        table->cellAt(block.position()).row() < table->format().headerRowCount()) {
        QTextCharFormat header;
        header.setFontWeight(QFont::Bold);
        apply(0, text.size(), header);
    }
    if (block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel) > 0) {
        QTextCharFormat quote;
        quote.setForeground(r.onSurfaceVariant);
        apply(0, text.size(), quote);
    }

    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        const QTextCharFormat format = fragment.charFormat();
        const qsizetype start = fragment.position() - block.position();
        const qsizetype length = fragment.length();
        QTextCharFormat look;
        if (const auto color = ui::colorFromKey(format.stringProperty(TextColorKey))) {
            look.setForeground(ui::tagTextColor(*color, r));
        }
        if (const auto color = ui::colorFromKey(format.stringProperty(HighlightKey))) {
            look.setBackground(ui::tagHighlightColor(*color, r));
        }
        // Inline code's rounded box is drawn under the text (paintInlineCode()); a colour chosen
        // for the text wins over the code's own.
        if (format.boolProperty(InlineCode)) {
            look.setFontFamilies({QFontDatabase::systemFont(QFontDatabase::FixedFont).family()});
            if (!look.hasProperty(QTextFormat::ForegroundBrush)) {
                look.setForeground(r.tertiary);
            }
        }
        if (format.isAnchor()) {
            const auto target = core::noteLinkTarget(format.anchorHref());
            if (target && m_missingNotes.contains(*target)) {
                look.setForeground(r.onSurfaceVariant);
                look.setFontStrikeOut(true);
            } else {
                look.setForeground(r.primary);
                look.setFontUnderline(true);
            }
        }
        apply(start, length, look);
    }

    // A presenter with fixed colours draws an export, which keeps the look of the note format.
    if (block.blockFormat().marker() == QTextBlockFormat::MarkerType::Checked &&
        (m_roles || strikeChecked())) {
        QTextCharFormat done;
        done.setFontStrikeOut(true);
        done.setForeground(r.onSurfaceVariant);
        apply(0, text.size(), done);
    }

    qsizetype runStart = 0;
    for (qsizetype i = 1; i <= text.size(); ++i) {
        if (i == text.size() || overlay.at(i) != overlay.at(runStart)) {
            if (!overlay.at(runStart).properties().isEmpty()) {
                setFormat(static_cast<int>(runStart), static_cast<int>(i - runStart),
                          overlay.at(runStart));
            }
            runStart = i;
        }
    }
}

} // namespace editor
