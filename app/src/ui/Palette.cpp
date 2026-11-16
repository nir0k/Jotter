#include "ui/Palette.h"

#include <array>

namespace ui {

namespace {

const QColor White(0xFF, 0xFF, 0xFF);

constexpr std::array Palettes{PaletteId::Warm, PaletteId::GitHub, PaletteId::GitHubDimmed,
                              PaletteId::Monokai};

/// The short list of colours a non-Warm palette is defined by; the roles are derived from it by
/// deriveRoles() with the same rules as Warm.
struct PaletteColors
{
    bool dark = false;
    QColor page;
    QColor card;
    QColor edge;
    QColor raised;
    QColor outline;
    QColor content;
    QColor primary;
    QColor onPrimary;
    QColor tertiary;
    QColor error;
    QColor success;
    QColor warning;
    QColor info;
};

/// Derives the roles of a palette from its colours; secondary roles repeat the primary ones.
Roles deriveRoles(const PaletteColors &p)
{
    const QColor onAccent = p.dark ? QColor(0x0D, 0x11, 0x17) : White;
    Roles r;
    r.dark = p.dark;
    r.primary = p.primary;
    r.onPrimary = p.onPrimary;
    r.primaryContainer = mix(p.primary, 0.18F, p.card);
    r.onPrimaryContainer = mix(p.primary, 0.9F, p.content);
    r.tertiary = p.tertiary;
    r.background = p.page;
    r.surface = p.card;
    r.onSurface = p.content;
    r.onSurfaceVariant = mix(p.content, 0.7F, p.card);
    r.surfaceContainerHighest = p.raised;
    r.outline = p.outline;
    r.outlineVariant = p.dark ? p.raised : p.edge;
    r.error = p.error;
    r.onError = onAccent;
    r.errorContainer = mix(p.error, 0.16F, p.card);
    r.onErrorContainer = mix(p.error, 0.9F, p.content);
    r.success = p.success;
    r.warning = p.warning;
    r.info = p.info;
    return r;
}

/// Returns the light variant of Warm, the default palette and the identity of the family.
Roles warmLight()
{
    const QColor primary(0xC2, 0x41, 0x0C);
    const QColor content(0x18, 0x18, 0x1B);
    const QColor card = White;
    const QColor edge(0xEE, 0xEA, 0xE7);
    const QColor error(0xB9, 0x1C, 0x1C);
    Roles r;
    r.primary = primary;
    r.onPrimary = White;
    r.primaryContainer = mix(primary, 0.14F, card);
    r.onPrimaryContainer = mix(primary, 0.85F, content);
    r.tertiary = QColor(0xB4, 0x53, 0x09);
    r.background = QColor(0xFA, 0xF8, 0xF6);
    r.surface = card;
    r.onSurface = content;
    r.onSurfaceVariant = mix(content, 0.7F, card);
    r.surfaceContainerHighest = edge;
    r.outline = QColor(0xBD, 0xB6, 0xAF);
    r.outlineVariant = edge;
    r.error = error;
    r.onError = White;
    r.errorContainer = mix(error, 0.10F, card);
    r.onErrorContainer = QColor(0x7F, 0x1D, 0x1D);
    r.success = QColor(0x15, 0x80, 0x3D);
    r.warning = QColor(0xFB, 0xBF, 0x24);
    r.info = QColor(0x03, 0x69, 0xA1);
    return r;
}

/// Returns the dark variant of Warm, built on slate surfaces.
Roles warmDark()
{
    const QColor primary(0xFB, 0x92, 0x3C);
    const QColor content(0xEC, 0xF9, 0xFF);
    const QColor card(0x1D, 0x23, 0x2A);
    const QColor raised(0x2A, 0x32, 0x3B);
    const QColor error(0xFF, 0x62, 0x7D);
    Roles r;
    r.dark = true;
    r.primary = primary;
    r.onPrimary = QColor(0x1C, 0x19, 0x17);
    r.primaryContainer = mix(primary, 0.20F, card);
    r.onPrimaryContainer = mix(primary, 0.9F, content);
    r.tertiary = QColor(0xFB, 0xBF, 0x24);
    r.background = QColor(0x19, 0x1E, 0x24);
    r.surface = card;
    r.onSurface = content;
    r.onSurfaceVariant = mix(content, 0.7F, card);
    r.surfaceContainerHighest = raised;
    r.outline = QColor(0x55, 0x60, 0x6C);
    r.outlineVariant = raised;
    r.error = error;
    r.onError = QColor(0x4D, 0x02, 0x18);
    r.errorContainer = mix(error, 0.18F, card);
    r.onErrorContainer = QColor(0xFF, 0xB3, 0xC0);
    r.success = QColor(0x00, 0xD3, 0x90);
    r.warning = QColor(0xFC, 0xB7, 0x00);
    r.info = QColor(0x00, 0xBA, 0xFE);
    return r;
}

} // namespace

QColor mix(const QColor &color, float alpha, const QColor &over)
{
    const auto channel = [alpha](float top, float bottom) {
        return (top * alpha) + (bottom * (1.0F - alpha));
    };
    return QColor::fromRgbF(channel(color.redF(), over.redF()),
                            channel(color.greenF(), over.greenF()),
                            channel(color.blueF(), over.blueF()));
}

std::span<const PaletteId> allPalettes()
{
    return Palettes;
}

QString paletteName(PaletteId id)
{
    switch (id) {
    case PaletteId::Warm:
        return QStringLiteral("Warm");
    case PaletteId::GitHub:
        return QStringLiteral("GitHub");
    case PaletteId::GitHubDimmed:
        return QStringLiteral("GitHub Dimmed");
    case PaletteId::Monokai:
        return QStringLiteral("Monokai");
    }
    return {};
}

QString paletteKey(PaletteId id)
{
    switch (id) {
    case PaletteId::Warm:
        return QStringLiteral("warm");
    case PaletteId::GitHub:
        return QStringLiteral("github");
    case PaletteId::GitHubDimmed:
        return QStringLiteral("github-dimmed");
    case PaletteId::Monokai:
        return QStringLiteral("monokai");
    }
    return QStringLiteral("warm");
}

std::optional<PaletteId> paletteFromKey(const QString &key)
{
    for (const PaletteId id : Palettes) {
        if (paletteKey(id) == key) {
            return id;
        }
    }
    return std::nullopt;
}

bool hasLightVariant(PaletteId id)
{
    return id != PaletteId::GitHubDimmed && id != PaletteId::Monokai;
}

Roles roles(PaletteId id, bool dark)
{
    switch (id) {
    case PaletteId::Warm:
        return dark ? warmDark() : warmLight();
    case PaletteId::GitHub:
        if (dark) {
            return deriveRoles({.dark = true,
                                .page = QColor(0x0D, 0x11, 0x17),
                                .card = QColor(0x16, 0x1B, 0x22),
                                .edge = QColor(0x30, 0x36, 0x3D),
                                .raised = QColor(0x21, 0x26, 0x2D),
                                .outline = QColor(0x6E, 0x76, 0x81),
                                .content = QColor(0xE6, 0xED, 0xF3),
                                .primary = QColor(0x2F, 0x81, 0xF7),
                                .onPrimary = White,
                                .tertiary = QColor(0xDB, 0x6D, 0x28),
                                .error = QColor(0xF8, 0x51, 0x49),
                                .success = QColor(0x3F, 0xB9, 0x50),
                                .warning = QColor(0xD2, 0x99, 0x22),
                                .info = QColor(0x58, 0xA6, 0xFF)});
        }
        return deriveRoles({.dark = false,
                            .page = QColor(0xF6, 0xF8, 0xFA),
                            .card = White,
                            .edge = QColor(0xD0, 0xD7, 0xDE),
                            .raised = QColor(0xEA, 0xEE, 0xF2),
                            .outline = QColor(0x8C, 0x95, 0x9F),
                            .content = QColor(0x1F, 0x23, 0x28),
                            .primary = QColor(0x09, 0x69, 0xDA),
                            .onPrimary = White,
                            .tertiary = QColor(0xBC, 0x4C, 0x00),
                            .error = QColor(0xCF, 0x22, 0x2E),
                            .success = QColor(0x1A, 0x7F, 0x37),
                            .warning = QColor(0x9A, 0x67, 0x00),
                            .info = QColor(0x09, 0x69, 0xDA)});
    case PaletteId::GitHubDimmed:
        return deriveRoles({.dark = true,
                            .page = QColor(0x22, 0x27, 0x2E),
                            .card = QColor(0x2D, 0x33, 0x3B),
                            .edge = QColor(0x44, 0x4C, 0x56),
                            .raised = QColor(0x37, 0x3E, 0x47),
                            .outline = QColor(0x76, 0x83, 0x90),
                            .content = QColor(0xAD, 0xBA, 0xC7),
                            .primary = QColor(0x53, 0x9B, 0xF5),
                            .onPrimary = QColor(0x22, 0x27, 0x2E),
                            .tertiary = QColor(0xCC, 0x6B, 0x2C),
                            .error = QColor(0xE5, 0x53, 0x4B),
                            .success = QColor(0x57, 0xAB, 0x5A),
                            .warning = QColor(0xC6, 0x90, 0x26),
                            .info = QColor(0x6C, 0xB6, 0xFF)});
    case PaletteId::Monokai: {
        // The colours of VS Code's built-in Monokai theme: its sidebar, editor, borders and text,
        // with one syntax colour per role so that no two roles share a hue.
        Roles r = deriveRoles({.dark = true,
                               .page = QColor(0x1E, 0x1F, 0x1C),
                               .card = QColor(0x27, 0x28, 0x22),
                               .edge = QColor(0x41, 0x43, 0x39),
                               .raised = QColor(0x41, 0x43, 0x39),
                               .outline = QColor(0x75, 0x71, 0x5E),
                               .content = QColor(0xF8, 0xF8, 0xF2),
                               .primary = QColor(0xFD, 0x97, 0x1F),
                               .onPrimary = QColor(0x27, 0x28, 0x22),
                               .tertiary = QColor(0xAE, 0x81, 0xFF),
                               .error = QColor(0xF9, 0x26, 0x72),
                               .success = QColor(0xA6, 0xE2, 0x2E),
                               .warning = QColor(0xE6, 0xDB, 0x74),
                               .info = QColor(0x66, 0xD9, 0xEF)});
        r.syntaxTheme = QStringLiteral("Monokai");
        return r;
    }
    }
    return warmLight();
}

} // namespace ui
