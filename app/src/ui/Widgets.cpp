#include "ui/Widgets.h"

#include "ui/TagColor.h"
#include "ui/Theme.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <array>

namespace ui {

namespace {

/// Returns the theme of the running application; widgets exist only after it is created.
const Theme &theme()
{
    Q_ASSERT(Theme::current() != nullptr);
    return *Theme::current();
}

/// Renders a glyph to a pixmap of @p size pixels for a label.
QPixmap glyph(const QString &name, IconTone tone, int size, const QWidget *widget)
{
    return icon(name, tone).pixmap(QSize(size, size), widget->devicePixelRatioF());
}

/// One palette of the theme picker, painted in the palette's own colours.
class PaletteCard : public QAbstractButton
{
public:
    PaletteCard(PaletteId palette, QWidget *parent)
        : QAbstractButton(parent)
        , m_palette(palette)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override
    {
        // Header, strip and a sample card with a title, a muted line and a button row.
        const Theme &t = theme();
        const int title = QFontMetrics(t.font(TextRole::BodySmall)).height();
        const int hint = QFontMetrics(t.font(TextRole::Hint)).height();
        return {t.px(26), t.px(1.5 + 0.5 + 0.6 + 0.4 + 0.4 + 0.4 + 0.75 + 0.4 + 0.75 + 0.3) +
                              title + (3 * hint)};
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        const Theme &t = theme();
        const Roles &current = t.roles();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal radius = t.px(0.75);

        // The card: the selected palette is outlined in the primary colour, as on the web.
        QColor borderColor = current.outlineVariant;
        if (isChecked()) {
            borderColor = current.primary;
        } else if (underMouse()) {
            borderColor = current.outline;
        }
        QPen border(borderColor);
        border.setWidthF(isChecked() || hasFocus() ? 2.0 : 1.0);
        p.setPen(border);
        p.setBrush(current.surface);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), radius, radius);

        const int pad = t.px(0.75);
        const QRect header(pad, pad, width() - (2 * pad), t.px(1.5));
        p.setPen(current.onSurface);
        p.setFont(t.font(TextRole::Label));
        const QString name = paletteName(m_palette);
        p.drawText(header, Qt::AlignVCenter | Qt::AlignLeft, name);
        const int nameWidth = p.fontMetrics().horizontalAdvance(name);
        if (!hasLightVariant(m_palette)) {
            p.setFont(t.font(TextRole::LabelSmall));
            p.setPen(current.onSurfaceVariant);
            p.drawText(header.adjusted(nameWidth + t.px(0.5), 0, 0, 0),
                       Qt::AlignVCenter | Qt::AlignLeft,
                       QCoreApplication::translate("ui::ThemePicker", "Dark only"));
        }
        if (isChecked()) {
            const int size = t.px(1.1);
            ui::icon(QStringLiteral("Check"), IconTone::Primary)
                .paint(&p,
                       QRect(header.right() - size, header.center().y() - (size / 2), size, size));
        }

        QList<Roles> variants;
        if (hasLightVariant(m_palette)) {
            variants.append(roles(m_palette, false));
        }
        variants.append(roles(m_palette, true));
        const int top = header.bottom() + t.px(0.5);
        const int gap = t.px(0.5);
        const int sampleWidth =
            (width() - (2 * pad) - (gap * static_cast<int>(variants.size() - 1))) /
            static_cast<int>(variants.size());
        int left = pad;
        for (const Roles &r : variants) {
            drawSample(p, QRect(left, top, sampleWidth, height() - top - pad), r);
            left += sampleWidth + gap;
        }
    }

private:
    /// Draws a small page of one variant: a strip of its main colours and a card with a title,
    /// a muted line, a primary button and a tag.
    void drawSample(QPainter &p, const QRect &area, const Roles &r) const
    {
        const Theme &t = theme();
        const qreal radius = t.px(0.5);
        p.setPen(r.outlineVariant);
        p.setBrush(r.background);
        p.drawRoundedRect(QRectF(area).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);

        const int pad = t.px(0.4);
        const QRect strip(area.left() + pad, area.top() + pad, area.width() - (2 * pad), t.px(0.6));
        const QList<QColor> colors{r.surface, r.background, r.primary, r.onSurface,
                                   r.success, r.warning,    r.error};
        QPainterPath clip;
        clip.addRoundedRect(QRectF(strip), strip.height() / 2.0, strip.height() / 2.0);
        p.save();
        p.setClipPath(clip);
        const qreal step = strip.width() / static_cast<qreal>(colors.size());
        for (qsizetype i = 0; i < colors.size(); ++i) {
            p.fillRect(QRectF(strip.left() + (step * static_cast<qreal>(i)), strip.top(), step + 1,
                              strip.height()),
                       colors.at(i));
        }
        p.restore();
        p.setPen(r.outlineVariant);
        p.setBrush(Qt::NoBrush);
        p.drawPath(clip);

        const QRect card(area.left() + pad, strip.bottom() + pad, area.width() - (2 * pad),
                         area.bottom() - strip.bottom() - (2 * pad));
        p.setPen(r.outlineVariant);
        p.setBrush(r.surface);
        p.drawRoundedRect(QRectF(card).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);

        const QRect inner = card.adjusted(pad, pad, -pad, -pad);
        QFont title = t.font(TextRole::BodySmall);
        title.setWeight(QFont::DemiBold);
        p.setFont(title);
        p.setPen(r.onSurface);
        const int line = p.fontMetrics().height();
        p.drawText(QRect(inner.left(), inner.top(), inner.width(), line), Qt::AlignLeft,
                   p.fontMetrics().elidedText(
                       QCoreApplication::translate("ui::ThemePicker", "Weekly planning"),
                       Qt::ElideRight, inner.width()));
        p.setFont(t.font(TextRole::Hint));
        p.setPen(r.onSurfaceVariant);
        p.drawText(QRect(inner.left(), inner.top() + line, inner.width(), line), Qt::AlignLeft,
                   p.fontMetrics().elidedText(
                       QCoreApplication::translate("ui::ThemePicker", "Updated today · 3 tags"),
                       Qt::ElideRight, inner.width()));

        const QString save = QCoreApplication::translate("ui::ThemePicker", "Save");
        const int buttonHeight = p.fontMetrics().height() + t.px(0.3);
        const QRect button(inner.left(), inner.top() + (2 * line) + t.px(0.4),
                           p.fontMetrics().horizontalAdvance(save) + t.px(1.0), buttonHeight);
        p.setPen(Qt::NoPen);
        p.setBrush(r.primary);
        p.drawRoundedRect(button, t.px(0.3), t.px(0.3));
        p.setPen(r.onPrimary);
        p.drawText(button, Qt::AlignCenter, save);

        const QString tag = QCoreApplication::translate("ui::ThemePicker", "#work");
        const QRect pill(button.right() + t.px(0.4), button.top(),
                         p.fontMetrics().horizontalAdvance(tag) + t.px(0.8), buttonHeight);
        const QColor green = tagColorValue(TagColor::Green);
        p.setPen(tagBorderColor(green, r));
        p.setBrush(tagFillColor(green, r));
        p.drawRoundedRect(QRectF(pill).adjusted(0.5, 0.5, -0.5, -0.5), buttonHeight / 2.0,
                          buttonHeight / 2.0);
        p.setPen(tagTextColor(green, r));
        p.drawText(pill, Qt::AlignCenter, tag);
    }

    PaletteId m_palette;
};

} // namespace

EmptyState::EmptyState(QWidget *parent)
    : QWidget(parent)
    , m_icon(new QLabel(this))
    , m_title(new QLabel(this))
    , m_text(new QLabel(this))
    , m_action(new QPushButton(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->addStretch();
    for (QLabel *label : {m_icon, m_title, m_text}) {
        label->setAlignment(Qt::AlignCenter);
        layout->addWidget(label);
    }
    m_text->setWordWrap(true);
    setTextRole(m_title, TextRole::SectionTitle);
    setTextRole(m_text, TextRole::Muted);
    m_action->setProperty("role", QStringLiteral("primary"));
    layout->addWidget(m_action, 0, Qt::AlignCenter);
    layout->addStretch();
    connect(m_action, &QPushButton::clicked, this, &EmptyState::actionTriggered);
    connect(Theme::current(), &Theme::changed, this, &EmptyState::updateIcon);
}

void EmptyState::setContent(const QString &iconName, const QString &title, const QString &text,
                            const QString &action)
{
    m_iconName = iconName;
    m_title->setText(title);
    m_text->setText(text);
    m_text->setVisible(!text.isEmpty());
    m_action->setText(action);
    m_action->setVisible(!action.isEmpty());
    updateIcon();
}

void EmptyState::updateIcon()
{
    m_icon->setPixmap(glyph(m_iconName, IconTone::Idle, theme().px(2.5), this));
}

Notice::Notice(QWidget *parent)
    : QFrame(parent)
    , m_icon(new QLabel(this))
    , m_text(new QLabel(this))
    , m_action(new QPushButton(this))
    , m_secondaryAction(new QPushButton(this))
{
    auto *layout = new QHBoxLayout(this);
    const int pad = theme().px(0.75);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->addWidget(m_icon, 0, Qt::AlignTop);
    layout->addWidget(m_text, 1);
    layout->addWidget(m_secondaryAction, 0, Qt::AlignVCenter);
    layout->addWidget(m_action, 0, Qt::AlignVCenter);
    connect(m_secondaryAction, &QPushButton::clicked, this, &Notice::secondaryActionTriggered);
    m_text->setWordWrap(true);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    connect(m_action, &QPushButton::clicked, this, &Notice::actionTriggered);
    connect(Theme::current(), &Theme::changed, this, &Notice::updateIcon);
    hide();
}

void Notice::showNotice(Kind kind, const QString &text, const QString &action,
                        const QString &secondaryAction)
{
    m_kind = kind;
    static constexpr std::array<const char *, 3> Names{"error", "warning", "info"};
    setStyleProperty(this, "notice", QString::fromLatin1(Names.at(static_cast<std::size_t>(kind))));
    m_text->setText(text);
    m_action->setText(action);
    m_action->setVisible(!action.isEmpty());
    m_secondaryAction->setText(secondaryAction);
    m_secondaryAction->setVisible(!secondaryAction.isEmpty());
    updateIcon();
    show();
}

void Notice::updateIcon()
{
    QString name = QStringLiteral("Info");
    if (m_kind == Kind::Error) {
        name = QStringLiteral("Error");
    } else if (m_kind == Kind::Warning) {
        name = QStringLiteral("Warning");
    }
    const IconTone tone = m_kind == Kind::Error ? IconTone::Error : IconTone::Idle;
    m_icon->setPixmap(glyph(name, tone, theme().px(1.25), this));
}

SegmentedControl::SegmentedControl(QWidget *parent)
    : QFrame(parent)
{
    setProperty("segmented", true);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);
    m_group.setExclusive(true);
    connect(&m_group, &QButtonGroup::idClicked, this,
            [this](int id) { Q_EMIT changed(m_values.at(id)); });
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
}

void SegmentedControl::addOption(const QString &text, const QVariant &value)
{
    auto *button = new QToolButton(this);
    button->setText(text);
    button->setCheckable(true);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_group.addButton(button, static_cast<int>(m_values.size()));
    m_values.append(value);
    layout()->addWidget(button);
}

void SegmentedControl::setOptionText(const QVariant &value, const QString &text)
{
    if (QAbstractButton *button = m_group.button(static_cast<int>(m_values.indexOf(value)))) {
        button->setText(text);
    }
}

void SegmentedControl::setValue(const QVariant &value)
{
    if (QAbstractButton *button = m_group.button(static_cast<int>(m_values.indexOf(value)))) {
        button->setChecked(true);
    }
}

QVariant SegmentedControl::value() const
{
    const int id = m_group.checkedId();
    return id < 0 ? QVariant() : m_values.at(id);
}

ThemePicker::ThemePicker(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme().px(0.75));
    m_group.setExclusive(true);
    for (const PaletteId palette : allPalettes()) {
        auto *card = new PaletteCard(palette, this);
        m_group.addButton(card, static_cast<int>(palette));
        layout->addWidget(card);
    }
    connect(&m_group, &QButtonGroup::idClicked, this,
            [this](int id) { Q_EMIT paletteChosen(static_cast<PaletteId>(id)); });
    connect(Theme::current(), &Theme::changed, this, qOverload<>(&QWidget::update));
}

void ThemePicker::setPalette(PaletteId palette)
{
    if (QAbstractButton *button = m_group.button(static_cast<int>(palette))) {
        button->setChecked(true);
    }
}

FlowLayout::FlowLayout(QWidget *parent, int spacing)
    : QLayout(parent)
    , m_gap(spacing)
{
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout()
{
    while (const QLayoutItem *item = takeAt(0)) {
        delete item;
    }
}

void FlowLayout::addItem(QLayoutItem *item)
{
    m_items.append(item);
    // As Qt's own layouts do: a changed item list lays the row out again, also when every
    // widget in it was already visible and none asks for a new layout itself.
    invalidate();
}

int FlowLayout::count() const
{
    return static_cast<int>(m_items.size());
}

QLayoutItem *FlowLayout::itemAt(int index) const
{
    return m_items.value(index);
}

QLayoutItem *FlowLayout::takeAt(int index)
{
    if (index < 0 || index >= m_items.size()) {
        return nullptr;
    }
    invalidate();
    return m_items.takeAt(index);
}

Qt::Orientations FlowLayout::expandingDirections() const
{
    return {};
}

bool FlowLayout::hasHeightForWidth() const
{
    return true;
}

int FlowLayout::heightForWidth(int width) const
{
    return arrange(QRect(0, 0, width, 0), false);
}

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for (const QLayoutItem *item : m_items) {
        size = size.expandedTo(item->minimumSize());
    }
    const QMargins margins = contentsMargins();
    return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
}

QSize FlowLayout::sizeHint() const
{
    return minimumSize();
}

void FlowLayout::setGeometry(const QRect &rect)
{
    QLayout::setGeometry(rect);
    arrange(rect, true);
}

int FlowLayout::arrange(const QRect &rect, bool apply) const
{
    const QRect area = rect.marginsRemoved(contentsMargins());
    // Items are first gathered into rows, so each row can centre its items vertically.
    QList<QList<QLayoutItem *>> rows(1);
    QList<int> heights(1, 0);
    int x = area.x();
    for (QLayoutItem *item : m_items) {
        if (item->isEmpty()) {
            continue;
        }
        const QSize hint = item->sizeHint();
        if (x > area.x() && x + hint.width() > area.right() + 1) {
            rows.append(QList<QLayoutItem *>());
            heights.append(0);
            x = area.x();
        }
        rows.last().append(item);
        heights.last() = std::max(heights.last(), hint.height());
        x += hint.width() + m_gap;
    }
    int y = area.y();
    for (qsizetype row = 0; row < rows.size(); ++row) {
        x = area.x();
        for (QLayoutItem *item : rows.at(row)) {
            const QSize hint = item->sizeHint();
            if (apply) {
                item->setGeometry(
                    QRect(QPoint(x, y + ((heights.at(row) - hint.height()) / 2)),
                          QSize(std::min(hint.width(), area.width()), hint.height())));
            }
            x += hint.width() + m_gap;
        }
        y += heights.at(row) + (row + 1 < rows.size() ? m_gap : 0);
    }
    return y - rect.y() + contentsMargins().bottom();
}

QFrame *settingsSection(QLabel *title, std::initializer_list<QWidget *> rows)
{
    const Theme &theme = *Theme::current();
    auto *section = new QFrame;
    section->setProperty("section", true);
    auto *layout = new QVBoxLayout(section);
    const int pad = theme.px(1.0);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(theme.px(0.6));
    setTextRole(title, TextRole::SectionTitle);
    layout->addWidget(title);
    for (QWidget *row : rows) {
        layout->addWidget(row);
    }
    return section;
}

QWidget *settingsPage(std::initializer_list<QWidget *> sections)
{
    const Theme &theme = *Theme::current();
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(theme.px(1.0), theme.px(1.0), theme.px(1.0), theme.px(1.0));
    layout->setSpacing(theme.px(1.0));
    for (QWidget *section : sections) {
        layout->addWidget(section);
    }
    layout->addStretch();
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    return scroll;
}

QWidget *settingsRow(std::initializer_list<QWidget *> widgets)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    for (QWidget *widget : widgets) {
        layout->addWidget(widget);
    }
    layout->addStretch();
    return row;
}

} // namespace ui
