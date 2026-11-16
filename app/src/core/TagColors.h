#pragma once

#include <QStringView>

#include <array>
#include <cstdint>
#include <optional>

namespace core {

/// One of the fixed tag colours: its storage key and its base value (0xRRGGBB).
struct TagColorValue
{
    const char *key;
    std::uint32_t rgb;
};

/// The fixed colours for things people colour themselves, in their order: the base
/// tag colours, nineteen hues and then maroon to graphite. Library files
/// store the key; the value is used by the interface (through ui::TagColor) and by the style
/// sheet that makes note files readable in a browser.
// clang-format off
inline constexpr std::array<TagColorValue, 29> TagColorValues{{
    {.key = "red", .rgb = 0xDC2626},
    {.key = "orange", .rgb = 0xEA580C},
    {.key = "amber", .rgb = 0xD97706},
    {.key = "yellow", .rgb = 0xCA8A04},
    {.key = "lime", .rgb = 0x65A30D},
    {.key = "green", .rgb = 0x16A34A},
    {.key = "emerald", .rgb = 0x059669},
    {.key = "teal", .rgb = 0x0D9488},
    {.key = "cyan", .rgb = 0x0891B2},
    {.key = "sky", .rgb = 0x0284C7},
    {.key = "blue", .rgb = 0x2563EB},
    {.key = "indigo", .rgb = 0x4F46E5},
    {.key = "violet", .rgb = 0x7C3AED},
    {.key = "purple", .rgb = 0x9333EA},
    {.key = "fuchsia", .rgb = 0xC026D3},
    {.key = "pink", .rgb = 0xDB2777},
    {.key = "rose", .rgb = 0xE11D48},
    {.key = "brown", .rgb = 0x92400E},
    {.key = "gray", .rgb = 0x6B7280},
    {.key = "maroon", .rgb = 0x7F1D1D},
    {.key = "coral", .rgb = 0xF2705E},
    {.key = "gold", .rgb = 0xD4AF37},
    {.key = "beige", .rgb = 0xC2A47C},
    {.key = "olive", .rgb = 0x708238},
    {.key = "mint", .rgb = 0x34D399},
    {.key = "navy", .rgb = 0x1E3A8A},
    {.key = "lavender", .rgb = 0xA78BFA},
    {.key = "slate", .rgb = 0x475569},
    {.key = "graphite", .rgb = 0x3F3F46},
}};
// clang-format on

/// Parses the key of a custom colour, `#rrggbb` in either case, stored where a tag colour key
/// would be.
///
/// @param key The stored colour key.
/// @return The colour's value (0xRRGGBB), or no value for a tag colour key or anything else.
inline std::optional<std::uint32_t> customColorValue(QStringView key)
{
    if (key.size() != 7 || key.front() != u'#') {
        return std::nullopt;
    }
    std::uint32_t rgb = 0;
    for (const QChar c : key.sliced(1)) {
        const char16_t u = c.unicode();
        std::uint32_t digit = 0;
        if (u >= u'0' && u <= u'9') {
            digit = static_cast<std::uint32_t>(u - u'0');
        } else if (u >= u'a' && u <= u'f') {
            digit = static_cast<std::uint32_t>(u - u'a') + 10U;
        } else if (u >= u'A' && u <= u'F') {
            digit = static_cast<std::uint32_t>(u - u'A') + 10U;
        } else {
            return std::nullopt;
        }
        rgb = (rgb << 4U) | digit;
    }
    return rgb;
}

/// Returns the highlight of a colour in a note file read in a browser: the colour at 30% over
/// white, so black text stays readable on it.
///
/// @param rgb The colour's value (0xRRGGBB).
/// @return The highlight's value (0xRRGGBB).
constexpr std::uint32_t paperHighlight(std::uint32_t rgb)
{
    std::uint32_t result = 0;
    for (const unsigned shift : {16U, 8U, 0U}) {
        const std::uint32_t channel = (rgb >> shift) & 0xFFU;
        result |= (((channel * 3U) + (255U * 7U) + 5U) / 10U) << shift;
    }
    return result;
}

} // namespace core
