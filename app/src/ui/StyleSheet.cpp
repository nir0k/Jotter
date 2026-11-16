#include "ui/StyleSheet.h"

#include "ui/TagColor.h"

#include <QHash>
#include <QRegularExpression>

#include <cmath>

namespace ui {

namespace {

// Placeholders: {role} is a colour role, {rem*N} a size of N rem in pixels, {tagPills} the rules
// of the pills of coloured tags.
// Desktop sizes: fields and buttons have a 6 px radius
// scaled with the font, menus and dialogs the 0.75 rem box radius, targets at least 2.25 rem.
constexpr auto Template = R"(
* { outline: none; }
QWidget { color: {onSurface}; }
QMainWindow, QDialog { background: {background}; }
QWidget[pane="list"], QWidget[pane="editor"] { background: {surface}; }
QWidget[pane="nav"] { background: {background}; }

/* The main window's sidebar is drawn in the palette's dark variant in both modes, as in Bear. */
QWidget[pane="sidebar"] { background: {sidebarBackground}; }
QWidget[pane="sidebar"] QWidget { color: {sidebarText}; }
QWidget[pane="sidebar"] QPushButton[flat="true"]:hover { background: {sidebarHover}; }
QWidget[pane="sidebar"] QScrollBar::handle { background: {sidebarScroll}; }
QWidget[pane="sidebar"] QScrollBar::handle:hover { background: {sidebarText}; }
/* The sidebar's delegate draws selection and hover itself, so the view's own row fill stays off. */
QWidget[pane="sidebar"] QTreeView { selection-background-color: transparent; }
QWidget[pane="sidebar"] QTreeView::item:selected, QWidget[pane="sidebar"] QTreeView::item:hover { background: transparent; }

QLabel[textRole="pageTitle"] { font-size: {rem*1.5}px; font-weight: 700; }
QLabel[textRole="sectionTitle"] { font-size: {rem*1.15}px; font-weight: 600; }
QLabel[textRole="bodySmall"] { font-size: {rem*0.92}px; }
QLabel[textRole="muted"] { font-size: {rem*0.92}px; color: {onSurfaceVariant}; }
QLabel[textRole="hint"] { font-size: {rem*0.85}px; color: {hint}; }
QLabel[textRole="label"] { font-weight: 500; }
QLabel[textRole="labelSmall"] { font-size: {rem*0.85}px; font-weight: 500; }

QPushButton {
    background: {surface}; color: {onSurface};
    border: 1px solid {outline}; border-radius: {rem*0.4}px;
    padding: {rem*0.375}px {rem*0.875}px; min-height: {rem*1.5}px;
}
QPushButton:hover { background: {hover}; }
QPushButton:pressed { background: {pressed}; }
QPushButton:focus { border-color: {primary}; }
QPushButton:disabled { color: {disabled}; border-color: {outlineVariant}; }
QPushButton[role="primary"] { background: {primary}; color: {onPrimary}; border: 1px solid {primary}; }
QPushButton[role="primary"]:hover { background: {primaryHover}; }
QPushButton[role="primary"]:disabled { background: {outlineVariant}; color: {disabled}; border-color: {outlineVariant}; }
QPushButton[role="danger"] { color: {error}; border-color: {error}; }
QPushButton[role="danger"]:hover { background: {errorContainer}; }
QPushButton[flat="true"] { background: transparent; border: none; text-align: left; }
QPushButton[flat="true"]:hover { background: {hover}; }
QPushButton[role="dangerFilled"] { background: {error}; color: {onError}; border: 1px solid {error}; }

QToolButton {
    background: transparent; border: none; border-radius: {rem*0.4}px;
    padding: {rem*0.375}px; min-width: {rem*1.5}px; min-height: {rem*1.5}px;
}
QToolButton:hover { background: {hover}; }
QToolButton:pressed { background: {pressed}; }
QToolButton:checked { background: {primaryContainer}; color: {onPrimaryContainer}; }
QToolButton:focus { border: 1px solid {primary}; }
QToolButton::menu-indicator { image: none; }

QLineEdit, QComboBox, QSpinBox, QDateEdit, QPlainTextEdit {
    background: {surface}; color: {onSurface};
    border: 1px solid {outline}; border-radius: {rem*0.4}px;
    padding: {rem*0.375}px {rem*0.5}px; min-height: {rem*1.5}px;
    selection-background-color: {primaryContainer}; selection-color: {onPrimaryContainer};
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDateEdit:focus, QPlainTextEdit:focus { border-color: {primary}; }
QLineEdit:disabled, QComboBox:disabled { color: {disabled}; border-color: {outlineVariant}; }
QLineEdit[titleField="true"] {
    background: transparent; border: none; padding: 0; min-height: 0;
    font-size: {rem*1.5}px; font-weight: 700;
}
/* The editor's toolbar keeps 2.25 rem pointer targets. */
QToolBar { background: transparent; border: none; spacing: {rem*0.2}px; }
QToolBar QToolButton { min-width: {rem*2.25}px; min-height: {rem*2.25}px; padding: 0; }
/* The overflow button is placed by its own size; a larger minimum would push it out of view. */
QToolBar QToolButton#qt_toolbar_ext_button { min-width: 0; min-height: 0; padding: {rem*0.2}px; }
QToolBar QComboBox { min-height: {rem*2.25}px; padding: 0 {rem*0.5}px; }
QWidget[formatBar="true"] { border-top: 1px solid {outline}; }
QWidget[noteHeader="true"] { border-bottom: 1px solid {outline}; }
QWidget[findBar="true"] { border-bottom: 1px solid {outline}; }
QWidget[infoPanel="true"] { border-left: 1px solid {outline}; }
QToolBar::separator { background: {outlineVariant}; width: 1px; margin: {rem*0.4}px {rem*0.4}px; }
/* Tag pills: neutral, fully rounded; the strip's field has the same shape. */
QFrame[tagPill="true"] { background: {tagFill}; border: 1px solid {tagBorder}; border-radius: {rem*0.6}px; }
/* Coloured tags: a rule per tag colour, ahead of the focus border so that it still shows. */
{tagPills}
QFrame[tagPill="true"]:focus { border-color: {primary}; }
QFrame[tagPill="true"] QLabel { color: {tagText}; }
QFrame[tagPill="true"] QToolButton { min-width: 0; min-height: 0; padding: 0; border-radius: {rem*0.5}px; background: {tagFill}; }
QFrame[tagPill="true"] QToolButton:hover { background: {tagBorder}; }
QToolButton[tagAdd="true"] { min-width: 0; min-height: 0; padding: 0; border-radius: {rem*0.6}px; }
QLineEdit[tagField="true"] {
    border-radius: {rem*0.6}px; padding: 0 {rem*0.6}px; min-height: 0;
    font-size: {rem*0.85}px; min-width: {rem*8}px;
}
QLabel[inlineError="true"] { color: {error}; font-size: {rem*0.85}px; }
QLineEdit[search="true"] { background: {surfaceContainerHighest}; border-color: {surfaceContainerHighest}; }
QLineEdit[search="true"]:focus { border-color: {primary}; }
QComboBox::drop-down { border: none; width: {rem*1.5}px; }
QComboBox QAbstractItemView {
    background: {surface}; border: 1px solid {outline}; padding: {rem*0.25}px;
    selection-background-color: {hover}; selection-color: {onSurface};
}

QTreeView, QListView {
    background: transparent; border: none;
    selection-background-color: {primaryContainer}; selection-color: {onPrimaryContainer};
}
QTreeView::item, QListView::item { padding: {rem*0.3}px {rem*0.25}px; border-radius: {rem*0.4}px; }
QTreeView::item:hover:!selected, QListView::item:hover:!selected { background: {hover}; }
QTreeView::item:selected, QListView::item:selected { background: {primaryContainer}; color: {onPrimaryContainer}; }

/* Checkboxes in lists: the item style above would otherwise leave them as plain squares. */
QTreeView::indicator, QListView::indicator {
    width: {rem*0.95}px; height: {rem*0.95}px; border: 1px solid {outline};
    border-radius: {rem*0.2}px; background: {surface};
}
QTreeView::indicator:checked, QListView::indicator:checked {
    background: {primary}; border-color: {primary}; image: url({images}/check.svg);
}
QTreeView::indicator:indeterminate, QListView::indicator:indeterminate {
    background: {primary}; border-color: {primary}; image: url({images}/dash.svg);
}

QSplitter::handle { background: {outline}; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
QFrame[divider="true"] { background: {outlineVariant}; border: none; max-height: 1px; min-height: 1px; }

QMenu {
    background: {surface}; border: 1px solid {outline};
    border-radius: {rem*0.75}px; padding: {rem*0.25}px;
}
/* Lists in their own popup window, such as completions, look like menus: a pane's transparent
   list would show through as black on Wayland. Mark the popup with popupList. */
QListView[popupList="true"] {
    background: {surface}; border: 1px solid {outline}; padding: 0;
    selection-background-color: {hover}; selection-color: {onSurface};
}
QListView[popupList="true"]::item { padding: {rem*0.4}px {rem*0.6}px; }
QListView[popupList="true"]::item:hover { background: {hover}; }
QMenu::item { padding: {rem*0.4}px {rem*1.5}px {rem*0.4}px {rem*0.6}px; border-radius: {rem*0.4}px; }
QMenu::item:selected { background: {hover}; }
QMenu::item:checked { background: {primaryContainer}; color: {onPrimaryContainer}; }
QMenu::item:disabled { color: {disabled}; }
QMenu::separator { height: 1px; background: {outlineVariant}; margin: {rem*0.25}px {rem*0.25}px; }
QMenu::icon { padding-left: {rem*0.5}px; }

QToolTip {
    background: {onSurface}; color: {surface}; border: none;
    border-radius: {rem*0.3}px; padding: {rem*0.25}px {rem*0.5}px;
}

QScrollBar:vertical { background: transparent; width: {rem*0.7}px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: {rem*0.7}px; margin: 0; }
QScrollBar::handle { background: {scrollHandle}; border-radius: {rem*0.3}px; margin: 2px; }
QScrollBar::handle:hover { background: {outline}; }
QScrollBar::handle:vertical { min-height: {rem*2}px; }
QScrollBar::handle:horizontal { min-width: {rem*2}px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QProgressBar {
    background: {surfaceContainerHighest}; border: none; border-radius: {rem*0.25}px;
    max-height: {rem*0.5}px; text-align: center; color: transparent;
}
QProgressBar::chunk { background: {primary}; border-radius: {rem*0.25}px; }

QCheckBox, QRadioButton { spacing: {rem*0.5}px; }
QTabBar::tab { padding: {rem*0.4}px {rem*0.9}px; border: none; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: {primary}; border-bottom-color: {primary}; }

QFrame[notice="error"] { background: {errorContainer}; border: 1px solid {error}; border-radius: {rem*0.75}px; }
QFrame[notice="warning"] { background: {warningContainer}; border: 1px solid {warning}; border-radius: {rem*0.75}px; }
QFrame[notice="info"] { background: {infoContainer}; border: 1px solid {info}; border-radius: {rem*0.75}px; }
QFrame[toast="true"] { background: {onSurface}; border-radius: {rem*0.5}px; }
QFrame[toast="true"] QLabel { color: {surface}; }
QFrame[toast="true"] QPushButton { background: transparent; border: none; color: {surface}; font-weight: 700; padding: {rem*0.3}px {rem*0.6}px; }
QFrame[toast="true"] QPushButton:hover { background: {toastHover}; }
QFrame[popup="true"] { background: {surface}; border: 1px solid {outline}; border-radius: {rem*0.75}px; }
QFrame[popup="true"] QToolButton:checked { background: {primaryContainer}; }
QFrame[popup="true"] QScrollArea { background: transparent; }
QWidget[popupContent="true"] { background: {surface}; }
/* Filter chips: fully rounded, outlined while off and tinted
   while the filter is applied; the panel holding them is a tinted box. */
QFrame[filterPanel="true"] { background: {surfaceContainerHighest}; border-radius: {rem*0.75}px; }
QToolButton[chip="true"] {
    background: {surface}; border: 1px solid {outline}; border-radius: {rem*0.8}px;
    padding: {rem*0.2}px {rem*0.7}px; min-height: {rem*1.2}px;
}
QToolButton[chip="true"]:hover { background: {hover}; }
QToolButton[chip="true"]:focus { border-color: {primary}; }
QToolButton[chip="true"][active="true"], QToolButton[chip="true"]:checked {
    background: {primaryContainer}; color: {onPrimaryContainer}; border-color: {primaryContainer};
}
QToolButton[chip="true"][active="true"]:focus, QToolButton[chip="true"]:checked:focus { border-color: {primary}; }
QToolButton[tile="true"] { padding: 2px; border: 2px solid transparent; border-radius: {rem*0.45}px; min-width: 0; min-height: 0; }
QToolButton[tile="true"]:hover { background: transparent; border-color: {outline}; }
QToolButton[tile="true"]:checked { background: transparent; border-color: {primary}; }
QFrame[section="true"] { background: {surface}; border: 1px solid {outlineVariant}; border-radius: {rem*0.75}px; }
QFrame[segmented="true"] { background: {surface}; border: 1px solid {outline}; border-radius: {rem*0.4}px; }
QFrame[segmented="true"] QToolButton { border-radius: {rem*0.3}px; padding: {rem*0.25}px {rem*0.8}px; min-height: {rem*1.25}px; }
QFrame[segmented="true"] QToolButton:checked { background: {primaryContainer}; color: {onPrimaryContainer}; font-weight: 600; }
QFrame[segmented="true"] QToolButton:disabled { color: {disabled}; }
QFrame[segmented="true"]:disabled { border-color: {outlineVariant}; }
)";

/// Returns the rules that draw the pills matching a selector in the given colours. The remove
/// button lies over the end of the name, so it takes the pill's opaque fill to hide the text
/// under it.
QString pillRules(const QString &selector, const PillColors &pill)
{
    return QStringLiteral("%1 { background: %2; border-color: %3; }\n%1 QLabel { color: %4; }\n"
                          "%1 QToolButton { background: %2; }\n"
                          "%1 QToolButton:hover { background: %3; }\n")
        .arg(selector, pill.fill.name(), pill.border.name(), pill.text.name());
}

} // namespace

QString buildStyleSheet(const Roles &roles, const Roles &sidebar, int rem, const QString &images)
{
    const QHash<QString, QColor> colors{
        {QStringLiteral("sidebarBackground"), sidebar.background},
        {QStringLiteral("sidebarText"), sidebar.onSurface},
        {QStringLiteral("sidebarHover"), mix(sidebar.onSurface, 0.08F, sidebar.background)},
        {QStringLiteral("sidebarScroll"), mix(sidebar.outline, 0.8F, sidebar.background)},
        {QStringLiteral("primary"), roles.primary},
        {QStringLiteral("onPrimary"), roles.onPrimary},
        {QStringLiteral("primaryContainer"), roles.primaryContainer},
        {QStringLiteral("onPrimaryContainer"), roles.onPrimaryContainer},
        {QStringLiteral("background"), roles.background},
        {QStringLiteral("surface"), roles.surface},
        {QStringLiteral("onSurface"), roles.onSurface},
        {QStringLiteral("onSurfaceVariant"), roles.onSurfaceVariant},
        {QStringLiteral("surfaceContainerHighest"), roles.surfaceContainerHighest},
        {QStringLiteral("outline"), roles.outline},
        {QStringLiteral("outlineVariant"), roles.outlineVariant},
        {QStringLiteral("error"), roles.error},
        {QStringLiteral("onError"), roles.onError},
        {QStringLiteral("errorContainer"), roles.errorContainer},
        {QStringLiteral("warning"), roles.warning},
        {QStringLiteral("info"), roles.info},
        {QStringLiteral("hint"), mix(roles.onSurface, 0.6F, roles.surface)},
        {QStringLiteral("toastHover"), mix(roles.surface, 0.15F, roles.onSurface)},
        {QStringLiteral("disabled"), mix(roles.onSurface, 0.38F, roles.surface)},
        {QStringLiteral("hover"), mix(roles.onSurface, 0.12F, roles.surface)},
        {QStringLiteral("pressed"), mix(roles.onSurface, 0.18F, roles.surface)},
        {QStringLiteral("primaryHover"), mix(roles.onPrimary, 0.1F, roles.primary)},
        {QStringLiteral("scrollHandle"), mix(roles.outline, 0.6F, roles.surface)},
        {QStringLiteral("warningContainer"), mix(roles.warning, 0.14F, roles.surface)},
        {QStringLiteral("infoContainer"), mix(roles.info, 0.12F, roles.surface)},
        {QStringLiteral("tagFill"), neutralPillColors(roles).fill},
        {QStringLiteral("tagBorder"), neutralPillColors(roles).border},
        {QStringLiteral("tagText"), neutralPillColors(roles).text},
    };

    static const QRegularExpression placeholder(QStringLiteral(R"(\{(\w+)(?:\*([0-9.]+))?\})"));
    const QString source = QString::fromUtf8(Template);
    QString result;
    result.reserve(source.size());
    qsizetype last = 0;
    for (auto it = placeholder.globalMatch(source); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        result += QStringView(source).mid(last, match.capturedStart() - last);
        const QString name = match.captured(1);
        if (name == QLatin1StringView("images")) {
            result += images;
        } else if (name == QLatin1StringView("tagPills")) {
            for (const TagColor color : allTagColors()) {
                result += pillRules(QStringLiteral(R"(QFrame[tagPill="true"][tagColor="%1"])")
                                        .arg(tagColorKey(color)),
                                    pillColors(tagColorKey(color), roles));
            }
        } else if (name == QLatin1StringView("rem")) {
            const double factor = match.hasCaptured(2) ? match.captured(2).toDouble() : 1.0;
            result += QString::number(std::lround(rem * factor));
        } else {
            Q_ASSERT_X(colors.contains(name), "buildStyleSheet", "unknown colour role");
            result += colors.value(name).name();
        }
        last = match.capturedEnd();
    }
    result += QStringView(source).mid(last);
    return result;
}

QString customPillStyleSheet(const QString &colorKey, const Roles &roles)
{
    // A widget's own style sheet outranks the application's, so the focus border is repeated.
    const QString selector = QStringLiteral(R"(QFrame[tagPill="true"])");
    return pillRules(selector, pillColors(colorKey, roles)) +
           QStringLiteral("%1:focus { border-color: %2; }\n").arg(selector, roles.primary.name());
}

} // namespace ui
