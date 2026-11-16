#include "ui/Icons.h"

#include "ui/Theme.h"

#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QMutex>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

namespace ui {

namespace {

/// Reads a glyph's SVG once; glyphs are small and few, so they stay in memory. Exports read
/// glyphs on worker threads, so the cache is locked.
QByteArray glyphSource(const QString &name)
{
    static QMutex mutex;
    static QHash<QString, QByteArray> cache;
    const QMutexLocker locker(&mutex);
    const auto found = cache.constFind(name);
    if (found != cache.constEnd()) {
        return *found;
    }
    QFile file(QStringLiteral(":/icons/%1.svg").arg(name));
    QByteArray source;
    if (file.open(QIODevice::ReadOnly)) {
        source = file.readAll();
    } else {
        qWarning("Unknown icon %s", qPrintable(name));
    }
    cache.insert(name, source);
    return source;
}

/// Draws a glyph's SVG source, whose shapes are filled with `currentColor`, in a colour.
void renderGlyph(QPainter &painter, const QRectF &rect, QByteArray svg, const QColor &color)
{
    svg.replace("currentColor", color.name().toLatin1());
    QSvgRenderer renderer(svg);
    renderer.render(&painter, rect);
}

/// Draws a Bootstrap glyph, whose shapes are filled with `currentColor`, in a theme colour.
class TintedIconEngine : public QIconEngine
{
public:
    TintedIconEngine(QByteArray source, IconTone tone, IconSurface surface,
                     std::optional<QColor> color = std::nullopt)
        : m_source(std::move(source))
        , m_tone(tone)
        , m_surface(surface)
        , m_color(color)
    {}

    /// Paints the glyph into @p rect in the colour of @p mode.
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode,
               QIcon::State /*state*/) override
    {
        renderGlyph(*painter, rect, m_source, colorFor(mode));
    }

    /// Renders the glyph at @p size for a screen of @p scale device pixels per pixel.
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override
    {
        QPixmap pixmap(size * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        paint(&painter, QRect(QPoint(0, 0), size), mode, state);
        return pixmap;
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QIconEngine *clone() const override
    {
        return new TintedIconEngine(m_source, m_tone, m_surface, m_color);
    }

    bool isNull() override { return m_source.isEmpty(); }

private:
    /// Resolves the tint of a mode from the current theme's roles.
    QColor colorFor(QIcon::Mode mode) const
    {
        const Theme *theme = Theme::current();
        const bool sidebar = m_surface == IconSurface::Sidebar;
        Roles r = roles(PaletteId::Warm, sidebar);
        if (theme != nullptr) {
            r = sidebar ? theme->sidebarRoles() : theme->roles();
        }
        if (mode == QIcon::Disabled) {
            return r.outline;
        }
        if (mode == QIcon::Selected && m_tone == IconTone::Idle) {
            return r.onPrimaryContainer;
        }
        if (m_color) {
            return tagTextColor(*m_color, r);
        }
        switch (m_tone) {
        case IconTone::Idle:
            return r.onSurfaceVariant;
        case IconTone::Primary:
            return r.primary;
        case IconTone::Error:
            return r.error;
        case IconTone::OnPrimary:
            return r.onPrimary;
        }
        return r.onSurfaceVariant;
    }

    QByteArray m_source;
    IconTone m_tone;
    IconSurface m_surface;
    std::optional<QColor> m_color;
};

} // namespace

QIcon icon(const QString &name, IconTone tone, IconSurface surface)
{
    QByteArray source = glyphSource(name);
    if (source.isEmpty()) {
        return {};
    }
    return QIcon(new TintedIconEngine(std::move(source), tone, surface));
}

QIcon icon(const QString &name, const QColor &color, IconSurface surface)
{
    QByteArray source = glyphSource(name);
    if (source.isEmpty()) {
        return {};
    }
    return QIcon(new TintedIconEngine(std::move(source), IconTone::Idle, surface, color));
}

void paintGlyph(QPainter &painter, const QRectF &rect, const QString &name, const QColor &color)
{
    QByteArray source = glyphSource(name);
    if (!source.isEmpty()) {
        renderGlyph(painter, rect, std::move(source), color);
    }
}

QIcon monochromeIcon(const QString &path, const QColor &color)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray source = file.readAll();
    QIcon result;
    for (const int size : {16, 22, 24, 32, 48, 64, 128}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        renderGlyph(painter, QRectF(0, 0, size, size), source, color);
        painter.end();
        result.addPixmap(pixmap);
    }
    return result;
}

} // namespace ui
