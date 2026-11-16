#include "ui/TagColor.h"

#include "core/TagColors.h"

#include <QCoreApplication>

#include <array>

namespace ui {

namespace {

static_assert(core::TagColorValues.size() == static_cast<std::size_t>(TagColor::Graphite) + 1,
              "The enum lists the tag colours of core::TagColorValues in their order");

constexpr std::array<TagColor, core::TagColorValues.size()> Order = [] {
    std::array<TagColor, core::TagColorValues.size()> order{};
    for (std::size_t i = 0; i < order.size(); ++i) {
        order.at(i) = static_cast<TagColor>(i);
    }
    return order;
}();

/// Finds the table entry of a tag colour; the enum follows the order of the table.
const core::TagColorValue &entry(TagColor color)
{
    return core::TagColorValues.at(static_cast<std::size_t>(color));
}

} // namespace

std::span<const TagColor> allTagColors()
{
    return Order;
}

QString tagColorName(TagColor color)
{
    switch (color) {
    case TagColor::Red:
        return QCoreApplication::translate("ui::TagColor", "Red");
    case TagColor::Orange:
        return QCoreApplication::translate("ui::TagColor", "Orange");
    case TagColor::Amber:
        return QCoreApplication::translate("ui::TagColor", "Amber");
    case TagColor::Yellow:
        return QCoreApplication::translate("ui::TagColor", "Yellow");
    case TagColor::Lime:
        return QCoreApplication::translate("ui::TagColor", "Lime");
    case TagColor::Green:
        return QCoreApplication::translate("ui::TagColor", "Green");
    case TagColor::Emerald:
        return QCoreApplication::translate("ui::TagColor", "Emerald");
    case TagColor::Teal:
        return QCoreApplication::translate("ui::TagColor", "Teal");
    case TagColor::Cyan:
        return QCoreApplication::translate("ui::TagColor", "Cyan");
    case TagColor::Sky:
        return QCoreApplication::translate("ui::TagColor", "Sky");
    case TagColor::Blue:
        return QCoreApplication::translate("ui::TagColor", "Blue");
    case TagColor::Indigo:
        return QCoreApplication::translate("ui::TagColor", "Indigo");
    case TagColor::Violet:
        return QCoreApplication::translate("ui::TagColor", "Violet");
    case TagColor::Purple:
        return QCoreApplication::translate("ui::TagColor", "Purple");
    case TagColor::Fuchsia:
        return QCoreApplication::translate("ui::TagColor", "Fuchsia");
    case TagColor::Pink:
        return QCoreApplication::translate("ui::TagColor", "Pink");
    case TagColor::Rose:
        return QCoreApplication::translate("ui::TagColor", "Rose");
    case TagColor::Brown:
        return QCoreApplication::translate("ui::TagColor", "Brown");
    case TagColor::Gray:
        return QCoreApplication::translate("ui::TagColor", "Gray");
    case TagColor::Maroon:
        return QCoreApplication::translate("ui::TagColor", "Maroon");
    case TagColor::Coral:
        return QCoreApplication::translate("ui::TagColor", "Coral");
    case TagColor::Gold:
        return QCoreApplication::translate("ui::TagColor", "Gold");
    case TagColor::Beige:
        return QCoreApplication::translate("ui::TagColor", "Beige");
    case TagColor::Olive:
        return QCoreApplication::translate("ui::TagColor", "Olive");
    case TagColor::Mint:
        return QCoreApplication::translate("ui::TagColor", "Mint");
    case TagColor::Navy:
        return QCoreApplication::translate("ui::TagColor", "Navy");
    case TagColor::Lavender:
        return QCoreApplication::translate("ui::TagColor", "Lavender");
    case TagColor::Slate:
        return QCoreApplication::translate("ui::TagColor", "Slate");
    case TagColor::Graphite:
        return QCoreApplication::translate("ui::TagColor", "Graphite");
    }
    return {};
}

QString tagColorKey(TagColor color)
{
    return QString::fromLatin1(entry(color).key);
}

std::optional<QColor> colorFromKey(const QString &key)
{
    if (const auto custom = core::customColorValue(key)) {
        return QColor::fromRgb(*custom);
    }
    for (const core::TagColorValue &color : core::TagColorValues) {
        if (key == QLatin1StringView(color.key)) {
            return QColor::fromRgb(color.rgb);
        }
    }
    return std::nullopt;
}

QColor tagColorValue(TagColor color)
{
    return QColor::fromRgb(entry(color).rgb);
}

// The proportions of a tag pill: a 14% fill, a 45% border and text mixed
// 65% with the palette's text colour.

QColor tagTextColor(const QColor &base, const Roles &roles)
{
    return mix(base, 0.65F, roles.onSurface);
}

QColor itemTextColor(const QString &colorKey, const Roles &roles, bool selected)
{
    if (selected) {
        return roles.onPrimaryContainer;
    }
    if (const auto color = colorFromKey(colorKey)) {
        return tagTextColor(*color, roles);
    }
    return roles.onSurface;
}

QColor tagHighlightColor(const QColor &base, const Roles &roles)
{
    return mix(base, roles.dark ? 0.38F : 0.28F, roles.surface);
}

QColor tagFillColor(const QColor &base, const Roles &roles)
{
    return mix(base, 0.14F, roles.surface);
}

QColor tagBorderColor(const QColor &base, const Roles &roles)
{
    return mix(base, 0.45F, roles.surface);
}

PillColors neutralPillColors(const Roles &roles)
{
    return {.fill = mix(roles.onSurface, 0.14F, roles.surface),
            .border = mix(roles.onSurface, 0.45F, roles.surface),
            .text = roles.onSurfaceVariant};
}

PillColors pillColors(const QString &colorKey, const Roles &roles)
{
    if (const auto color = colorFromKey(colorKey)) {
        return {.fill = tagFillColor(*color, roles),
                .border = tagBorderColor(*color, roles),
                .text = tagTextColor(*color, roles)};
    }
    return neutralPillColors(roles);
}

} // namespace ui
