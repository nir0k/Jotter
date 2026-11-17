#include "ui/Popups.h"

#include "core/TagColors.h"
#include "ui/Icons.h"
#include "ui/TagColor.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QCoreApplication>
#include <QDialog>
#include <QEvent>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <array>
#include <span>

namespace ui {

void showNear(QWidget *popup, QWidget *anchor)
{
    popup->adjustSize();
    // The popup opens below its button, or above it when it would leave the window, as from
    // the formatting toolbar at the bottom of the editor.
    const QWidget *window = anchor->window();
    const int windowBottom = window->mapToGlobal(QPoint(0, window->height())).y();
    QPoint position = anchor->mapToGlobal(QPoint(0, anchor->height()));
    if (position.y() + popup->height() > windowBottom) {
        position.setY(anchor->mapToGlobal(QPoint()).y() - popup->height());
    }
    showOnScreen(popup, position);
}

void showOnScreen(QWidget *popup, QPoint position)
{
    if (const QScreen *screen = QGuiApplication::screenAt(position)) {
        const QRect area = screen->availableGeometry();
        position.setX(std::min(position.x(), area.right() - popup->width()));
        position.setY(std::min(position.y(), area.bottom() - popup->height()));
    }
    popup->move(position);
    popup->show();
}

namespace {

/// Messages stay this long unless replaced or acted on.
constexpr int ToastMs = 5000;

/// Glyph columns of the icon picker.
constexpr int PickerColumns = 9;

/// The icon picker's glyph area shows this many rem of glyphs and scrolls beyond them.
constexpr double PickerHeightRem = 16;

/// A glyph of the icon picker with the name it is shown and searched by.
struct PickerGlyph
{
    const char *name;
    const char *label;
};

/// A titled group of the icon picker.
struct PickerGroup
{
    const char *title;
    std::span<const PickerGlyph> glyphs;
};

constexpr auto GeneralGlyphs = std::to_array<PickerGlyph>({
    {"Note", QT_TRANSLATE_NOOP("ui::IconPicker", "Note")},
    {"Document", QT_TRANSLATE_NOOP("ui::IconPicker", "Document")},
    {"Folder", QT_TRANSLATE_NOOP("ui::IconPicker", "Folder")},
    {"Bookmark", QT_TRANSLATE_NOOP("ui::IconPicker", "Bookmark")},
    {"Heart", QT_TRANSLATE_NOOP("ui::IconPicker", "Heart")},
    {"Flag", QT_TRANSLATE_NOOP("ui::IconPicker", "Flag")},
    {"Tag", QT_TRANSLATE_NOOP("ui::IconPicker", "Tag")},
    {"Lightbulb", QT_TRANSLATE_NOOP("ui::IconPicker", "Idea")},
    {"Pin", QT_TRANSLATE_NOOP("ui::IconPicker", "Pin")},
    {"Inbox", QT_TRANSLATE_NOOP("ui::IconPicker", "Inbox")},
    {"Archive", QT_TRANSLATE_NOOP("ui::IconPicker", "Archive")},
    {"Checklist", QT_TRANSLATE_NOOP("ui::IconPicker", "Checklist")},
});

constexpr auto WorkGlyphs = std::to_array<PickerGlyph>({
    {"Briefcase", QT_TRANSLATE_NOOP("ui::IconPicker", "Work")},
    {"Calendar", QT_TRANSLATE_NOOP("ui::IconPicker", "Calendar")},
    {"Time", QT_TRANSLATE_NOOP("ui::IconPicker", "Clock")},
    {"Kanban", QT_TRANSLATE_NOOP("ui::IconPicker", "Kanban board")},
    {"Clipboard", QT_TRANSLATE_NOOP("ui::IconPicker", "Tasks")},
    {"Chart", QT_TRANSLATE_NOOP("ui::IconPicker", "Chart")},
    {"Calculator", QT_TRANSLATE_NOOP("ui::IconPicker", "Calculator")},
    {"People", QT_TRANSLATE_NOOP("ui::IconPicker", "People")},
    {"Profile", QT_TRANSLATE_NOOP("ui::IconPicker", "Person")},
    {"Chat", QT_TRANSLATE_NOOP("ui::IconPicker", "Chat")},
    {"Envelope", QT_TRANSLATE_NOOP("ui::IconPicker", "Mail")},
    {"Phone", QT_TRANSLATE_NOOP("ui::IconPicker", "Phone")},
    {"Mortarboard", QT_TRANSLATE_NOOP("ui::IconPicker", "Study")},
    {"Book", QT_TRANSLATE_NOOP("ui::IconPicker", "Book")},
    {"Journal", QT_TRANSLATE_NOOP("ui::IconPicker", "Journal")},
    {"Bookshelf", QT_TRANSLATE_NOOP("ui::IconPicker", "Library")},
    {"Pen", QT_TRANSLATE_NOOP("ui::IconPicker", "Pen")},
});

constexpr auto ProgrammingGlyphs = std::to_array<PickerGlyph>({
    {"Code", QT_TRANSLATE_NOOP("ui::IconPicker", "Code")},
    {"Terminal", QT_TRANSLATE_NOOP("ui::IconPicker", "Terminal")},
    {"Braces", QT_TRANSLATE_NOOP("ui::IconPicker", "Braces")},
    {"Bug", QT_TRANSLATE_NOOP("ui::IconPicker", "Bug")},
    {"Database", QT_TRANSLATE_NOOP("ui::IconPicker", "Database")},
    {"Rocket", QT_TRANSLATE_NOOP("ui::IconPicker", "Rocket")},
    {"Key", QT_TRANSLATE_NOOP("ui::IconPicker", "Key")},
    {"Lock", QT_TRANSLATE_NOOP("ui::IconPicker", "Lock")},
});

constexpr auto DeviceGlyphs = std::to_array<PickerGlyph>({
    {"Desktop", QT_TRANSLATE_NOOP("ui::IconPicker", "Computer")},
    {"Laptop", QT_TRANSLATE_NOOP("ui::IconPicker", "Laptop")},
    {"Server", QT_TRANSLATE_NOOP("ui::IconPicker", "Server")},
    {"HardDrive", QT_TRANSLATE_NOOP("ui::IconPicker", "Hard drive")},
    {"Cpu", QT_TRANSLATE_NOOP("ui::IconPicker", "Processor")},
    {"Memory", QT_TRANSLATE_NOOP("ui::IconPicker", "Memory")},
    {"Wifi", QT_TRANSLATE_NOOP("ui::IconPicker", "Wi-Fi")},
    {"UsbDrive", QT_TRANSLATE_NOOP("ui::IconPicker", "USB drive")},
    {"Keyboard", QT_TRANSLATE_NOOP("ui::IconPicker", "Keyboard")},
    {"Printer", QT_TRANSLATE_NOOP("ui::IconPicker", "Printer")},
    {"Smartphone", QT_TRANSLATE_NOOP("ui::IconPicker", "Smartphone")},
    {"Tablet", QT_TRANSLATE_NOOP("ui::IconPicker", "Tablet")},
    {"Headphones", QT_TRANSLATE_NOOP("ui::IconPicker", "Headphones")},
    {"Microphone", QT_TRANSLATE_NOOP("ui::IconPicker", "Microphone")},
    {"Tv", QT_TRANSLATE_NOOP("ui::IconPicker", "TV")},
    {"Camera", QT_TRANSLATE_NOOP("ui::IconPicker", "Camera")},
    {"Antenna", QT_TRANSLATE_NOOP("ui::IconPicker", "Antenna")},
});

constexpr auto SportGlyphs = std::to_array<PickerGlyph>({
    {"Run", QT_TRANSLATE_NOOP("ui::IconPicker", "Running")},
    {"Walk", QT_TRANSLATE_NOOP("ui::IconPicker", "Walking")},
    {"Hiking", QT_TRANSLATE_NOOP("ui::IconPicker", "Hiking")},
    {"Gym", QT_TRANSLATE_NOOP("ui::IconPicker", "Gym")},
    {"Bike", QT_TRANSLATE_NOOP("ui::IconPicker", "Cycling")},
    {"Swim", QT_TRANSLATE_NOOP("ui::IconPicker", "Swimming")},
    {"Stopwatch", QT_TRANSLATE_NOOP("ui::IconPicker", "Stopwatch")},
    {"Trophy", QT_TRANSLATE_NOOP("ui::IconPicker", "Trophy")},
    {"Backpack", QT_TRANSLATE_NOOP("ui::IconPicker", "Backpack")},
    {"Compass", QT_TRANSLATE_NOOP("ui::IconPicker", "Compass")},
});

constexpr auto TravelGlyphs = std::to_array<PickerGlyph>({
    {"Suitcase", QT_TRANSLATE_NOOP("ui::IconPicker", "Vacation")},
    {"Airplane", QT_TRANSLATE_NOOP("ui::IconPicker", "Flight")},
    {"Train", QT_TRANSLATE_NOOP("ui::IconPicker", "Train")},
    {"Bus", QT_TRANSLATE_NOOP("ui::IconPicker", "Bus")},
    {"CarFront", QT_TRANSLATE_NOOP("ui::IconPicker", "Car")},
    {"Ticket", QT_TRANSLATE_NOOP("ui::IconPicker", "Ticket")},
    {"Globe", QT_TRANSLATE_NOOP("ui::IconPicker", "World")},
    {"Map", QT_TRANSLATE_NOOP("ui::IconPicker", "Map")},
    {"Place", QT_TRANSLATE_NOOP("ui::IconPicker", "Place")},
    {"Umbrella", QT_TRANSLATE_NOOP("ui::IconPicker", "Umbrella")},
    {"LifeBuoy", QT_TRANSLATE_NOOP("ui::IconPicker", "Lifebuoy")},
});

constexpr auto HomeGlyphs = std::to_array<PickerGlyph>({
    {"Home", QT_TRANSLATE_NOOP("ui::IconPicker", "Home")},
    {"Bed", QT_TRANSLATE_NOOP("ui::IconPicker", "Bedroom")},
    {"Sofa", QT_TRANSLATE_NOOP("ui::IconPicker", "Living room")},
    {"Lamp", QT_TRANSLATE_NOOP("ui::IconPicker", "Lamp")},
    {"Recycle", QT_TRANSLATE_NOOP("ui::IconPicker", "Recycling")},
    {"Basket", QT_TRANSLATE_NOOP("ui::IconPicker", "Basket")},
    {"Bag", QT_TRANSLATE_NOOP("ui::IconPicker", "Bag")},
    {"Tools", QT_TRANSLATE_NOOP("ui::IconPicker", "Tools")},
    {"PaintBucket", QT_TRANSLATE_NOOP("ui::IconPicker", "Painting")},
    {"Scissors", QT_TRANSLATE_NOOP("ui::IconPicker", "Scissors")},
});

constexpr auto FoodGlyphs = std::to_array<PickerGlyph>({
    {"ForkKnife", QT_TRANSLATE_NOOP("ui::IconPicker", "Food")},
    {"CookingPot", QT_TRANSLATE_NOOP("ui::IconPicker", "Cooking")},
    {"CupHot", QT_TRANSLATE_NOOP("ui::IconPicker", "Coffee")},
    {"Cake", QT_TRANSLATE_NOOP("ui::IconPicker", "Cake")},
    {"Gift", QT_TRANSLATE_NOOP("ui::IconPicker", "Gift")},
    {"Balloon", QT_TRANSLATE_NOOP("ui::IconPicker", "Party")},
});

constexpr auto HobbyGlyphs = std::to_array<PickerGlyph>({
    {"Palette", QT_TRANSLATE_NOOP("ui::IconPicker", "Art")},
    {"Brush", QT_TRANSLATE_NOOP("ui::IconPicker", "Brush")},
    {"Image", QT_TRANSLATE_NOOP("ui::IconPicker", "Picture")},
    {"Music", QT_TRANSLATE_NOOP("ui::IconPicker", "Music")},
    {"Film", QT_TRANSLATE_NOOP("ui::IconPicker", "Film")},
    {"Gamepad", QT_TRANSLATE_NOOP("ui::IconPicker", "Games")},
    {"Dice", QT_TRANSLATE_NOOP("ui::IconPicker", "Board games")},
    {"Puzzle", QT_TRANSLATE_NOOP("ui::IconPicker", "Puzzle")},
    {"Feather", QT_TRANSLATE_NOOP("ui::IconPicker", "Writing")},
    {"Gem", QT_TRANSLATE_NOOP("ui::IconPicker", "Gem")},
});

constexpr auto MoneyGlyphs = std::to_array<PickerGlyph>({
    {"Money", QT_TRANSLATE_NOOP("ui::IconPicker", "Money")},
    {"Wallet", QT_TRANSLATE_NOOP("ui::IconPicker", "Wallet")},
    {"CreditCard", QT_TRANSLATE_NOOP("ui::IconPicker", "Bank card")},
    {"Bank", QT_TRANSLATE_NOOP("ui::IconPicker", "Bank")},
    {"Cart", QT_TRANSLATE_NOOP("ui::IconPicker", "Shopping")},
    {"Receipt", QT_TRANSLATE_NOOP("ui::IconPicker", "Receipt")},
});

constexpr auto HealthGlyphs = std::to_array<PickerGlyph>({
    {"HeartPulse", QT_TRANSLATE_NOOP("ui::IconPicker", "Health")},
    {"Capsule", QT_TRANSLATE_NOOP("ui::IconPicker", "Medicine")},
    {"Bandaid", QT_TRANSLATE_NOOP("ui::IconPicker", "Bandage")},
    {"Hospital", QT_TRANSLATE_NOOP("ui::IconPicker", "Hospital")},
});

constexpr auto NatureGlyphs = std::to_array<PickerGlyph>({
    {"Flower", QT_TRANSLATE_NOOP("ui::IconPicker", "Flower")},
    {"Tree", QT_TRANSLATE_NOOP("ui::IconPicker", "Tree")},
    {"LightMode", QT_TRANSLATE_NOOP("ui::IconPicker", "Sun")},
    {"DarkMode", QT_TRANSLATE_NOOP("ui::IconPicker", "Moon")},
    {"Cloud", QT_TRANSLATE_NOOP("ui::IconPicker", "Cloud")},
    {"Rain", QT_TRANSLATE_NOOP("ui::IconPicker", "Rain")},
    {"Snow", QT_TRANSLATE_NOOP("ui::IconPicker", "Snow")},
    {"Water", QT_TRANSLATE_NOOP("ui::IconPicker", "Water")},
    {"Fire", QT_TRANSLATE_NOOP("ui::IconPicker", "Fire")},
});

/// The groups of the icon picker, in their order.
constexpr auto PickerGroups = std::to_array<PickerGroup>({
    {QT_TRANSLATE_NOOP("ui::IconPicker", "General"), GeneralGlyphs},
    {QT_TRANSLATE_NOOP("ui::IconPicker", "Work and study"), WorkGlyphs},
    {QT_TRANSLATE_NOOP("ui::IconPicker", "Programming"), ProgrammingGlyphs},
    {QT_TRANSLATE_NOOP("ui::IconPicker", "Devices and sensors"), DeviceGlyphs},
    {QT_TRANSLATE_NOOP("ui::IconPicker", "Sport and activity"), SportGlyphs},
    {QT_TRANSLATE_NOOP("ui::IconPicker", "Travel and leisure"), TravelGlyphs},
    {QT_TRANSLATE_NOOP("ui::IconPicker", "Home and cleaning"), HomeGlyphs},
    {QT_TRANSLATE_NOOP("ui::IconPicker", "Food and celebrations"), FoodGlyphs},
    {QT_TRANSLATE_NOOP("ui::IconPicker", "Hobbies and creativity"), HobbyGlyphs},
    {QT_TRANSLATE_NOOP("ui::IconPicker", "Money and shopping"), MoneyGlyphs},
    {QT_TRANSLATE_NOOP("ui::IconPicker", "Health"), HealthGlyphs},
    {QT_TRANSLATE_NOOP("ui::IconPicker", "Nature and weather"), NatureGlyphs},
});

/// Translates a text of the icon picker marked with QT_TRANSLATE_NOOP.
QString pickerText(const char *source)
{
    return QCoreApplication::translate("ui::IconPicker", source);
}

/// Colour tiles per row: Default and the twenty-nine tag colours make three rows.
constexpr int ColorColumns = 10;

/// A multiple-choice list longer than this gets a search field.
constexpr int SearchableChoices = 8;

/// Draws a colour tile as what the colour does: a letter in it, a letter on it, or the colour.
QIcon colorTile(const std::optional<QColor> &color, ColorGrid::Style style, int size)
{
    const Theme &theme = *Theme::current();
    const Roles &r = theme.roles();
    QPixmap pixmap(size * 2, size * 2);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF tile(1, 1, size - 2, size - 2);
    const qreal radius = theme.px(0.3);
    QColor fill = r.surface;
    QColor letter = r.onSurface;
    if (color && style == ColorGrid::Style::Swatch) {
        fill = tagTextColor(*color, r);
    } else if (color && style == ColorGrid::Style::Highlight) {
        fill = tagHighlightColor(*color, r);
    } else if (color) {
        letter = tagTextColor(*color, r);
    }
    painter.setPen(QPen(color && style == ColorGrid::Style::Swatch ? fill : r.outline, 1.0));
    painter.setBrush(fill);
    painter.drawRoundedRect(tile, radius, radius);
    if (style != ColorGrid::Style::Swatch) {
        QFont font = theme.font(TextRole::Label);
        font.setWeight(QFont::Bold);
        font.setPixelSize(std::max(1, size / 2));
        painter.setFont(font);
        painter.setPen(letter);
        painter.drawText(tile, Qt::AlignCenter, QStringLiteral("A"));
    }
    return {pixmap};
}

QToolButton *pickerButton(QWidget *parent, const QIcon &icon, const QString &tip, bool checked)
{
    auto *button = new QToolButton(parent);
    button->setIcon(icon);
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    button->setCheckable(true);
    button->setChecked(checked);
    button->setAutoRaise(true);
    return button;
}

} // namespace

Toast::Toast(QWidget *host)
    : QFrame(host)
    , m_text(new QLabel(this))
    , m_action(new QPushButton(this))
{
    setProperty("toast", true);
    setAttribute(Qt::WA_StyledBackground);
    auto *layout = new QHBoxLayout(this);
    const Theme &theme = *Theme::current();
    layout->setContentsMargins(theme.px(1.0), theme.px(0.5), theme.px(0.5), theme.px(0.5));
    layout->setSpacing(theme.px(1.0));
    layout->addWidget(m_text, 1);
    layout->addWidget(m_action);
    m_action->setFlat(true);
    m_hide.setSingleShot(true);
    m_hide.setInterval(ToastMs);
    connect(&m_hide, &QTimer::timeout, this, &QWidget::hide);
    connect(m_action, &QPushButton::clicked, this, [this] {
        hide();
        if (m_onAction) {
            const auto action = std::exchange(m_onAction, {});
            action();
        }
    });
    host->installEventFilter(this);
    hide();
}

void Toast::showMessage(const QString &text, const QString &action, std::function<void()> onAction)
{
    m_text->setText(text);
    m_action->setText(action);
    m_action->setVisible(!action.isEmpty());
    m_onAction = std::move(onAction);
    adjustSize();
    place();
    show();
    raise();
    m_hide.start();
}

bool Toast::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parent() && event->type() == QEvent::Resize && isVisible()) {
        place();
    }
    return QFrame::eventFilter(watched, event);
}

void Toast::place()
{
    const auto *host = parentWidget();
    const int margin = Theme::current()->px(1.5);
    const int width = std::min(sizeHint().width(), host->width() - (2 * margin));
    resize(width, sizeHint().height());
    move((host->width() - width) / 2, host->height() - height() - margin);
}

ColorGrid::ColorGrid(Style style, QWidget *parent)
    : QWidget(parent)
    , m_style(style)
    , m_grid(new QGridLayout(this))
{
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(Theme::current()->px(0.2));
    connect(Theme::current(), &Theme::changed, this, &ColorGrid::rebuild);
    rebuild();
}

void ColorGrid::setCurrent(const QString &key)
{
    m_current = key;
    rebuild();
}

void ColorGrid::rebuild()
{
    while (const QLayoutItem *item = m_grid->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const int size = Theme::current()->px(2.0);
    const auto add = [&](const std::optional<TagColor> &color, int index) {
        const QString key = color ? tagColorKey(*color) : QString();
        const QString name =
            color ? tagColorName(*color) : QCoreApplication::translate("ui::ColorGrid", "Default");
        const auto value = color ? std::optional(tagColorValue(*color)) : std::nullopt;
        auto *button = pickerButton(this, colorTile(value, m_style, size), name, key == m_current);
        button->setProperty("tile", true);
        button->setIconSize(QSize(size, size));
        connect(button, &QToolButton::clicked, this, [this, key] { Q_EMIT chosen(key); });
        m_grid->addWidget(button, index / ColorColumns, index % ColorColumns);
    };
    add(std::nullopt, 0);
    int index = 1;
    for (const TagColor color : allTagColors()) {
        add(color, index++);
    }

    // Below the tiles, a custom colour from the colour dialog; while one is current, the button
    // shows it and is marked.
    const auto custom = core::customColorValue(m_current);
    const QString name = QCoreApplication::translate("ui::ColorGrid", "Custom colour…");
    auto *button = pickerButton(this,
                                custom ? colorTile(QColor::fromRgb(*custom), m_style, size)
                                       : icon(QStringLiteral("Palette")),
                                name, custom.has_value());
    button->setText(name);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setIconSize(QSize(size, size));
    connect(button, &QToolButton::clicked, this, [this] {
        // The popup holding the grid closes first; the dialog belongs to the window under it.
        QWidget *popup = window();
        QWidget *host = popup->parentWidget() != nullptr ? popup->parentWidget()->window() : popup;
        popup->hide();
        auto *dialog = new QColorDialog(colorFromKey(m_current).value_or(QColor(Qt::white)), host);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &QColorDialog::colorSelected, this,
                [this](const QColor &color) { Q_EMIT chosen(color.name()); });
        dialog->open();
    });
    m_grid->addWidget(button, (index + ColorColumns - 1) / ColorColumns, 0, 1, ColorColumns,
                      Qt::AlignLeft);
}

ColorPopup::ColorPopup(ColorGrid::Style style, QWidget *parent)
    : QFrame(parent, Qt::Popup)
    , m_grid(new ColorGrid(style, this))
{
    setProperty("popup", true);
    setAttribute(Qt::WA_StyledBackground);
    auto *layout = new QVBoxLayout(this);
    const int pad = Theme::current()->px(0.6);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->addWidget(m_grid);
    connect(m_grid, &ColorGrid::chosen, this, [this](const QString &key) {
        hide();
        Q_EMIT chosen(key);
    });
}

void ColorPopup::open(QWidget *anchor, const QString &current)
{
    m_grid->setCurrent(current);
    showNear(this, anchor);
}

void ColorPopup::openAt(const QPoint &globalPosition, const QString &current)
{
    m_grid->setCurrent(current);
    adjustSize();
    showOnScreen(this, globalPosition);
}

/// The cells of the table size popup, drawn by hand so the choice follows the pointer.
class TableSizePopup::Grid : public QWidget
{
public:
    static constexpr int Cells = 6;

    explicit Grid(QWidget *parent)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        const int side = (Cells * step()) - gap();
        setFixedSize(side, side);
    }

    /// Called with the size under the pointer or the arrow keys.
    std::function<void(int, int)> onHover;
    /// Called with the chosen size.
    std::function<void(int, int)> onChoose;

    void reset()
    {
        m_rows = 1;
        m_columns = 1;
        update();
        onHover(m_rows, m_columns);
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        const Roles &r = Theme::current()->roles();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const int cell = step() - gap();
        for (int row = 0; row < Cells; ++row) {
            for (int column = 0; column < Cells; ++column) {
                const bool lit = row < m_rows && column < m_columns;
                painter.setPen(QPen(lit ? r.primary : r.outline, 1));
                painter.setBrush(lit ? r.primaryContainer : r.surface);
                painter.drawRoundedRect(QRectF(column * step(), row * step(), cell, cell)
                                            .adjusted(0.5, 0.5, -0.5, -0.5),
                                        2, 2);
            }
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPoint at = event->position().toPoint();
        setSize((at.y() / step()) + 1, (at.x() / step()) + 1);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            onChoose(m_rows, m_columns);
        }
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        switch (event->key()) {
        case Qt::Key_Left:
            setSize(m_rows, m_columns - 1);
            break;
        case Qt::Key_Right:
            setSize(m_rows, m_columns + 1);
            break;
        case Qt::Key_Up:
            setSize(m_rows - 1, m_columns);
            break;
        case Qt::Key_Down:
            setSize(m_rows + 1, m_columns);
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            onChoose(m_rows, m_columns);
            break;
        default:
            QWidget::keyPressEvent(event);
        }
    }

private:
    static int gap() { return std::max(2, Theme::current()->px(0.15)); }
    static int step() { return Theme::current()->px(1.25) + gap(); }

    void setSize(int rows, int columns)
    {
        rows = std::clamp(rows, 1, Cells);
        columns = std::clamp(columns, 1, Cells);
        if (rows != m_rows || columns != m_columns) {
            m_rows = rows;
            m_columns = columns;
            update();
            onHover(m_rows, m_columns);
        }
    }

    int m_rows = 1;
    int m_columns = 1;
};

TableSizePopup::TableSizePopup(QWidget *parent)
    : QFrame(parent, Qt::Popup)
    , m_grid(new Grid(this))
    , m_size(new QLabel(this))
    , m_header(new QCheckBox(this))
{
    setProperty("popup", true);
    setAttribute(Qt::WA_StyledBackground);
    const Theme &theme = *Theme::current();
    auto *layout = new QVBoxLayout(this);
    const int pad = theme.px(0.75);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(theme.px(0.5));
    layout->addWidget(m_grid);
    auto *row = new QHBoxLayout;
    setTextRole(m_size, TextRole::LabelSmall);
    row->addWidget(m_size);
    row->addStretch();
    row->addWidget(m_header);
    layout->addLayout(row);
    m_header->setChecked(true);
    m_grid->onHover = [this](int rows, int columns) {
        //: The size of a new table: rows × columns.
        m_size->setText(tr("%1 × %2").arg(rows).arg(columns));
    };
    m_grid->onChoose = [this](int rows, int columns) {
        hide();
        Q_EMIT chosen(rows, columns, m_header->isChecked());
    };
    retranslate();
}

void TableSizePopup::open(QWidget *anchor)
{
    m_grid->reset();
    showNear(this, anchor);
    m_grid->setFocus();
}

void TableSizePopup::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QFrame::changeEvent(event);
}

void TableSizePopup::retranslate()
{
    m_header->setText(tr("Header row"));
}

IconPicker::IconPicker(QWidget *parent)
    : QFrame(parent, Qt::Popup)
    , m_colorTitle(new QLabel(this))
    , m_iconTitle(new QLabel(this))
    , m_colors(new ColorGrid(ColorGrid::Style::Swatch, this))
    , m_search(new QLineEdit(this))
    , m_scroll(new QScrollArea(this))
    , m_content(new QWidget)
    , m_none(pickerButton(m_content, icon(QStringLiteral("Close")), {}, false))
    , m_icons(new QGridLayout)
    , m_noMatches(new QLabel(m_content))
{
    setProperty("popup", true);
    setAttribute(Qt::WA_StyledBackground);
    const Theme &theme = *Theme::current();
    const int glyphSize = theme.px(1.5);
    auto *layout = new QVBoxLayout(this);
    const int pad = theme.px(0.75);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(theme.px(0.4));
    setTextRole(m_colorTitle, TextRole::LabelSmall);
    setTextRole(m_iconTitle, TextRole::LabelSmall);
    m_search->setProperty("search", true);
    m_search->setClearButtonEnabled(true);
    m_search->addAction(icon(QStringLiteral("Search")), QLineEdit::LeadingPosition);
    m_search->installEventFilter(this);
    m_none->setIconSize(QSize(glyphSize, glyphSize));
    layout->addWidget(m_colorTitle);
    layout->addWidget(m_colors);
    layout->addSpacing(theme.px(0.4));
    layout->addWidget(m_iconTitle);
    layout->addWidget(m_search);
    layout->addWidget(m_scroll);

    // The grid sits at the top of the scrolled area, so a short list of matches does not spread
    // over its height.
    auto *contentLayout = new QVBoxLayout(m_content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->addLayout(m_icons);
    contentLayout->addStretch();
    m_icons->setSpacing(theme.px(0.15));
    m_content->setAttribute(Qt::WA_StyledBackground);
    m_content->setProperty("popupContent", true);
    setTextRole(m_noMatches, TextRole::Hint);
    int group = 0;
    for (const PickerGroup &pickerGroup : PickerGroups) {
        auto *title = new QLabel(m_content);
        setTextRole(title, TextRole::LabelSmall);
        if (group > 0) {
            title->setContentsMargins(0, theme.px(0.4), 0, 0);
        }
        m_groupTitles.append(title);
        for (const PickerGlyph &glyph : pickerGroup.glyphs) {
            const QString name = QString::fromLatin1(glyph.name);
            QToolButton *button = pickerButton(m_content, {}, {}, false);
            button->setIconSize(QSize(glyphSize, glyphSize));
            connect(button, &QToolButton::clicked, this, [this, name] {
                Q_EMIT iconChosen(name);
                hide();
            });
            m_buttons.append({button, name, glyph.label, group});
        }
        ++group;
    }
    m_scroll->setWidget(m_content);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->viewport()->setAutoFillBackground(false);
    filter({});
    m_scroll->setFixedSize(m_content->sizeHint().width() +
                               m_scroll->verticalScrollBar()->sizeHint().width(),
                           theme.px(PickerHeightRem));

    connect(m_colors, &ColorGrid::chosen, this, [this](const QString &key) {
        m_color = key;
        Q_EMIT colorChosen(key);
        refresh();
    });
    connect(m_none, &QToolButton::clicked, this, [this] {
        Q_EMIT iconChosen(QString());
        hide();
    });
    connect(m_search, &QLineEdit::textChanged, this, &IconPicker::filter);
}

const QStringList &IconPicker::glyphs()
{
    static const QStringList list = [] {
        QStringList names;
        for (const PickerGroup &group : PickerGroups) {
            for (const PickerGlyph &glyph : group.glyphs) {
                names.append(QString::fromLatin1(glyph.name));
            }
        }
        return names;
    }();
    return list;
}

void IconPicker::open(QWidget *anchor, const QString &icon, const QString &color, bool allowNoIcon)
{
    openAt(anchor->mapToGlobal(QPoint(0, anchor->height())), icon, color, allowNoIcon);
}

void IconPicker::openAt(const QPoint &globalPosition, const QString &icon, const QString &color,
                        bool allowNoIcon)
{
    m_icon = icon;
    m_color = color;
    m_allowNoIcon = allowNoIcon;
    {
        const QSignalBlocker blocker(m_search);
        m_search->clear();
    }
    refresh();
    filter({});
    adjustSize();
    showOnScreen(this, globalPosition);
    m_scroll->verticalScrollBar()->setValue(0);
    for (const GlyphButton &glyph : std::as_const(m_buttons)) {
        if (glyph.name == m_icon) {
            m_scroll->ensureWidgetVisible(glyph.button);
            break;
        }
    }
    m_search->setFocus();
}

bool IconPicker::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            const auto found = std::ranges::find_if(
                m_buttons, [](const GlyphButton &glyph) { return !glyph.button->isHidden(); });
            if (found != m_buttons.cend()) {
                found->button->click();
            }
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void IconPicker::refresh()
{
    m_colorTitle->setText(tr("Colour"));
    m_iconTitle->setText(tr("Icon"));
    m_search->setPlaceholderText(tr("Search icons"));
    m_noMatches->setText(tr("No matching icons"));
    m_colors->setCurrent(m_color);
    m_none->setToolTip(tr("No icon"));
    m_none->setAccessibleName(m_none->toolTip());
    m_none->setChecked(m_icon.isEmpty());

    int group = 0;
    for (const PickerGroup &pickerGroup : PickerGroups) {
        m_groupTitles.at(group)->setText(pickerText(pickerGroup.title));
        ++group;
    }
    const auto tint = colorFromKey(m_color);
    for (const GlyphButton &glyph : std::as_const(m_buttons)) {
        const QString label = pickerText(glyph.label);
        glyph.button->setToolTip(label);
        glyph.button->setAccessibleName(label);
        glyph.button->setIcon(tint ? icon(glyph.name, *tint) : icon(glyph.name));
        glyph.button->setChecked(glyph.name == m_icon);
    }
}

void IconPicker::filter(const QString &text)
{
    const QString wanted = text.trimmed();
    while (const QLayoutItem *item = m_icons->takeAt(0)) {
        delete item;
    }
    for (QLabel *title : std::as_const(m_groupTitles)) {
        title->setVisible(wanted.isEmpty());
    }
    // No icon leads the first group while browsing; a search shows glyphs only.
    m_none->setVisible(wanted.isEmpty() && m_allowNoIcon);
    int row = 0;
    int column = 0;
    int group = -1;
    bool any = false;
    for (const GlyphButton &glyph : std::as_const(m_buttons)) {
        if (glyph.group != group) {
            group = glyph.group;
            if (wanted.isEmpty()) {
                // Each group starts on a new row under its title.
                if (column > 0) {
                    ++row;
                    column = 0;
                }
                m_icons->addWidget(m_groupTitles.at(group), row++, 0, 1, PickerColumns);
                if (group == 0 && m_allowNoIcon) {
                    m_icons->addWidget(m_none, row, column++);
                }
            }
        }
        const bool shown = wanted.isEmpty() ||
                           glyph.button->toolTip().contains(wanted, Qt::CaseInsensitive) ||
                           glyph.name.contains(wanted, Qt::CaseInsensitive) ||
                           m_groupTitles.at(group)->text().contains(wanted, Qt::CaseInsensitive);
        glyph.button->setVisible(shown);
        if (!shown) {
            continue;
        }
        any = true;
        m_icons->addWidget(glyph.button, row, column);
        if (++column == PickerColumns) {
            ++row;
            column = 0;
        }
    }
    m_noMatches->setVisible(!any);
    if (!any) {
        m_icons->addWidget(m_noMatches, 0, 0, 1, PickerColumns);
    }
}

MultiChoicePopup::MultiChoicePopup(QWidget *parent)
    : QFrame(parent, Qt::Popup)
    , m_search(new QLineEdit(this))
    , m_scroll(new QScrollArea(this))
    , m_options(new QWidget)
    , m_optionsLayout(new QVBoxLayout(m_options))
    , m_noMatches(new QLabel(this))
    , m_clear(new QPushButton(this))
{
    setProperty("popup", true);
    setAttribute(Qt::WA_StyledBackground);
    const Theme &theme = *Theme::current();
    m_search->setProperty("search", true);
    m_search->setClearButtonEnabled(true);
    m_search->addAction(icon(QStringLiteral("Search")), QLineEdit::LeadingPosition);
    m_optionsLayout->setContentsMargins(0, 0, 0, 0);
    m_optionsLayout->setSpacing(theme.px(0.3));
    m_optionsLayout->addStretch();
    m_scroll->setWidget(m_options);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_options->setAttribute(Qt::WA_StyledBackground);
    m_options->setProperty("popupContent", true);
    setTextRole(m_noMatches, TextRole::Hint);
    m_clear->setProperty("flat", true);
    m_clear->setIcon(icon(QStringLiteral("Close")));

    auto *layout = new QVBoxLayout(this);
    const int pad = theme.px(0.6);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(theme.px(0.5));
    layout->addWidget(m_search);
    layout->addWidget(m_scroll, 1);
    layout->addWidget(m_noMatches);
    layout->addWidget(m_clear);
    setMinimumWidth(theme.px(15));

    connect(m_search, &QLineEdit::textChanged, this, &MultiChoicePopup::filter);
    connect(m_clear, &QPushButton::clicked, this, [this] {
        for (QCheckBox *box : std::as_const(m_boxes)) {
            const QSignalBlocker blocker(box);
            box->setChecked(false);
        }
        updateClear();
        Q_EMIT changed({});
    });
    retranslate();
}

void MultiChoicePopup::open(QWidget *anchor, const QList<Option> &options,
                            const QStringList &chosen)
{
    qDeleteAll(m_boxes);
    m_boxes.clear();
    const Theme &theme = *Theme::current();
    for (const Option &option : options) {
        auto *box = new QCheckBox(option.label, m_options);
        box->setProperty("key", option.key);
        box->setChecked(chosen.contains(option.key));
        if (!option.iconName.isEmpty()) {
            const auto color = colorFromKey(option.color.value_or(QString()));
            box->setIcon(color ? icon(option.iconName, *color) : icon(option.iconName));
        }
        box->setContentsMargins(option.depth * theme.px(1.0), 0, 0, 0);
        connect(box, &QCheckBox::toggled, this, [this] {
            updateClear();
            Q_EMIT changed(this->chosen());
        });
        m_optionsLayout->insertWidget(m_optionsLayout->count() - 1, box);
        m_boxes.append(box);
    }
    m_search->clear();
    m_search->setVisible(options.size() > SearchableChoices);
    filter({});
    updateClear();
    // The list takes the height of its options, scrolling beyond twenty lines.
    m_scroll->setFixedHeight(std::min(m_options->sizeHint().height(), theme.px(20)));
    showNear(this, anchor);
    if (m_search->isVisible()) {
        m_search->setFocus();
    } else if (!m_boxes.isEmpty()) {
        m_boxes.first()->setFocus();
    }
}

QStringList MultiChoicePopup::chosen() const
{
    QStringList keys;
    for (const QCheckBox *box : m_boxes) {
        if (box->isChecked()) {
            keys.append(box->property("key").toString());
        }
    }
    return keys;
}

void MultiChoicePopup::filter(const QString &text)
{
    const QString wanted = text.trimmed();
    bool any = false;
    for (QCheckBox *box : std::as_const(m_boxes)) {
        const bool shown = box->text().contains(wanted, Qt::CaseInsensitive);
        box->setVisible(shown);
        any = any || shown;
    }
    m_noMatches->setText(m_boxes.isEmpty() ? tr("Nothing to choose yet") : tr("No matches"));
    m_noMatches->setVisible(!any);
}

void MultiChoicePopup::updateClear()
{
    m_clear->setVisible(std::ranges::any_of(m_boxes, &QCheckBox::isChecked));
}

void MultiChoicePopup::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QFrame::changeEvent(event);
}

void MultiChoicePopup::retranslate()
{
    m_search->setPlaceholderText(tr("Search"));
    m_clear->setText(tr("Clear"));
    filter(m_search->text());
}

SearchPopup::SearchPopup(QWidget *parent)
    : QFrame(parent, Qt::Popup)
    , m_search(new QLineEdit(this))
    , m_list(new QListWidget(this))
{
    setProperty("popup", true);
    setAttribute(Qt::WA_StyledBackground);
    const Theme &theme = *Theme::current();
    auto *layout = new QVBoxLayout(this);
    const int pad = theme.px(0.6);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(theme.px(0.4));
    layout->addWidget(m_search);
    layout->addWidget(m_list);
    m_search->setClearButtonEnabled(true);
    m_search->installEventFilter(this);
    m_list->setUniformItemSizes(true);
    m_list->setFocusPolicy(Qt::NoFocus);
    setFixedSize(theme.px(18), theme.px(20));
    connect(m_search, &QLineEdit::textChanged, this, &SearchPopup::refresh);
    connect(m_list, &QListWidget::itemClicked, this, &SearchPopup::choose);
}

void SearchPopup::openAt(const QPoint &globalPosition, const QString &current)
{
    {
        const QSignalBlocker blocker(m_search);
        m_search->clear();
    }
    refresh();
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(Qt::UserRole).toString() == current) {
            m_list->setCurrentRow(i);
            m_list->scrollToItem(m_list->item(i), QAbstractItemView::PositionAtCenter);
            break;
        }
    }
    showOnScreen(this, globalPosition);
    m_search->setFocus();
}

bool SearchPopup::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_PageUp ||
            key == Qt::Key_PageDown) {
            QCoreApplication::sendEvent(m_list, event);
            return true;
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            choose();
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void SearchPopup::setPlaceholder(const QString &text)
{
    m_search->setPlaceholderText(text);
}

void SearchPopup::refresh()
{
    m_list->clear();
    for (const Entry &entry : entries(m_search->text().trimmed())) {
        auto *item = new QListWidgetItem(entry.icon, entry.text, m_list);
        item->setData(Qt::UserRole, entry.key);
    }
    if (m_list->count() > 0) {
        m_list->setCurrentRow(0);
    }
}

void SearchPopup::choose()
{
    const QListWidgetItem *item = m_list->currentItem();
    if (item == nullptr) {
        return;
    }
    hide();
    Q_EMIT chosen(item->data(Qt::UserRole).toString(), item->text());
}

bool confirmDestructive(QWidget *parent, const QString &title, const QString &text,
                        const QString &action, QWidget *choices)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    const Theme &theme = *Theme::current();
    auto *heading = new QLabel(title, &dialog);
    setTextRole(heading, TextRole::SectionTitle);
    heading->setWordWrap(true);
    auto *body = new QLabel(text, &dialog);
    body->setWordWrap(true);
    auto *cancel = new QPushButton(QCoreApplication::translate("ui::Dialogs", "Cancel"), &dialog);
    auto *confirm = new QPushButton(action, &dialog);
    confirm->setProperty("role", QStringLiteral("dangerFilled"));
    cancel->setDefault(true);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(confirm, &QPushButton::clicked, &dialog, &QDialog::accept);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(confirm);
    auto *layout = new QVBoxLayout(&dialog);
    const int pad = theme.px(1.25);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(theme.px(0.75));
    layout->addWidget(heading);
    layout->addWidget(body);
    if (choices != nullptr) {
        layout->addWidget(choices);
    }
    layout->addLayout(buttons);
    dialog.setMinimumWidth(theme.px(24));
    return dialog.exec() == QDialog::Accepted;
}

} // namespace ui
