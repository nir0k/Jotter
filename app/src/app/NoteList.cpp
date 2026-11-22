#include "app/NoteList.h"

#include "app/Mime.h"
#include "app/SearchPanel.h"
#include "core/Tags.h"
#include "ui/Icons.h"
#include "ui/TagColor.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QActionGroup>
#include <QDir>
#include <QEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

/// Draws at most @p maxLines lines of @p text wrapped into @p rect, eliding the last one; the
/// words of @p matches are drawn bold in @p matchColor, as search results mark them.
void drawWrapped(QPainter *painter, const QRect &rect, const QString &text, int maxLines,
                 const QList<core::TextSpan> &matches = {}, const QColor &matchColor = {})
{
    QTextCharFormat strong;
    strong.setFontWeight(QFont::Bold);
    strong.setForeground(matchColor);
    // The formats of the part of the text that starts at @p from and is @p length long.
    const auto formatsOf = [&](qsizetype from, qsizetype length) {
        QList<QTextLayout::FormatRange> formats;
        for (const core::TextSpan &match : matches) {
            const qsizetype start = std::max(match.start, from);
            const qsizetype end = std::min(match.start + match.length, from + length);
            if (start < end) {
                formats.append({.start = static_cast<int>(start - from),
                                .length = static_cast<int>(end - start),
                                .format = strong});
            }
        }
        return formats;
    };
    const QFontMetrics metrics = painter->fontMetrics();
    QTextLayout layout(text, painter->font());
    // A word longer than the row (a link, a code) wraps anywhere instead of running out of it.
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.setFormats(formatsOf(0, text.size()));
    layout.beginLayout();
    int y = 0;
    for (int line = 0; line < maxLines; ++line) {
        QTextLine textLine = layout.createLine();
        if (!textLine.isValid()) {
            break;
        }
        textLine.setLineWidth(rect.width());
        textLine.setPosition(QPointF(0, y));
        y += metrics.height();
    }
    layout.endLayout();
    painter->save();
    painter->setClipRect(rect);
    for (int i = 0; i < layout.lineCount(); ++i) {
        const QTextLine textLine = layout.lineAt(i);
        if (i == maxLines - 1 && textLine.textStart() + textLine.textLength() < text.size()) {
            // The last line is cut with an ellipsis, keeping the marks of the words it shows.
            const QString elided =
                metrics.elidedText(text.mid(textLine.textStart()), Qt::ElideRight, rect.width());
            QTextLayout tail(elided, painter->font());
            tail.setFormats(formatsOf(textLine.textStart(), elided.size() - 1));
            tail.beginLayout();
            QTextLine tailLine = tail.createLine();
            tailLine.setPosition(textLine.position());
            tail.endLayout();
            tail.draw(painter, rect.topLeft());
        } else {
            textLine.draw(painter, rect.topLeft());
        }
    }
    painter->restore();
}

} // namespace

/// The filter toggle of the search field: checked while the filter panel is open, with a dot
/// while any filter is applied.
class FilterToggle : public QToolButton
{
public:
    using QToolButton::QToolButton;

    void setDot(bool dot)
    {
        m_dot = dot;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QToolButton::paintEvent(event);
        if (!m_dot) {
            return;
        }
        const ui::Theme &theme = *ui::Theme::current();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.roles().primary);
        const qreal radius = theme.px(0.2);
        painter.drawEllipse(QPointF(width() - (2 * radius), 2 * radius), radius, radius);
    }

private:
    bool m_dot = false;
};

namespace {

/// Draws tags as pills in their colours on one line from the left of @p rect; the tags that do
/// not fit are counted as +N at the end, so a dense list keeps one line per note.
///
/// @param colors Tag colour keys by tag key; a tag without colour is drawn neutral.
void drawTagLine(QPainter *painter, const QRect &rect, const QStringList &tags,
                 const QMap<QString, QString> &colors)
{
    const ui::Theme &theme = *ui::Theme::current();
    const ui::Roles &r = theme.roles();
    painter->setFont(theme.font(ui::TextRole::LabelSmall));
    const QFontMetrics metrics = painter->fontMetrics();
    const int padding = theme.px(0.5);
    const int gap = theme.px(0.3);
    const auto moreText = [](qsizetype count) { return QStringLiteral("+%1").arg(count); };
    int x = rect.left();
    for (qsizetype i = 0; i < tags.size(); ++i) {
        const qsizetype rest = tags.size() - i - 1;
        const int moreWidth = rest > 0 ? gap + metrics.horizontalAdvance(moreText(rest)) : 0;
        QString text = tags.at(i);
        int width = metrics.horizontalAdvance(text) + (2 * padding);
        if (x + width + moreWidth > rect.right()) {
            if (i > 0) {
                painter->setPen(r.onSurfaceVariant);
                painter->drawText(QRect(x, rect.top(), rect.right() - x, rect.height()),
                                  Qt::AlignLeft | Qt::AlignVCenter, moreText(tags.size() - i));
                return;
            }
            // A first tag too long for the row is shortened rather than left out.
            width = std::max(2 * padding, rect.right() - x - moreWidth);
            text = metrics.elidedText(text, Qt::ElideRight, width - (2 * padding));
        }
        const QRectF pill(x + 0.5, rect.top() + 0.5, width - 1, rect.height() - 1);
        const ui::PillColors pills = ui::pillColors(core::tagColor(colors, tags.at(i)), r);
        painter->setPen(pills.border);
        painter->setBrush(pills.fill);
        painter->drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
        painter->setPen(pills.text);
        painter->drawText(pill, Qt::AlignCenter, text);
        x += width + gap;
    }
}

/// Height of the tag line of a row: a pill around the small label text.
int tagLineHeight()
{
    const ui::Theme &theme = *ui::Theme::current();
    return QFontMetrics(theme.font(ui::TextRole::LabelSmall)).height() + theme.px(0.3);
}

/// Draws a note row: glyph and title in the title colour, the date, two lines of text and, when
/// the note has tags, a line of tags.
class NoteDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const auto *model = static_cast<const NoteListModel *>(index.model());
        const core::NoteSummary &note = model->summary(index.row());
        const ui::Theme &theme = *ui::Theme::current();
        const ui::Roles &r = theme.roles();
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        const QRect rect = option.rect;
        const int pad = theme.px(0.75);
        // Every row ends with a separator in the outline role, the colour of the pane dividers,
        // so notes stay apart in light and dark variants; the hover and selection backgrounds
        // are inset and never cover it.
        painter->setPen(r.outline);
        painter->drawLine(rect.left() + pad, rect.bottom(), rect.right() - pad, rect.bottom());
        if (selected || hovered) {
            const int inset = theme.px(0.25);
            painter->setPen(Qt::NoPen);
            painter->setBrush(selected ? r.primaryContainer
                                       : ui::mix(r.onSurface, 0.05F, r.surface));
            painter->drawRoundedRect(rect.adjusted(inset, inset, -inset, -inset), theme.px(0.5),
                                     theme.px(0.5));
        }

        const QRect content = rect.adjusted(pad, theme.px(0.75), -pad, -theme.px(0.75));
        int left = content.left();

        // First line: glyph, title, marks and the date.
        painter->setFont(theme.font(ui::TextRole::Label));
        const int lineHeight = painter->fontMetrics().height();
        const QRect firstLine(content.left(), content.top(), content.width(), lineHeight);
        if (note.icon) {
            const int size = theme.px(1.0);
            const auto color = ui::colorFromKey(note.color.value_or(QString()));
            const QIcon glyph =
                color && !selected
                    ? ui::icon(*note.icon, *color)
                    : ui::icon(*note.icon, selected ? ui::IconTone::Primary : ui::IconTone::Idle);
            glyph.paint(painter, QRect(left, firstLine.center().y() - (size / 2), size, size));
            left += size + theme.px(0.4);
        }
        painter->setFont(theme.font(ui::TextRole::Hint));
        const QDateTime date =
            model->dateKey() == core::NoteSort::Key::Created ? note.created : note.updated;
        const QString dateText = shortDate(date);
        const int dateWidth = painter->fontMetrics().horizontalAdvance(dateText);
        painter->setPen(selected ? r.onPrimaryContainer : r.onSurfaceVariant);
        painter->drawText(
            QRect(content.right() - dateWidth, firstLine.top(), dateWidth, lineHeight),
            Qt::AlignRight | Qt::AlignVCenter, dateText);
        int right = content.right() - dateWidth - theme.px(0.4);
        if (model->isFavorite(note.id)) {
            const int size = theme.px(0.85);
            ui::icon(QStringLiteral("FavoriteFilled"), ui::IconTone::Primary)
                .paint(painter,
                       QRect(right - size, firstLine.center().y() - (size / 2), size, size));
            right -= size + theme.px(0.3);
        }
        // Small marks for a favourite, attachments and a geotag.
        for (const auto &[shown, glyph] :
             {std::pair{note.attachments > 0, "Attach"}, std::pair{note.hasLocation, "Place"}}) {
            if (!shown) {
                continue;
            }
            const int size = theme.px(0.85);
            ui::icon(QString::fromLatin1(glyph))
                .paint(painter,
                       QRect(right - size, firstLine.center().y() - (size / 2), size, size));
            right -= size + theme.px(0.3);
        }

        QFont titleFont = theme.font(ui::TextRole::Label);
        titleFont.setWeight(QFont::DemiBold);
        painter->setFont(titleFont);
        painter->setPen(ui::itemTextColor(note.color.value_or(QString()), r, selected));
        const QString title =
            note.title.isEmpty() ? QCoreApplication::translate("NoteList", "Untitled") : note.title;
        painter->drawText(QRect(left, firstLine.top(), right - left, lineHeight),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(title, Qt::ElideRight, right - left));

        // Then up to two lines of the note's text.
        painter->setFont(theme.font(ui::TextRole::Muted));
        painter->setPen(selected ? r.onPrimaryContainer : r.onSurfaceVariant);
        const QRect textRect(content.left(), firstLine.bottom() + theme.px(0.2), content.width(),
                             content.bottom() - firstLine.bottom());
        drawWrapped(painter, textRect, note.snippet, 2, note.matches,
                    selected ? r.onPrimaryContainer : r.onSurface);
        if (!note.tags.isEmpty()) {
            const int height = tagLineHeight();
            drawTagLine(
                painter,
                QRect(content.left(), content.bottom() - height + 1, content.width(), height),
                note.tags, model->tagColors());
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const ui::Theme &theme = *ui::Theme::current();
        const int title = QFontMetrics(theme.font(ui::TextRole::Label)).height();
        const int text = QFontMetrics(theme.font(ui::TextRole::Muted)).height();
        const auto *model = static_cast<const NoteListModel *>(index.model());
        const int tags =
            model->summary(index.row()).tags.isEmpty() ? 0 : tagLineHeight() + theme.px(0.4);
        return {option.rect.width(), title + (2 * text) + tags + theme.px(1.7)};
    }
};

} // namespace

QString shortDate(const QDateTime &time)
{
    const QLocale locale;
    const QDateTime local = time.toLocalTime();
    const QDate today = QDate::currentDate();
    if (local.date() == today) {
        return locale.toString(local.time(), QLocale::ShortFormat);
    }
    if (local.date() == today.addDays(-1)) {
        return QCoreApplication::translate("NoteList", "Yesterday");
    }
    if (local.date().year() == today.year()) {
        return locale.toString(local.date(), QStringLiteral("d MMM"));
    }
    return locale.toString(local.date(), QLocale::ShortFormat);
}

void NoteListModel::setNotes(QList<core::NoteSummary> notes, core::NoteSort::Key dateKey)
{
    beginResetModel();
    m_notes = std::move(notes);
    m_dateKey = dateKey;
    endResetModel();
}

void NoteListModel::setFavorites(QSet<QString> favorites)
{
    m_favorites = std::move(favorites);
    if (!m_notes.isEmpty()) {
        Q_EMIT dataChanged(index(0), index(static_cast<int>(m_notes.size()) - 1));
    }
}

void NoteListModel::setTagColors(QMap<QString, QString> colors)
{
    m_tagColors = std::move(colors);
    if (!m_notes.isEmpty()) {
        Q_EMIT dataChanged(index(0), index(static_cast<int>(m_notes.size()) - 1));
    }
}

int NoteListModel::rowOf(const QString &id) const
{
    for (qsizetype i = 0; i < m_notes.size(); ++i) {
        if (m_notes.at(i).id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int NoteListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_notes.size());
}

Qt::ItemFlags NoteListModel::flags(const QModelIndex &index) const
{
    return QAbstractListModel::flags(index) |
           (index.isValid() ? Qt::ItemIsDragEnabled : Qt::NoItemFlags);
}

QStringList NoteListModel::mimeTypes() const
{
    return {mime::Notes};
}

QMimeData *NoteListModel::mimeData(const QModelIndexList &indexes) const
{
    QStringList ids;
    for (const QModelIndex &index : indexes) {
        ids.append(index.data(Qt::UserRole).toString());
    }
    auto *data = new QMimeData;
    data->setData(mime::Notes, ids.join(u'\n').toUtf8());
    return data;
}

QVariant NoteListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_notes.size()) {
        return {};
    }
    const core::NoteSummary &note = m_notes.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case Qt::AccessibleTextRole:
    case Qt::ToolTipRole:
        return note.title;
    case Qt::UserRole:
        return note.id;
    default:
        return {};
    }
}

NoteList::NoteList(QWidget *parent)
    : QWidget(parent)
    , m_sidebarButton(new QToolButton(this))
    , m_title(new QLabel(this))
    , m_count(new QLabel(this))
    , m_search(new QLineEdit(this))
    , m_filterToggle(new FilterToggle(this))
    , m_panel(new SearchPanel(this))
    , m_sortButton(new QToolButton(this))
    , m_sortMenu(new QMenu(this))
    , m_newButton(new QToolButton(this))
    , m_subcollections(new QToolButton(this))
    , m_emptyTrash(new QPushButton(this))
    , m_headerBox(new QWidget(this))
    , m_selectionBar(new QWidget(this))
    , m_selectionCount(new QLabel(this))
    , m_selectionActions(new QToolButton(this))
    , m_clearSelection(new QToolButton(this))
    , m_notice(new ui::Notice(this))
    , m_stack(new QStackedWidget(this))
    , m_list(new QListView(this))
    , m_empty(new ui::EmptyState(this))
    , m_noMatches(new ui::EmptyState(this))
{
    setProperty("pane", QStringLiteral("list"));
    setAttribute(Qt::WA_StyledBackground);
    const ui::Theme &theme = *ui::Theme::current();

    m_sidebarButton->setIcon(ui::icon(QStringLiteral("Sidebar")));
    ui::setTextRole(m_title, ui::TextRole::SectionTitle);
    ui::setTextRole(m_count, ui::TextRole::Muted);
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);
    m_sortButton->setPopupMode(QToolButton::InstantPopup);
    m_sortButton->setMenu(m_sortMenu);

    m_newButton->setIcon(ui::icon(QStringLiteral("Add"), ui::IconTone::Primary));
    m_emptyTrash->setProperty("role", QStringLiteral("danger"));
    m_emptyTrash->hide();

    auto *header = new QHBoxLayout;
    header->setSpacing(theme.px(0.5));
    header->addWidget(m_sidebarButton);
    header->addWidget(m_title, 1);
    header->addWidget(m_count);
    header->addWidget(m_subcollections);
    header->addWidget(m_newButton);

    // The search field is always visible, on a row of its own in the narrow pane.
    m_search->setProperty("search", true);
    m_search->setClearButtonEnabled(true);
    m_search->addAction(ui::icon(QStringLiteral("Search")), QLineEdit::LeadingPosition);
    m_search->installEventFilter(this);
    m_filterToggle->setIcon(ui::icon(QStringLiteral("Filter")));
    m_filterToggle->setCheckable(true);
    m_panel->hide();
    m_searchDelay.setSingleShot(true);
    m_searchDelay.setInterval(150);
    const int pad = theme.px(0.75);
    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(pad, 0, pad, 0);
    searchRow->setSpacing(theme.px(0.4));
    searchRow->addWidget(m_search, 1);
    searchRow->addWidget(m_filterToggle);
    searchRow->addWidget(m_sortButton);
    auto *panelBox = new QVBoxLayout;
    panelBox->setContentsMargins(pad, 0, pad, 0);
    panelBox->addWidget(m_panel);

    // While several notes are selected, the selection bar takes the header's place.
    m_clearSelection->setIcon(ui::icon(QStringLiteral("Close")));
    ui::setTextRole(m_selectionCount, ui::TextRole::Label);
    m_selectionActions->setIcon(ui::icon(QStringLiteral("More")));
    m_selectionActions->setPopupMode(QToolButton::InstantPopup);
    m_selectionActions->setMenu(new QMenu(m_selectionActions));
    auto *selection = new QHBoxLayout(m_selectionBar);
    selection->setSpacing(theme.px(0.5));
    selection->addWidget(m_clearSelection);
    selection->addWidget(m_selectionCount, 1);
    selection->addWidget(m_selectionActions);
    m_selectionBar->hide();

    m_list->setModel(&m_model);
    setFocusProxy(m_list);
    m_list->setItemDelegate(new NoteDelegate(m_list));
    m_list->setMouseTracking(true);
    m_list->setFrameShape(QFrame::NoFrame);
    // Rows with tags are taller by one line.
    m_list->setUniformItemSizes(false);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setDefaultDropAction(Qt::CopyAction);
    m_subcollections->setIcon(ui::icon(QStringLiteral("Folder")));
    m_subcollections->setCheckable(true);
    m_subcollections->hide();
    m_list->installEventFilter(this);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_stack->addWidget(m_list);
    m_stack->addWidget(m_empty);
    m_stack->addWidget(m_noMatches);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, pad, 0, 0);
    layout->setSpacing(theme.px(0.5));
    m_headerBox->setLayout(header);
    header->setContentsMargins(pad, 0, pad, 0);
    selection->setContentsMargins(pad, 0, pad, 0);
    layout->addWidget(m_headerBox);
    layout->addWidget(m_selectionBar);
    layout->addLayout(searchRow);
    layout->addLayout(panelBox);
    auto *trashBox = new QHBoxLayout;
    trashBox->setContentsMargins(pad, 0, pad, 0);
    trashBox->addWidget(m_emptyTrash);
    trashBox->addStretch();
    layout->addLayout(trashBox);
    auto *noticeBox = new QVBoxLayout;
    noticeBox->setContentsMargins(pad, 0, pad, 0);
    noticeBox->addWidget(m_notice);
    layout->addLayout(noticeBox);
    layout->addWidget(m_stack, 1);

    connect(m_sidebarButton, &QToolButton::clicked, this, &NoteList::sidebarRequested);
    connect(m_newButton, &QToolButton::clicked, this, &NoteList::newNoteRequested);
    connect(m_subcollections, &QToolButton::toggled, this, &NoteList::subcollectionsToggled);
    connect(m_emptyTrash, &QPushButton::clicked, this, &NoteList::emptyTrashRequested);
    connect(m_clearSelection, &QToolButton::clicked, this, [this] {
        const QString current = currentNote();
        selectNote(current);
    });
    connect(m_selectionActions->menu(), &QMenu::aboutToShow, this, [this] {
        m_selectionActions->menu()->clear();
        addActions(m_selectionActions->menu(), selectedNotes());
    });
    connect(m_list->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &NoteList::updateSelectionBar);
    connect(m_list, &QListView::customContextMenuRequested, this, [this](const QPoint &position) {
        const QModelIndex index = m_list->indexAt(position);
        if (!index.isValid() || !m_writable) {
            return;
        }
        if (!m_list->selectionModel()->isSelected(index)) {
            m_list->setCurrentIndex(index);
        }
        QMenu menu(this);
        addActions(&menu, selectedNotes());
        menu.exec(m_list->viewport()->mapToGlobal(position));
    });
    connect(m_notice, &ui::Notice::actionTriggered, this, &NoteList::retryRequested);
    connect(m_search, &QLineEdit::textChanged, &m_searchDelay, qOverload<>(&QTimer::start));
    connect(&m_searchDelay, &QTimer::timeout, this, &NoteList::searchChanged);
    connect(m_filterToggle, &QToolButton::toggled, this, [this](bool shown) {
        m_panel->setVisible(shown);
        m_filterToggle->setIcon(
            ui::icon(QStringLiteral("Filter"), shown ? ui::IconTone::Primary : ui::IconTone::Idle));
        retranslate();
    });
    connect(m_panel, &SearchPanel::changed, this, [this] {
        updateFilterToggle();
        Q_EMIT searchChanged();
    });
    connect(m_noMatches, &ui::EmptyState::actionTriggered, this, [this] {
        clearSearch();
        Q_EMIT searchChanged();
    });
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current) {
                if (current.isValid()) {
                    Q_EMIT noteSelected(current.data(Qt::UserRole).toString());
                }
            });
    // Ctrl and Shift clicks extend the selection and must not open the note.
    const auto activate = [this](const QModelIndex &index) {
        if (index.isValid() && QGuiApplication::keyboardModifiers() == Qt::NoModifier) {
            Q_EMIT noteActivated(index.data(Qt::UserRole).toString());
        }
    };
    connect(m_list, &QAbstractItemView::clicked, this, activate);
    connect(m_list, &QAbstractItemView::activated, this, activate);
    retranslate();
}

void NoteList::setNotes(const QString &title, QList<core::NoteSummary> notes,
                        const core::NoteSort &sort)
{
    const QString selected = currentNote();
    m_sort = sort;
    const qsizetype count = notes.size();
    {
        const QSignalBlocker blocker(m_list->selectionModel());
        m_model.setNotes(std::move(notes), sort.key);
    }
    m_title->setText(title);
    m_count->setText(count > 0 ? QLocale().toString(count) : QString());
    QWidget *page = m_list;
    if (count == 0) {
        // "No matches" offers to clear the search; "nothing yet" describes the view.
        page = search().isActive() ? m_noMatches : m_empty;
    }
    m_stack->setCurrentWidget(page);
    m_emptyTrash->setVisible(m_mode == Mode::Trash && m_writable && count > 0);
    selectNote(selected);
    updateSortMenu();
}

void NoteList::setMode(Mode mode, bool writable)
{
    m_mode = mode;
    m_writable = writable;
    m_newButton->setVisible(writable && mode == Mode::Notes);
    m_emptyTrash->setVisible(mode == Mode::Trash && writable && m_model.rowCount() > 0);
    updateSelectionBar();
}

void NoteList::setFavorites(QSet<QString> favorites)
{
    m_model.setFavorites(std::move(favorites));
}

void NoteList::setTagColors(QMap<QString, QString> colors)
{
    m_model.setTagColors(std::move(colors));
}

void NoteList::setSubcollectionsToggle(bool visible, bool included)
{
    const QSignalBlocker blocker(m_subcollections);
    m_subcollections->setVisible(visible);
    m_subcollections->setChecked(included);
}

QStringList NoteList::selectedNotes() const
{
    QModelIndexList rows = m_list->selectionModel()->selectedRows();
    std::ranges::sort(rows,
                      [](const QModelIndex &a, const QModelIndex &b) { return a.row() < b.row(); });
    QStringList ids;
    for (const QModelIndex &row : rows) {
        ids.append(row.data(Qt::UserRole).toString());
    }
    return ids;
}

void NoteList::updateSelectionBar()
{
    const qsizetype count = m_list->selectionModel()->selectedRows().size();
    const bool several = count > 1;
    m_selectionBar->setVisible(several);
    m_headerBox->setVisible(!several);
    m_selectionActions->setEnabled(m_writable);
    m_selectionCount->setText(tr("%n selected", nullptr, static_cast<int>(count)));
}

void NoteList::addActions(QMenu *menu, const QStringList &ids)
{
    const auto add = [&](const QString &text, editor::NoteAction action, bool danger = false) {
        QAction *entry = menu->addAction(text);
        if (danger) {
            entry->setIcon(ui::icon(QStringLiteral("Delete"), ui::IconTone::Error));
        }
        connect(entry, &QAction::triggered, this,
                [this, action, ids] { Q_EMIT actionRequested(action, ids); });
    };
    using editor::NoteAction;
    switch (m_mode) {
    case Mode::Trash:
        add(tr("Restore"), NoteAction::Restore);
        menu->addSeparator();
        add(tr("Delete permanently…"), NoteAction::DeletePermanently, true);
        break;
    case Mode::Archive:
    case Mode::Notes: {
        // A mixed selection is first added to the favourites; when all are favourites, the
        // entry removes them.
        const bool allFavorite =
            std::ranges::all_of(ids, [this](const QString &id) { return m_model.isFavorite(id); });
        add(tr("Collections…"), NoteAction::Collections);
        add(tr("Tags…"), NoteAction::Tags);
        add(tr("Export…"), NoteAction::Export);
        menu->addSeparator();
        add(allFavorite ? tr("Remove from favourites") : tr("Add to favourites"),
            allFavorite ? NoteAction::Unfavorite : NoteAction::Favorite);
        add(m_mode == Mode::Archive ? tr("Unarchive") : tr("Archive"),
            m_mode == Mode::Archive ? NoteAction::Unarchive : NoteAction::Archive);
        menu->addSeparator();
        add(tr("Move to trash"), NoteAction::Trash, true);
        break;
    }
    }
}

bool NoteList::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        // Escape clears the query first and then leaves the field; Down goes to the notes.
        if (key == Qt::Key_Escape && !m_search->text().isEmpty()) {
            m_search->clear();
            return true;
        }
        if ((key == Qt::Key_Escape || key == Qt::Key_Down) && m_model.rowCount() > 0) {
            if (!m_list->currentIndex().isValid()) {
                m_list->setCurrentIndex(m_model.index(0));
            }
            m_list->setFocus(Qt::ShortcutFocusReason);
            return true;
        }
    }
    if (watched == m_list && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape &&
        m_list->selectionModel()->selectedRows().size() > 1) {
        selectNote(currentNote());
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void NoteList::setEmptyText(const QString &iconName, const QString &title, const QString &text)
{
    m_empty->setContent(iconName, title, text);
}

void NoteList::selectNote(const QString &id)
{
    {
        const QSignalBlocker blocker(m_list->selectionModel());
        const int row = m_model.rowOf(id);
        if (row < 0) {
            m_list->selectionModel()->clear();
        } else {
            const QModelIndex index = m_model.index(row);
            // The view's own setCurrentIndex() follows the keyboard modifiers, and Qt reports
            // Ctrl as held until the next input after Ctrl+C or Ctrl+V; a list refreshed then
            // would toggle the note off and close it.
            m_list->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
            m_list->scrollTo(index);
        }
    }
    // The selection signals were blocked, so the selection bar is brought up to date here.
    updateSelectionBar();
    m_list->viewport()->update();
}

QString NoteList::currentNote() const
{
    const QModelIndex current = m_list->currentIndex();
    return current.isValid() && m_list->selectionModel()->isSelected(current)
               ? current.data(Qt::UserRole).toString()
               : QString();
}

void NoteList::setSidebarShown(bool shown)
{
    m_sidebarShown = shown;
    const QString label = shown ? tr("Hide the sidebar") : tr("Show the sidebar");
    m_sidebarButton->setToolTip(label);
    m_sidebarButton->setAccessibleName(label);
}

void NoteList::setUnavailable(bool unavailable, const QString &path)
{
    m_unavailablePath = unavailable ? path : QString();
    if (!unavailable) {
        m_notice->hide();
        return;
    }
    m_notice->showNotice(ui::Notice::Kind::Error,
                         tr("The library folder %1 cannot be reached. Notes are shown from the "
                            "last scan and cannot be changed until it is back.")
                             .arg(QDir::toNativeSeparators(path)),
                         tr("Retry"));
}

core::Search NoteList::search() const
{
    core::Search search;
    search.query = m_search->text();
    m_panel->apply(search);
    return search;
}

bool NoteList::searchesEverywhere() const
{
    return m_panel->searchesEverywhere();
}

void NoteList::setSearchSources(QList<core::Collection> collections, QList<core::TagCount> tags)
{
    m_panel->setSources(std::move(collections), std::move(tags));
    updateFilterToggle();
}

void NoteList::setSearchWidening(bool everywhere, bool archived)
{
    m_panel->setWidening(everywhere, archived);
    updateFilterToggle();
}

void NoteList::focusSearch()
{
    m_search->setFocus(Qt::ShortcutFocusReason);
    m_search->selectAll();
}

void NoteList::clearSearch()
{
    {
        const QSignalBlocker blocker(m_search);
        m_search->clear();
    }
    m_searchDelay.stop();
    m_panel->clear();
    updateFilterToggle();
}

void NoteList::updateFilterToggle()
{
    m_filterToggle->setDot(m_panel->isFiltering());
}

void NoteList::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QWidget::changeEvent(event);
}

void NoteList::retranslate()
{
    setSidebarShown(m_sidebarShown);
    m_subcollections->setToolTip(tr("Include notes of subcollections"));
    m_subcollections->setAccessibleName(tr("Include notes of subcollections"));
    m_newButton->setToolTip(tr("New note (Ctrl+N)"));
    m_newButton->setAccessibleName(tr("New note"));
    m_emptyTrash->setText(tr("Empty trash…"));
    m_clearSelection->setToolTip(tr("Clear selection (Escape)"));
    m_clearSelection->setAccessibleName(tr("Clear selection"));
    m_selectionActions->setToolTip(tr("Actions for the selected notes"));
    m_selectionActions->setAccessibleName(tr("Actions for the selected notes"));
    updateSelectionBar();
    m_sortButton->setToolTip(tr("Sort"));
    m_sortButton->setAccessibleName(tr("Sort"));
    m_search->setPlaceholderText(tr("Search notes"));
    m_search->setToolTip(tr("Search notes (Ctrl+Shift+F)"));
    m_search->setAccessibleName(tr("Search notes"));
    const QString filters = m_filterToggle->isChecked() ? tr("Hide filters") : tr("Show filters");
    m_filterToggle->setToolTip(filters);
    m_filterToggle->setAccessibleName(filters);
    m_noMatches->setContent(QStringLiteral("Search"), tr("No matches"),
                            tr("No notes match the search and the filters."), tr("Clear search"));
    updateSortMenu();
    if (!m_unavailablePath.isEmpty()) {
        setUnavailable(true, m_unavailablePath);
    }
    m_list->viewport()->update();
}

void NoteList::updateSortMenu()
{
    m_sortButton->setIcon(ui::icon(m_sort.descending ? QStringLiteral("SortDescending")
                                                     : QStringLiteral("SortAscending")));
    m_sortMenu->clear();
    auto *keys = new QActionGroup(m_sortMenu);
    const auto addKey = [&](core::NoteSort::Key key, const QString &text) {
        QAction *action = m_sortMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(m_sort.key == key);
        keys->addAction(action);
        connect(action, &QAction::triggered, this, [this, key] {
            Q_EMIT sortChanged({.key = key, .descending = key != core::NoteSort::Key::Title});
        });
    };
    addKey(core::NoteSort::Key::Updated, tr("Date updated"));
    addKey(core::NoteSort::Key::Created, tr("Date created"));
    addKey(core::NoteSort::Key::Title, tr("Title"));
    m_sortMenu->addSeparator();

    auto *orders = new QActionGroup(m_sortMenu);
    const bool byTitle = m_sort.key == core::NoteSort::Key::Title;
    const auto addOrder = [&](bool descending, const QString &text) {
        QAction *action = m_sortMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(m_sort.descending == descending);
        orders->addAction(action);
        connect(action, &QAction::triggered, this, [this, descending] {
            Q_EMIT sortChanged({.key = m_sort.key, .descending = descending});
        });
    };
    if (byTitle) {
        addOrder(false, tr("A to Z"));
        addOrder(true, tr("Z to A"));
    } else {
        addOrder(true, tr("Newest first"));
        addOrder(false, tr("Oldest first"));
    }
}
