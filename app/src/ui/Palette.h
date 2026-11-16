#pragma once

#include <QColor>
#include <QString>

#include <optional>
#include <span>

namespace ui {

/// The palettes of the theme picker, stored by their stable key: Warm, GitHub
/// and GitHub Dimmed, then Jotter's own Monokai.
enum class PaletteId { Warm, GitHub, GitHubDimmed, Monokai };

/// The colour roles of one palette variant.
struct Roles
{
    QColor primary;
    QColor onPrimary;
    QColor primaryContainer;
    QColor onPrimaryContainer;
    QColor tertiary;
    /// The page behind sections; the sidebar and dialogs use it.
    QColor background;
    /// Cards and panes: the note list and the editor.
    QColor surface;
    QColor onSurface;
    /// Secondary text, metadata and idle icons.
    QColor onSurfaceVariant;
    QColor surfaceContainerHighest;
    QColor outline;
    QColor outlineVariant;
    QColor error;
    QColor onError;
    QColor errorContainer;
    QColor onErrorContainer;
    QColor success;
    QColor warning;
    QColor info;
    bool dark = false;
    /// The KDE SyntaxHighlighting theme of code blocks, or empty for the mode's default theme.
    QString syntaxTheme;
};

/// Composites @p color at @p alpha over @p over.
///
/// @param color The colour laid on top.
/// @param alpha Its opacity, from 0 to 1.
/// @param over The opaque colour underneath.
/// @return The resulting opaque colour.
QColor mix(const QColor &color, float alpha, const QColor &over);

/// Returns every palette in the theme picker's order.
std::span<const PaletteId> allPalettes();

/// Returns the name of a palette, a proper name shown untranslated.
QString paletteName(PaletteId id);

/// Returns the stable storage key of a palette (`warm`, `github`, `github-dimmed`, `monokai`).
QString paletteKey(PaletteId id);

/// Parses a stored palette key.
///
/// @param key The stored key.
/// @return The palette, or no value for a key Jotter does not know; callers fall back to Warm.
std::optional<PaletteId> paletteFromKey(const QString &key);

/// Tells whether a palette has a light variant (GitHub Dimmed and Monokai are dark only).
bool hasLightVariant(PaletteId id);

/// Returns the colour roles of a palette variant.
///
/// @param id The palette.
/// @param dark Whether the dark variant is wanted; a palette without the requested variant
///             returns the one it has.
/// @return The roles of the variant.
Roles roles(PaletteId id, bool dark);

} // namespace ui
