#pragma once

#include "ui/Palette.h"

#include <QColor>
#include <QString>

#include <optional>
#include <span>

namespace ui {

/// The fixed colours for things people colour themselves: tags, note and collection titles, text
/// colours and highlights. The base tag colours come first, the additions from
/// Maroon on. Stored by key; a custom colour is stored as `#rrggbb` instead.
enum class TagColor {
    Red,
    Orange,
    Amber,
    Yellow,
    Lime,
    Green,
    Emerald,
    Teal,
    Cyan,
    Sky,
    Blue,
    Indigo,
    Violet,
    Purple,
    Fuchsia,
    Pink,
    Rose,
    Brown,
    Gray,
    Maroon,
    Coral,
    Gold,
    Beige,
    Olive,
    Mint,
    Navy,
    Lavender,
    Slate,
    Graphite,
};

/// Returns every tag colour in its order, for pickers.
std::span<const TagColor> allTagColors();

/// Returns the localized name of a tag colour, for menus and tooltips.
QString tagColorName(TagColor color);

/// Returns the stable storage key of a tag colour (`red`, `sky`, …).
QString tagColorKey(TagColor color);

/// Returns the base value of a stored colour key, a tag colour key or a custom `#rrggbb` colour.
///
/// @param key The stored colour key.
/// @return The colour's base value, or no value for an empty or unknown key, which is shown
///         without colour.
std::optional<QColor> colorFromKey(const QString &key);

/// Returns the base value of a tag colour, the same in every palette.
QColor tagColorValue(TagColor color);

/// Returns the colour for text drawn in a colour on the given palette, mixed with the text
/// colour so that it stays readable in light and dark variants.
///
/// @param base The base value of a tag colour or a custom colour.
/// @param roles The palette's roles.
/// @return The text colour.
QColor tagTextColor(const QColor &base, const Roles &roles);

/// Returns the text colour of an item title in a list: on the selection colour when selected,
/// otherwise in its colour, or the ordinary text colour without one (or with an unknown key).
QColor itemTextColor(const QString &colorKey, const Roles &roles, bool selected);

/// Returns the background of text highlighted in a colour: strong enough to see, light enough
/// that the palette's text colour stays readable on it.
QColor tagHighlightColor(const QColor &base, const Roles &roles);

/// Returns the fill of a tag pill in the given palette.
QColor tagFillColor(const QColor &base, const Roles &roles);

/// Returns the border of a tag pill in the given palette.
QColor tagBorderColor(const QColor &base, const Roles &roles);

/// The colours of a tag pill; a neutral pill, for a tag without meaning in colour, is
/// drawn in the text colour.
struct PillColors
{
    QColor fill;
    QColor border;
    QColor text;
};

/// Returns the neutral pill colours of the given palette: a 14% tint and a 45% border of the
/// text colour, with muted text.
PillColors neutralPillColors(const Roles &roles);

/// Returns the colours of a tag pill: a 14% tint, a 45% border and the text of its colour, or
/// the neutral pill colours for a tag without colour (or with an unknown key).
PillColors pillColors(const QString &colorKey, const Roles &roles);

} // namespace ui
