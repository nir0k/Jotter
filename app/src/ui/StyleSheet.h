#pragma once

#include "ui/Palette.h"

#include <QString>

namespace ui {

/// Builds the application style sheet from the colour roles and the root size.
///
/// Widgets opt into their looks through dynamic properties: `role` on buttons
/// (`primary`, `danger`), `textRole` on labels (set by setTextRole()) and `pane` on containers
/// (`sidebar`, `list`, `editor`, and `nav` for the settings dialog's section list).
///
/// @param roles The colour roles of the variant being shown.
/// @param sidebar The colour roles of the main window's sidebar.
/// @param rem The root size unit in device-independent pixels.
/// @param images A folder with `check.svg` and `dash.svg` for checkboxes in lists, drawn in
///               the roles' `onPrimary` colour; style sheets take images only as files.
/// @return The style sheet text.
QString buildStyleSheet(const Roles &roles, const Roles &sidebar, int rem, const QString &images);

/// Builds the own style sheet of a tag pill in a custom colour, which the application style
/// sheet has no rule for; it keeps the focus border of the other pills.
///
/// @param colorKey The custom colour key, `#rrggbb`.
/// @param roles The colour roles of the variant being shown.
/// @return The style sheet text.
QString customPillStyleSheet(const QString &colorKey, const Roles &roles);

} // namespace ui
