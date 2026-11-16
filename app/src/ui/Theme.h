#pragma once

#include "ui/Palette.h"

#include <QFont>
#include <QObject>
#include <QTemporaryDir>

#include <memory>
#include <optional>

class QWidget;

namespace ui {

/// Whether the interface is light, dark or follows the desktop's colour-scheme preference.
enum class ThemeMode { Auto, Light, Dark };

/// Returns the stable storage key of a mode (`auto`, `light`, `dark`).
QString themeModeKey(ThemeMode mode);

/// Parses a stored mode key; an unknown key gives no value and callers fall back to Auto.
std::optional<ThemeMode> themeModeFromKey(const QString &key);

/// The text roles. Widgets take their size and colour from a role, never from a
/// size chosen for one screen.
enum class TextRole {
    PageTitle,
    SectionTitle,
    Body,
    BodySmall,
    Muted,
    Hint,
    Label,
    LabelSmall,
    Code,
};

/// The interface theme: the chosen palette and mode resolved to colour roles, applied to the
/// whole application as a QPalette and a style sheet generated from the roles.
///
/// One instance exists while the application runs; icons and custom painting read the current
/// roles from it and repaint on changed().
class Theme : public QObject
{
    Q_OBJECT

public:
    /// Creates the theme and makes it the current one; call apply() once the application exists.
    explicit Theme(QObject *parent = nullptr);
    ~Theme() override;

    Theme(const Theme &) = delete;
    Theme &operator=(const Theme &) = delete;

    /// Returns the theme of the running application, or nullptr before one is created.
    static Theme *current();

    PaletteId palette() const { return m_palette; }
    ThemeMode mode() const { return m_mode; }

    /// Chooses the palette and re-applies the theme.
    void setPalette(PaletteId palette);

    /// Chooses the mode and re-applies the theme.
    void setMode(ThemeMode mode);

    /// Returns the colour roles of the variant being shown.
    const Roles &roles() const { return m_roles; }

    /// Returns the colour roles of the main window's sidebar: the palette's dark variant in both
    /// modes, so the sidebar stays dark as in Bear.
    const Roles &sidebarRoles() const { return m_sidebarRoles; }

    /// Returns the root size unit in device-independent pixels: the pixel size of the
    /// application font, so every size follows the person's font setting.
    int rem() const;

    /// Converts a size in rem to device-independent pixels, rounded.
    int px(double rems) const;

    /// Returns the font of a text role.
    QFont font(TextRole role) const;

    /// Returns the text colour of a text role.
    QColor textColor(TextRole role) const;

    /// Applies the palette, the mode and the style sheet to the application and emits changed().
    void apply();

Q_SIGNALS:
    /// Emitted after the theme was applied with different roles or sizes.
    void changed();

private:
    /// Resolves the variant to show: the mode, the desktop preference for Auto, and the
    /// variants the palette has.
    bool resolveDark() const;

    /// Writes the checkbox images in the current `onPrimary` colour and returns their folder.
    QString writeIndicatorImages();

    PaletteId m_palette = PaletteId::Warm;
    ThemeMode m_mode = ThemeMode::Auto;
    Roles m_roles;
    Roles m_sidebarRoles;
    bool m_installed = false;
    /// Checkbox images for the style sheet, one folder per colour.
    std::unique_ptr<QTemporaryDir> m_images;
};

/// Assigns a text role to a label or button: the style sheet gives it the role's font and colour.
void setTextRole(QWidget *widget, TextRole role);

/// Sets a property the style sheet selects on, such as the active state of a filter chip, and
/// applies the matching rules at once.
///
/// @param widget The widget.
/// @param name The property's name.
/// @param value Its new value.
void setStyleProperty(QWidget *widget, const char *name, const QVariant &value);

} // namespace ui
