#pragma once

#include <QList>
#include <QString>
#include <QStringView>

#include <span>

namespace core {

/// Lists nest up to this many levels.
inline constexpr int MaxListLevels = 9;

/// The numbering style of an `<ol>` without `data-style`: `1.`.
inline constexpr QStringView DefaultNumberingStyle(u"decimal");

/// How a numbered list counts.
enum class Counter {
    Decimal,
    UpperLatin,
    UpperRoman,
};

/// A numbering style: how every level of a numbered list counts.
struct NumberingStyle
{
    /// The value of the list's `data-style` attribute.
    QStringView id;
    Counter counter = Counter::Decimal;
    /// Whether a marker starts with the numbers of the items above it (`1.2.3.`).
    bool includeParents = false;
};

/// Returns the numbering styles in the order the interface offers them: `1.`, `A.`, `I.` and
/// Multilevel.
std::span<const NumberingStyle> numberingStyles();

/// Finds a numbering style.
///
/// @param id The style's identifier.
/// @return The style, or nullptr for an unknown identifier, which is drawn as `1.`.
const NumberingStyle *findNumberingStyle(QStringView id);

/// Writes a number in a counter. Letters continue with two letters after the last one (`Z`,
/// `AA`); Roman numerals above 3999 fall back to decimal.
QString formatCounter(Counter counter, int value);

/// Returns the marker of a list item.
///
/// @param style The list's numbering style.
/// @param numbers The item's number at every level from the first to its own, so the last
///        one is the item's own number.
/// @return The marker, such as `B.` or `1.2.`.
QString numberingMarker(const NumberingStyle &style, const QList<int> &numbers);

/// Returns the style sheet that makes a browser draw the numbering styles a note body uses with
/// the same markers as the editor: CSS counters per nesting level and a `::marker` rule for each
/// styled list's level. A body with only plain numbered lists, or lists of unknown styles,
/// needs none.
///
/// @param body The note body.
/// @return The CSS rules, or an empty string.
QString numberingStyleSheet(QStringView body);

} // namespace core
