#pragma once

#include "ui/Palette.h"

#include <QHash>
#include <QSet>
#include <QSyntaxHighlighter>

#include <memory>
#include <optional>

class QTextFrame;

namespace KSyntaxHighlighting {
class Repository;
}

namespace core {
struct NumberingStyle;
}

namespace editor {

/// Draws the blocks of a code block or table again, after a change of its format that the
/// document does not report as a change of text, such as a code block's language or a table's
/// header row.
void rehighlight(const QTextFrame &frame);

/// Returns the markers of a document's numbered list items, by block number.
///
/// Items are counted as the note file nests them, so the editor shows the numbers a browser
/// does: a list continues until a block outside it or a shallower item of another list, each
/// table cell counts on its own, and a level includes the numbers of the items above it when
/// its style says so.
///
/// @param document The document.
/// @return The marker text of every numbered list item; unknown styles count as `1.`.
QHash<int, QString> listMarkers(const QTextDocument &document);

class CodeHighlighter;

/// Draws the meaning held in the document in the current theme, without changing the document:
/// heading sizes, quotes, link and inline code looks, text colours and highlights in the shade
/// of the palette, checked checklist items struck through and muted (unless the device
/// preference turns this off), and code blocks in a fixed font with the syntax highlighting of
/// their language, and the header rows of tables in bold.
///
/// It redraws everything when the theme changes, so a colour key always looks right in light
/// and dark variants.
class Presenter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    /// Creates a presenter that follows the current theme.
    explicit Presenter(QTextDocument *document);

    /// Creates a presenter that draws in fixed colours with syntax definitions of its own, for a
    /// document painted away from the interface thread, such as in an export.
    ///
    /// @param document The document.
    /// @param roles The colour roles to draw in.
    /// @param repository The syntax definitions; it must outlive the presenter.
    Presenter(QTextDocument *document, const ui::Roles &roles,
              const KSyntaxHighlighting::Repository &repository);
    ~Presenter() override;

    /// Chooses whether checked checklist items are drawn struck through and muted, a device
    /// preference. It applies to every presenter that follows the theme and
    /// redraws them; presenters with fixed colours, which draw exports, always strike them.
    ///
    /// @param on Whether checked items are struck through and muted.
    static void setStrikeChecked(bool on);

    /// Sets the notes that links point to but the library does not have; such links are drawn
    /// muted and struck through.
    void setMissingNotes(QSet<QString> ids);

protected:
    void highlightBlock(const QString &text) override;

private:
    /// Highlights code lines; created with the first code block.
    std::unique_ptr<CodeHighlighter> m_code;
    /// The fixed colours, or no value to follow the current theme.
    std::optional<ui::Roles> m_roles;
    /// The syntax definitions of a presenter with fixed colours; nullptr for the shared ones.
    const KSyntaxHighlighting::Repository *m_repository = nullptr;
    QSet<QString> m_missingNotes;
};

} // namespace editor
