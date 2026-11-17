#pragma once

#include "ui/TagColor.h"

#include <QIcon>
#include <QString>

class QPainter;
class QRectF;

namespace ui {

/// The colour role an icon is tinted with in its normal state.
enum class IconTone {
    /// Idle icons: `onSurfaceVariant`.
    Idle,
    /// Active or meaningful icons: `primary`.
    Primary,
    /// Destructive actions: `error`.
    Error,
    /// Icons on a filled primary button: `onPrimary`.
    OnPrimary,
};

/// The surface an icon is drawn on, which decides the roles it takes its colours from.
enum class IconSurface {
    /// The content of the window and dialogs: the theme's roles.
    Content,
    /// The main window's sidebar: the sidebar roles (the palette's dark variant).
    Sidebar,
};

/// Returns a glyph of Jotter's icon manifest, tinted from the current theme.
///
/// The icon re-reads the theme whenever it is painted, so it follows palette and mode changes.
/// Disabled icons use `outline`; selected ones in item views use `onPrimaryContainer`.
///
/// @param name The glyph's name in `resources/icons/manifest.json`, such as `Search`.
/// @param tone The colour role of the normal state.
/// @param surface The surface the icon is drawn on.
/// @return The icon; a null icon for an unknown name, with a warning in debug builds.
QIcon icon(const QString &name, IconTone tone = IconTone::Idle,
           IconSurface surface = IconSurface::Content);

/// Returns a glyph drawn in a colour, as the icon of a coloured note or collection title; the
/// shade follows the current theme like the title's text.
///
/// @param name The glyph's name in the icon manifest.
/// @param color The base value of a tag colour or a custom colour (see colorFromKey()).
/// @param surface The surface the icon is drawn on.
QIcon icon(const QString &name, const QColor &color, IconSurface surface = IconSurface::Content);

/// Returns the glyph a collection is drawn with: its stored icon name when Jotter has that glyph,
/// otherwise `Folder`, such as for a name the icon picker no longer offers. The stored name itself
/// is not changed.
///
/// @param name The icon name the collection stores.
/// @return @p name, or `Folder` when Jotter has no glyph of that name.
QString collectionGlyph(const QString &name);

/// Draws a glyph in a given colour. Unlike icon(), it reads nothing from the theme and makes no
/// pixmaps, so it also draws on worker threads, such as into an export.
///
/// @param painter The painter.
/// @param rect Where to draw the glyph.
/// @param name The glyph's name in the icon manifest; an unknown name draws nothing.
/// @param color The colour of the glyph's shapes.
void paintGlyph(QPainter &painter, const QRectF &rect, const QString &name, const QColor &color);

/// Returns a one-colour drawing whose shapes use `currentColor`, such as the monochrome tray
/// icon, as pixmaps of the sizes trays ask for.
///
/// @param path The SVG file, usually a resource.
/// @param color The colour of the shapes.
/// @return The icon, or a null icon when the file cannot be read.
QIcon monochromeIcon(const QString &path, const QColor &color);

} // namespace ui
