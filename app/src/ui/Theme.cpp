#include "ui/Theme.h"

#include "ui/StyleSheet.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QPalette>
#include <QProxyStyle>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QWidget>

#include <array>
#include <cmath>

namespace ui {

namespace {

Theme *currentTheme = nullptr;

/// Fusion with a full pointer target for the toolbar overflow button, which Fusion
/// draws only 12 pixels wide.
class Style : public QProxyStyle
{
public:
    Style()
        : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion")))
    {}

    int pixelMetric(PixelMetric metric, const QStyleOption *option,
                    const QWidget *widget) const override
    {
        if (metric == PM_ToolBarExtensionExtent && currentTheme != nullptr) {
            return currentTheme->px(2.25);
        }
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
};

/// Maps the colour roles to a QPalette for widgets and painting that the style sheet does not
/// reach (text selection, links, placeholders, disabled text).
QPalette paletteFor(const Roles &r)
{
    QPalette p;
    const QColor disabled = mix(r.onSurface, 0.38F, r.surface);
    p.setColor(QPalette::Window, r.surface);
    p.setColor(QPalette::WindowText, r.onSurface);
    p.setColor(QPalette::Base, r.surface);
    p.setColor(QPalette::AlternateBase, r.background);
    p.setColor(QPalette::Text, r.onSurface);
    p.setColor(QPalette::Button, r.surface);
    p.setColor(QPalette::ButtonText, r.onSurface);
    p.setColor(QPalette::BrightText, r.error);
    p.setColor(QPalette::Highlight, r.primaryContainer);
    p.setColor(QPalette::HighlightedText, r.onPrimaryContainer);
    p.setColor(QPalette::Link, r.primary);
    p.setColor(QPalette::LinkVisited, r.primary);
    p.setColor(QPalette::PlaceholderText, mix(r.onSurface, 0.6F, r.surface));
    p.setColor(QPalette::ToolTipBase, r.onSurface);
    p.setColor(QPalette::ToolTipText, r.surface);
    p.setColor(QPalette::Accent, r.primary);
    p.setColor(QPalette::Light, r.surface);
    p.setColor(QPalette::Midlight, r.outlineVariant);
    p.setColor(QPalette::Mid, r.outline);
    p.setColor(QPalette::Dark, r.outline);
    p.setColor(QPalette::Shadow, r.outline);
    for (const QPalette::ColorRole role :
         {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        p.setColor(QPalette::Disabled, role, disabled);
    }
    return p;
}

} // namespace

QString themeModeKey(ThemeMode mode)
{
    switch (mode) {
    case ThemeMode::Auto:
        return QStringLiteral("auto");
    case ThemeMode::Light:
        return QStringLiteral("light");
    case ThemeMode::Dark:
        return QStringLiteral("dark");
    }
    return QStringLiteral("auto");
}

std::optional<ThemeMode> themeModeFromKey(const QString &key)
{
    for (const ThemeMode mode : {ThemeMode::Auto, ThemeMode::Light, ThemeMode::Dark}) {
        if (themeModeKey(mode) == key) {
            return mode;
        }
    }
    return std::nullopt;
}

Theme::Theme(QObject *parent)
    : QObject(parent)
    , m_roles(ui::roles(m_palette, false))
    , m_sidebarRoles(ui::roles(m_palette, true))
{
    Q_ASSERT_X(currentTheme == nullptr, "Theme", "only one theme may exist");
    currentTheme = this;
}

Theme::~Theme()
{
    if (currentTheme == this) {
        currentTheme = nullptr;
    }
}

Theme *Theme::current()
{
    return currentTheme;
}

void Theme::setPalette(PaletteId palette)
{
    if (m_palette != palette) {
        m_palette = palette;
        apply();
    }
}

void Theme::setMode(ThemeMode mode)
{
    if (m_mode != mode) {
        m_mode = mode;
        apply();
    }
}

int Theme::rem() const
{
    return QFontInfo(QApplication::font()).pixelSize();
}

int Theme::px(double rems) const
{
    return static_cast<int>(std::lround(rem() * rems));
}

QFont Theme::font(TextRole role) const
{
    QFont f = role == TextRole::Code ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
                                     : QApplication::font();
    const auto size = [this](double rems) { return std::max(1, px(rems)); };
    switch (role) {
    case TextRole::PageTitle:
        f.setPixelSize(size(1.5));
        f.setWeight(QFont::Bold);
        break;
    case TextRole::SectionTitle:
        f.setPixelSize(size(1.15));
        f.setWeight(QFont::DemiBold);
        break;
    case TextRole::BodySmall:
    case TextRole::Muted:
        f.setPixelSize(size(0.92));
        break;
    case TextRole::Hint:
        f.setPixelSize(size(0.85));
        break;
    case TextRole::Label:
        f.setPixelSize(size(1.0));
        f.setWeight(QFont::Medium);
        break;
    case TextRole::LabelSmall:
        f.setPixelSize(size(0.85));
        f.setWeight(QFont::Medium);
        break;
    case TextRole::Body:
    case TextRole::Code:
        f.setPixelSize(size(1.0));
        break;
    }
    return f;
}

QColor Theme::textColor(TextRole role) const
{
    switch (role) {
    case TextRole::Muted:
        return m_roles.onSurfaceVariant;
    case TextRole::Hint:
        return mix(m_roles.onSurface, 0.6F, m_roles.surface);
    default:
        return m_roles.onSurface;
    }
}

void Theme::apply()
{
    if (!m_installed) {
        // Fusion draws the same everywhere, so the look does not depend on the
        // desktop's widget style (sway has none).
        QApplication::setStyle(new Style);
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
                [this] { apply(); });
        m_installed = true;
    }
    m_roles = ui::roles(m_palette, resolveDark());
    m_sidebarRoles = ui::roles(m_palette, true);
    QApplication::setPalette(paletteFor(m_roles));
    qApp->setStyleSheet(buildStyleSheet(m_roles, m_sidebarRoles, rem(), writeIndicatorImages()));
    Q_EMIT changed();
}

bool Theme::resolveDark() const
{
    if (!hasLightVariant(m_palette)) {
        return true;
    }
    switch (m_mode) {
    case ThemeMode::Light:
        return false;
    case ThemeMode::Dark:
        return true;
    case ThemeMode::Auto:
        return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    }
    return false;
}

QString Theme::writeIndicatorImages()
{
    if (!m_images) {
        m_images = std::make_unique<QTemporaryDir>();
    }
    // A folder per colour: the style sheet caches images by path.
    const QString color = m_roles.onPrimary.name();
    const QString folder = m_images->filePath(color.mid(1));
    if (QDir(folder).exists()) {
        return folder;
    }
    QDir().mkpath(folder);
    QFile glyph(QStringLiteral(":/icons/Check.svg"));
    QByteArray check = glyph.open(QIODevice::ReadOnly) ? glyph.readAll() : QByteArray();
    check.replace("currentColor", color.toLatin1());
    const QByteArray dash =
        QStringLiteral(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 16 16\">"
            "<rect x=\"3.5\" y=\"7\" width=\"9\" height=\"2\" rx=\"1\" fill=\"%1\"/></svg>")
            .arg(color)
            .toUtf8();
    for (const auto &[name, data] : {std::pair{QStringLiteral("check.svg"), check},
                                     std::pair{QStringLiteral("dash.svg"), dash}}) {
        QFile file(folder + u'/' + name);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(data);
        }
    }
    return folder;
}

void setTextRole(QWidget *widget, TextRole role)
{
    static constexpr std::array<const char *, 9> Keys{
        "pageTitle", "sectionTitle", "body",       "bodySmall", "muted",
        "hint",      "label",        "labelSmall", "code",
    };
    if (role == TextRole::Code) {
        widget->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    }
    setStyleProperty(widget, "textRole",
                     QString::fromLatin1(Keys.at(static_cast<std::size_t>(role))));
}

void setStyleProperty(QWidget *widget, const char *name, const QVariant &value)
{
    widget->setProperty(name, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

} // namespace ui
