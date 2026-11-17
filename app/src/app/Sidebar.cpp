#include "app/Sidebar.h"

#include "app/CollectionCombo.h"
#include "app/Mime.h"
#include "app/RuleEditorDialog.h"
#include "core/Files.h"
#include "core/LibrarySession.h"
#include "core/Tags.h"
#include "ui/Icons.h"
#include "ui/Popups.h"
#include "ui/TagColor.h"
#include "ui/Theme.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QEvent>
#include <QGuiApplication>
#include <QHash>
#include <QHeaderView>
#include <QHelpEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSet>
#include <QStyledItemDelegate>
#include <QToolTip>
#include <QTreeView>
#include <QVBoxLayout>

namespace {

enum Role {
    KindRole = Qt::UserRole + 1,
    IdRole,
    CountRole,
    IconRole,
    ColorRole,
    KeyRole,
    HeaderRole,
    /// Whether a section header offers + after its name to create an entry.
    AddRole,
};

/// Returns where a section header draws its + button: right after its name and as tall as the
/// name's line, so the header keeps its height.
///
/// @param rect The header's row.
/// @param text The header's name.
/// @return The button's rectangle in viewport coordinates.
QRect headerAddRect(const QRect &rect, const QString &text)
{
    const ui::Theme &theme = *ui::Theme::current();
    const QFontMetrics metrics(theme.font(ui::TextRole::LabelSmall));
    const int size = metrics.height();
    const int bottom = rect.bottom() - theme.px(0.15);
    const int left = rect.left() + theme.px(0.25) + metrics.horizontalAdvance(text) + theme.px(0.3);
    return {left, bottom - size + 1, size, size};
}

/// Draws a sidebar entry: glyph, name and count, or a section header in the muted label style.
class SidebarDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const ui::Theme &theme = *ui::Theme::current();
        const ui::Roles &r = theme.sidebarRoles();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect rect = option.rect;
        const bool header = index.data(HeaderRole).toBool();
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const bool hovered = (option.state & QStyle::State_MouseOver) != 0;

        if (header) {
            painter->setFont(theme.font(ui::TextRole::LabelSmall));
            painter->setPen(r.onSurfaceVariant);
            painter->drawText(rect.adjusted(theme.px(0.25), 0, 0, -theme.px(0.15)),
                              Qt::AlignLeft | Qt::AlignBottom, index.data().toString());
            if (index.data(AddRole).toBool()) {
                const QRect add = headerAddRect(rect, index.data().toString());
                const auto *view = qobject_cast<const QAbstractItemView *>(option.widget);
                const bool over = hovered && view != nullptr &&
                                  add.contains(view->viewport()->mapFromGlobal(QCursor::pos()));
                ui::icon(QStringLiteral("Add"), over ? ui::IconTone::Primary : ui::IconTone::Idle,
                         ui::IconSurface::Sidebar)
                    .paint(painter, add);
            }
            painter->restore();
            return;
        }

        const qreal radius = theme.px(0.4);
        if (selected) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(r.primaryContainer);
            painter->drawRoundedRect(rect.adjusted(0, 1, 0, -1), radius, radius);
        } else if (hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(ui::mix(r.onSurface, 0.08F, r.background));
            painter->drawRoundedRect(rect.adjusted(0, 1, 0, -1), radius, radius);
        }

        const int pad = theme.px(0.4);
        const int iconSize = theme.px(1.0);
        const QRect iconRect(rect.left() + pad, rect.center().y() - (iconSize / 2), iconSize,
                             iconSize);
        // A coloured collection or note shows its glyph in its colour, as its name.
        const QString glyphName = index.data(IconRole).toString();
        const auto color = ui::colorFromKey(index.data(ColorRole).toString());
        const QIcon glyph =
            color && !selected
                ? ui::icon(glyphName, *color, ui::IconSurface::Sidebar)
                : ui::icon(glyphName, selected ? ui::IconTone::Primary : ui::IconTone::Idle,
                           ui::IconSurface::Sidebar);
        glyph.paint(painter, iconRect);

        const QColor text = ui::itemTextColor(index.data(ColorRole).toString(), r, selected);
        int right = rect.right() - pad;
        const int count = index.data(CountRole).toInt();
        painter->setFont(theme.font(ui::TextRole::Hint));
        if (count > 0) {
            const QString number = QLocale().toString(count);
            const int width = painter->fontMetrics().horizontalAdvance(number);
            painter->setPen(selected ? r.onPrimaryContainer : r.onSurfaceVariant);
            painter->drawText(QRect(right - width, rect.top(), width, rect.height()),
                              Qt::AlignVCenter | Qt::AlignRight, number);
            right -= width + pad;
        }
        painter->setFont(theme.font(ui::TextRole::Body));
        painter->setPen(text);
        const QRect textRect(iconRect.right() + pad, rect.top(), right - iconRect.right() - pad,
                             rect.height());
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                          painter->fontMetrics().elidedText(index.data().toString(), Qt::ElideRight,
                                                            textRect.width()));
        painter->restore();
    }

    /// Places the name editor beside the glyph, where the name is drawn.
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                              const QModelIndex & /*index*/) const override
    {
        const ui::Theme &theme = *ui::Theme::current();
        const int left = theme.px(0.4) + theme.px(1.0) + theme.px(0.2);
        editor->setGeometry(option.rect.adjusted(left, 0, 0, 0));
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const ui::Theme &theme = *ui::Theme::current();
        Q_UNUSED(index);
        return {option.rect.width(), theme.px(2.0)};
    }
};

/// Creates an entry with its view, glyph and optional count and colour.
QStandardItem *makeItem(View::Kind kind, const QString &id, const QString &text,
                        const QString &iconName, int count = 0, const QString &color = {})
{
    auto *item = new QStandardItem(text);
    item->setEditable(false);
    item->setData(static_cast<int>(kind), KindRole);
    item->setData(id, IdRole);
    item->setData(count, CountRole);
    item->setData(iconName, IconRole);
    item->setData(color, ColorRole);
    item->setData(View{.kind = kind, .id = id}.key(), KeyRole);
    item->setToolTip(text);
    item->setDragEnabled(false);
    item->setDropEnabled(false);
    return item;
}

/// Creates a section header that folds its entries.
QStandardItem *makeHeader(const QString &key, const QString &text)
{
    auto *item = new QStandardItem(text);
    item->setEditable(false);
    item->setSelectable(false);
    item->setData(true, HeaderRole);
    item->setData(QStringLiteral("section:") + key, KeyRole);
    item->setDragEnabled(false);
    item->setDropEnabled(false);
    return item;
}

/// The sidebar's entries, taking drops of collections (to move them) and of notes (to file
/// them). Drops only request the change; the tree is rebuilt from the library afterwards.
class SidebarModel : public QStandardItemModel
{
public:
    using QStandardItemModel::QStandardItemModel;

    std::function<void(const QString &, const std::optional<QString> &, const QString &)>
        collectionDropped;
    std::function<void(const QStringList &, const QString &, bool)> notesDropped;
    std::function<void(const QString &, const QString &)> favoriteDropped;

    QStringList mimeTypes() const override
    {
        return {mime::Collection, mime::Notes, mime::Favorite};
    }

    Qt::DropActions supportedDropActions() const override
    {
        return Qt::MoveAction | Qt::CopyAction;
    }

    QMimeData *mimeData(const QModelIndexList &indexes) const override
    {
        if (indexes.isEmpty()) {
            return nullptr;
        }
        const QModelIndex index = indexes.first();
        auto *data = new QMimeData;
        if (isFavorite(index)) {
            data->setData(mime::Favorite, index.data(KeyRole).toString().toUtf8());
        } else if (static_cast<View::Kind>(index.data(KindRole).toInt()) ==
                   View::Kind::Collection) {
            data->setData(mime::Collection, index.data(IdRole).toString().toUtf8());
        } else {
            delete data;
            return nullptr;
        }
        return data;
    }

    bool canDropMimeData(const QMimeData *data, Qt::DropAction /*action*/, int row, int /*column*/,
                         const QModelIndex &parent) const override
    {
        if (data->hasFormat(mime::Notes)) {
            return row < 0 && isCollection(parent);
        }
        if (data->hasFormat(mime::Favorite)) {
            return isFavoritesHeader(parent);
        }
        return data->hasFormat(mime::Collection) &&
               (isCollection(parent) ||
                parent.data(KeyRole).toString() == QLatin1StringView("section:collections"));
    }

    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                      const QModelIndex &parent) override
    {
        if (!canDropMimeData(data, action, row, column, parent)) {
            return false;
        }
        if (data->hasFormat(mime::Notes)) {
            const QStringList ids = QString::fromUtf8(data->data(mime::Notes)).split(u'\n');
            // Shift moves the notes out of the collection being viewed.
            const bool move = (QGuiApplication::keyboardModifiers() & Qt::ShiftModifier) != 0;
            notesDropped(ids, parent.data(IdRole).toString(), move);
            return false;
        }
        if (data->hasFormat(mime::Favorite)) {
            const QString before = row >= 0 && row < rowCount(parent)
                                       ? index(row, 0, parent).data(KeyRole).toString()
                                       : QString();
            favoriteDropped(QString::fromUtf8(data->data(mime::Favorite)), before);
            return false;
        }
        const QString id = QString::fromUtf8(data->data(mime::Collection));
        const std::optional<QString> parentId =
            isCollection(parent) ? std::optional<QString>(parent.data(IdRole).toString())
                                 : std::nullopt;
        QString before;
        if (row >= 0 && row < rowCount(parent)) {
            before = index(row, 0, parent).data(IdRole).toString();
        }
        // Dropped onto an entry, the collection moves inside it, at the end.
        collectionDropped(id, parentId, before);
        return false;
    }

private:
    static bool isFavoritesHeader(const QModelIndex &index)
    {
        return index.data(KeyRole).toString() == QLatin1StringView("section:favorites");
    }

    static bool isFavorite(const QModelIndex &index)
    {
        return index.isValid() && isFavoritesHeader(index.parent());
    }

    static bool isCollection(const QModelIndex &index)
    {
        return index.isValid() && !index.data(HeaderRole).toBool() &&
               static_cast<View::Kind>(index.data(KindRole).toInt()) == View::Kind::Collection &&
               index.parent().isValid() &&
               index.parent().data(KeyRole).toString() != QLatin1StringView("section:favorites");
    }
};

} // namespace

Sidebar::Sidebar(QWidget *parent)
    : QWidget(parent)
    , m_model(new SidebarModel(this))
    , m_picker(new ui::IconPicker(this))
    , m_tagColors(new ui::ColorPopup(ui::ColorGrid::Style::Swatch, this))
    , m_tree(new QTreeView(this))
    , m_sync(new QPushButton(this))
    , m_settings(new QPushButton(this))
{
    setProperty("pane", QStringLiteral("sidebar"));
    setAttribute(Qt::WA_StyledBackground);
    const ui::Theme &theme = *ui::Theme::current();

    m_tree->setModel(m_model);
    setFocusProxy(m_tree);
    m_tree->setHeaderHidden(true);
    m_tree->setItemDelegate(new SidebarDelegate(m_tree));
    m_tree->setIndentation(theme.px(0.9));
    m_tree->setMouseTracking(true);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setUniformRowHeights(true);
    m_tree->setExpandsOnDoubleClick(false);
    m_tree->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_tree->setDragEnabled(true);
    m_tree->setAcceptDrops(true);
    m_tree->setDropIndicatorShown(true);
    m_tree->setDragDropMode(QAbstractItemView::DragDrop);
    m_tree->setDefaultDropAction(Qt::MoveAction);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    auto *model = static_cast<SidebarModel *>(m_model);
    model->collectionDropped = [this](const QString &id, const std::optional<QString> &target,
                                      const QString &before) {
        dropCollection(id, target, before);
    };
    model->notesDropped = [this](const QStringList &ids, const QString &collection, bool move) {
        dropNotes(ids, collection, move);
    };
    model->favoriteDropped = [this](const QString &key, const QString &before) {
        dropFavorite(key, before);
    };

    m_settings->setIcon(
        ui::icon(QStringLiteral("Settings"), ui::IconTone::Idle, ui::IconSurface::Sidebar));
    m_settings->setFlat(true);
    m_sync->setFlat(true);
    m_sync->hide();

    auto *layout = new QVBoxLayout(this);
    const int pad = theme.px(0.5);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_sync);
    layout->addWidget(m_settings);

    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &Sidebar::onCurrentChanged);
    connect(m_tree, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        if (index.data(HeaderRole).toBool()) {
            m_tree->setExpanded(index, !m_tree->isExpanded(index));
        } else {
            Q_EMIT entryClicked();
        }
    });
    const auto track = [this](const QModelIndex &index, bool expanded) {
        if (m_rebuilding) {
            return;
        }
        const QString key = index.data(KeyRole).toString();
        m_expanded.removeAll(key);
        if (expanded) {
            m_expanded.append(key);
        }
    };
    connect(m_tree, &QTreeView::expanded, this,
            [track](const QModelIndex &index) { track(index, true); });
    connect(m_tree, &QTreeView::collapsed, this,
            [track](const QModelIndex &index) { track(index, false); });
    connect(m_settings, &QPushButton::clicked, this, &Sidebar::settingsRequested);
    connect(m_sync, &QPushButton::clicked, this, &Sidebar::syncRequested);
    connect(m_tree, &QTreeView::customContextMenuRequested, this, &Sidebar::showContextMenu);
    m_tree->viewport()->installEventFilter(this);
    connect(m_model, &QStandardItemModel::itemChanged, this, &Sidebar::onItemChanged);
    connect(m_tree->itemDelegate(), &QAbstractItemDelegate::closeEditor, this,
            [this](QWidget * /*editor*/, QAbstractItemDelegate::EndEditHint hint) {
                // Escape on a collection just created takes it away again.
                const QString pending = std::exchange(m_pendingCollection, QString());
                if (hint == QAbstractItemDelegate::RevertModelCache && !pending.isEmpty() &&
                    m_session != nullptr) {
                    m_session->mutate([pending](core::LibrarySession &worker) {
                        return worker.deleteCollection(pending);
                    });
                }
            });
    connect(m_picker, &ui::IconPicker::iconChosen, this, [this](const QString &icon) {
        if (m_session != nullptr && !icon.isEmpty()) {
            succeeded(m_session->updateCollection(m_pickerCollection,
                                                  [&](core::Collection &c) { c.icon = icon; }));
        }
    });
    connect(m_picker, &ui::IconPicker::colorChosen, this, [this](const QString &color) {
        if (m_session != nullptr) {
            succeeded(m_session->updateCollection(m_pickerCollection, [&](core::Collection &c) {
                c.color = color.isEmpty() ? std::nullopt : std::optional<QString>(color);
            }));
        }
    });
    // Tag colours live in `library.json` by the tag's key.
    connect(m_tagColors, &ui::ColorPopup::chosen, this, [this](const QString &color) {
        if (m_session != nullptr) {
            succeeded(m_session->updateInfo([&](core::LibraryInfo &info) {
                if (color.isEmpty()) {
                    info.tagColors.remove(core::tagKey(m_colorTag));
                } else {
                    info.tagColors.insert(core::tagKey(m_colorTag), color);
                }
            }));
        }
    });
    retranslate();
}

void Sidebar::setSession(core::LibrarySession *session)
{
    m_session = session;
    reload();
}

QStringList Sidebar::expandedItems() const
{
    return m_expanded;
}

void Sidebar::setExpandedItems(const QStringList &items)
{
    m_expanded = items;
    reload();
}

void Sidebar::selectView(const View &view)
{
    m_current = view;
    const QModelIndexList found = m_model->match(m_model->index(0, 0), KeyRole, view.key(), 1,
                                                 Qt::MatchExactly | Qt::MatchRecursive);
    const QSignalBlocker blocker(m_tree->selectionModel());
    if (found.isEmpty()) {
        m_tree->selectionModel()->clearSelection();
        return;
    }
    for (QModelIndex parent = found.first().parent(); parent.isValid(); parent = parent.parent()) {
        m_tree->setExpanded(parent, true);
    }
    m_tree->setCurrentIndex(found.first());
}

void Sidebar::reload()
{
    m_rebuilding = true;
    m_model->clear();
    if (m_session == nullptr) {
        m_rebuilding = false;
        return;
    }
    const core::Index &index = m_session->index();
    const core::LibraryInfo &info = m_session->library().info();
    const QList<core::Collection> &collections = m_session->collections();
    const auto countOf = [&](const View &view) {
        return index.count(filterFor(view, info, collections)).value_or(0);
    };
    QStandardItem *root = m_model->invisibleRootItem();

    root->appendRow(
        makeItem(View::Kind::All, {}, tr("All notes"), QStringLiteral("AllNotes"), countOf({})));
    root->appendRow(makeItem(View::Kind::Unfiled, {}, tr("Unfiled notes"), QStringLiteral("Inbox"),
                             countOf({.kind = View::Kind::Unfiled, .id = {}})));

    QHash<QString, const core::Collection *> byId;
    for (const core::Collection &collection : collections) {
        byId.insert(collection.id, &collection);
    }

    if (!info.favorites.isEmpty()) {
        QStringList noteIds;
        for (const core::FavoriteRef &favorite : info.favorites) {
            if (favorite.kind == core::FavoriteRef::Kind::Note) {
                noteIds.append(favorite.id);
            }
        }
        const QHash<QString, core::NoteSummary> notes =
            index.notesById(noteIds).value_or(QHash<QString, core::NoteSummary>());
        QStandardItem *favorites = makeHeader(QStringLiteral("favorites"), tr("Favourites"));
        // Favourites are put in order by dragging them within their section.
        const bool writable = !m_session->library().isReadOnly();
        favorites->setDropEnabled(writable);
        for (const core::FavoriteRef &favorite : info.favorites) {
            if (favorite.kind == core::FavoriteRef::Kind::Collection) {
                if (const core::Collection *collection = byId.value(favorite.id)) {
                    favorites->appendRow(makeItem(View::Kind::Collection, collection->id,
                                                  collection->name,
                                                  ui::collectionGlyph(collection->icon), 0,
                                                  collection->color.value_or(QString())));
                }
            } else if (const auto note = notes.constFind(favorite.id); note != notes.cend()) {
                const core::NoteSummary &summary = *note;
                favorites->appendRow(
                    makeItem(View::Kind::FavoriteNote, summary.id,
                             summary.title.isEmpty() ? tr("Untitled") : summary.title,
                             summary.icon.value_or(QStringLiteral("Note")), 0,
                             summary.color.value_or(QString())));
            }
        }
        for (int row = 0; row < favorites->rowCount(); ++row) {
            favorites->child(row)->setDragEnabled(writable);
        }
        root->appendRow(favorites);
    }

    // Collections nest under their parents; one whose parent is missing or forms a cycle with
    // it is shown at the top level, so a damaged file never hides a collection.
    QStandardItem *collectionsHeader = makeHeader(QStringLiteral("collections"), tr("Collections"));
    collectionsHeader->setDropEnabled(true);
    collectionsHeader->setData(!m_session->library().isReadOnly(), AddRole);
    QHash<QString, QStandardItem *> items;
    const QHash<QString, int> counts =
        index.collectionCounts(collections).value_or(QHash<QString, int>());
    for (const core::Collection &collection : collections) {
        QStandardItem *item =
            makeItem(View::Kind::Collection, collection.id, collection.name,
                     ui::collectionGlyph(collection.icon), counts.value(collection.id),
                     collection.color.value_or(QString()));
        // Collections in the tree are renamed in place and moved by dragging.
        const bool writable = !m_session->library().isReadOnly();
        item->setEditable(writable);
        item->setDragEnabled(writable);
        item->setDropEnabled(writable);
        items.insert(collection.id, item);
    }
    const auto attachable = [&](const core::Collection &collection) {
        QSet<QString> seen{collection.id};
        for (std::optional<QString> parent = collection.parent; parent;) {
            const core::Collection *up = byId.value(*parent);
            if (up == nullptr || seen.contains(up->id)) {
                return false;
            }
            seen.insert(up->id);
            parent = up->parent;
        }
        return true;
    };
    for (const core::Collection &collection : collections) {
        QStandardItem *item = items.value(collection.id);
        if (collection.parent && attachable(collection)) {
            items.value(*collection.parent)->appendRow(item);
        } else {
            collectionsHeader->appendRow(item);
        }
    }
    root->appendRow(collectionsHeader);

    const QList<core::TagCount> tags = index.tags().value_or(QList<core::TagCount>());
    if (!tags.isEmpty()) {
        QStandardItem *tagsHeader = makeHeader(QStringLiteral("tags"), tr("Tags"));
        QHash<QString, QStandardItem *> nodes;
        for (const core::TagCount &tag : tags) {
            QStandardItem *parent = tagsHeader;
            QString path;
            for (const QString &part : tag.name.split(u'/', Qt::SkipEmptyParts)) {
                path += (path.isEmpty() ? QString() : QStringLiteral("/")) + part;
                const QString key = path.toCaseFolded();
                QStandardItem *node = nodes.value(key);
                if (node == nullptr) {
                    node = makeItem(View::Kind::Tag, path, part, QStringLiteral("Tag"), 0,
                                    core::tagColor(info.tagColors, path));
                    // Tags are renamed in place, like collections.
                    node->setEditable(!m_session->library().isReadOnly());
                    nodes.insert(key, node);
                    parent->appendRow(node);
                }
                parent = node;
            }
            parent->setData(tag.count, CountRole);
        }
        root->appendRow(tagsHeader);
    }

    root->appendRow(makeItem(View::Kind::Archive, {}, tr("Archive"), QStringLiteral("Archive")));
    root->appendRow(makeItem(View::Kind::Trash, {}, tr("Trash"), QStringLiteral("Delete")));

    for (const QString &key : std::as_const(m_expanded)) {
        const QModelIndexList found = m_model->match(m_model->index(0, 0), KeyRole, key, 1,
                                                     Qt::MatchExactly | Qt::MatchRecursive);
        if (!found.isEmpty()) {
            m_tree->setExpanded(found.first(), true);
        }
    }
    m_rebuilding = false;
    selectView(m_current);
}

void Sidebar::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
        reload();
    }
    QWidget::changeEvent(event);
}

void Sidebar::setSyncStatus(const SyncStatus &status, const QString &toolTip)
{
    m_sync->setVisible(!status.text.isEmpty());
    m_sync->setText(status.failed ? tr("%1 · Retry").arg(status.text) : status.text);
    m_sync->setIcon(ui::icon(status.icon.isEmpty() ? QStringLiteral("Cloud") : status.icon,
                             status.failed ? ui::IconTone::Error : ui::IconTone::Idle,
                             ui::IconSurface::Sidebar));
    m_sync->setToolTip(toolTip);
}

void Sidebar::retranslate()
{
    m_settings->setText(tr("Settings"));
}

void Sidebar::onCurrentChanged(const QModelIndex &index)
{
    if (m_rebuilding || !index.isValid() || index.data(HeaderRole).toBool()) {
        return;
    }
    m_current = {.kind = static_cast<View::Kind>(index.data(KindRole).toInt()),
                 .id = index.data(IdRole).toString()};
    Q_EMIT viewSelected(m_current);
}

void Sidebar::createCollection(const std::optional<QString> &parent)
{
    if (m_session == nullptr || m_session->library().isReadOnly()) {
        return;
    }
    auto created = m_session->createCollection(tr("New collection"), parent);
    if (!created) {
        Q_EMIT failed(core::describe(created.error()));
        return;
    }
    // The session's change rebuilt the tree; the new entry is shown and its name edited.
    const View view{.kind = View::Kind::Collection, .id = created->id};
    switchTo(view);
    const QModelIndexList found = m_model->match(m_model->index(0, 0), KeyRole, view.key(), -1,
                                                 Qt::MatchExactly | Qt::MatchRecursive);
    for (const QModelIndex &index : found) {
        if (index.flags() & Qt::ItemIsEditable) {
            m_pendingCollection = created->id;
            m_tree->scrollTo(index);
            m_tree->edit(index);
            break;
        }
    }
}

void Sidebar::showContextMenu(const QPoint &position)
{
    if (m_session == nullptr || m_session->library().isReadOnly()) {
        return;
    }
    const QModelIndex index = m_tree->indexAt(position);
    QMenu menu(this);
    if (index.data(KeyRole).toString() == QLatin1StringView("section:collections")) {
        connect(menu.addAction(tr("New collection")), &QAction::triggered, this,
                [this] { createCollection(std::nullopt); });
    } else if (!index.data(HeaderRole).toBool() && index.isValid()) {
        const auto kind = static_cast<View::Kind>(index.data(KindRole).toInt());
        const QString id = index.data(IdRole).toString();
        if (kind == View::Kind::Collection) {
            addCollectionActions(menu, index);
        } else if (kind == View::Kind::Tag) {
            addTagActions(menu, index);
        } else if (kind == View::Kind::Trash) {
            QAction *empty = menu.addAction(ui::icon(QStringLiteral("Delete"), ui::IconTone::Error),
                                            tr("Empty trash…"));
            empty->setEnabled(m_session->index()
                                  .count({.view = core::NoteFilter::View::Trash,
                                          .ids = {},
                                          .tag = {},
                                          .anyState = false})
                                  .value_or(0) > 0);
            connect(empty, &QAction::triggered, this, &Sidebar::emptyTrashRequested);
        } else if (kind == View::Kind::FavoriteNote) {
            connect(menu.addAction(tr("Remove from favourites")), &QAction::triggered, this,
                    [this, id] {
                        succeeded(m_session->updateInfo([&](core::LibraryInfo &info) {
                            info.favorites.removeIf([&](const core::FavoriteRef &ref) {
                                return ref.kind == core::FavoriteRef::Kind::Note && ref.id == id;
                            });
                        }));
                    });
        }
    }
    if (!menu.isEmpty()) {
        menu.exec(m_tree->viewport()->mapToGlobal(position));
    }
}

void Sidebar::addCollectionActions(QMenu &menu, const QModelIndex &index)
{
    const QString id = index.data(IdRole).toString();
    const auto &collections = m_session->collections();
    const auto found = std::ranges::find(collections, id, &core::Collection::id);
    if (found == collections.end()) {
        return;
    }
    const core::Collection &collection = *found;
    connect(menu.addAction(tr("New collection inside")), &QAction::triggered, this,
            [this, id] { createCollection(id); });
    // Favourites list collections too, but names are edited in the tree only.
    addRenameAction(menu, index);
    connect(menu.addAction(tr("Icon and colour…")), &QAction::triggered, this, [this, collection] {
        m_pickerCollection = collection.id;
        m_picker->openAt(QCursor::pos(), collection.icon, collection.color.value_or(QString()),
                         false);
    });
    // A new rule that files notes into this collection.
    QAction *rules = menu.addAction(tr("Rules for this collection…"));
    rules->setEnabled(!m_session->library().isReadOnly());
    connect(rules, &QAction::triggered, this, [this, id] {
        core::Rule rule;
        rule.id = core::newId();
        rule.action.collections = {id};
        editRule(window(), *m_session, rule);
    });
    const bool favorite = std::ranges::any_of(
        m_session->library().info().favorites, [&](const core::FavoriteRef &ref) {
            return ref.kind == core::FavoriteRef::Kind::Collection && ref.id == id;
        });
    connect(menu.addAction(favorite ? tr("Remove from favourites") : tr("Add to favourites")),
            &QAction::triggered, this, [this, id, favorite] {
                succeeded(m_session->updateInfo([&](core::LibraryInfo &info) {
                    if (favorite) {
                        info.favorites.removeIf([&](const core::FavoriteRef &ref) {
                            return ref.kind == core::FavoriteRef::Kind::Collection && ref.id == id;
                        });
                    } else {
                        info.favorites.append(
                            {.kind = core::FavoriteRef::Kind::Collection, .id = id});
                    }
                }));
            });
    menu.addSeparator();
    const QAction *remove =
        menu.addAction(ui::icon(QStringLiteral("Delete"), ui::IconTone::Error), tr("Delete…"));
    connect(remove, &QAction::triggered, this, [this, id] { deleteCollection(id); });
}

void Sidebar::addRenameAction(QMenu &menu, const QModelIndex &index)
{
    if (!(index.flags() & Qt::ItemIsEditable)) {
        return;
    }
    QAction *rename = menu.addAction(tr("Rename"));
    rename->setShortcut(QKeySequence(Qt::Key_F2));
    const QPersistentModelIndex persistent(index);
    connect(rename, &QAction::triggered, this, [this, persistent] {
        if (persistent.isValid()) {
            m_tree->edit(persistent);
        }
    });
}

void Sidebar::addTagActions(QMenu &menu, const QModelIndex &index)
{
    const QString tag = index.data(IdRole).toString();
    addRenameAction(menu, index);
    connect(menu.addAction(tr("Colour…")), &QAction::triggered, this, [this, tag] {
        // The popup marks the tag's own colour; Default lets it take its parent's again.
        m_colorTag = tag;
        m_tagColors->openAt(QCursor::pos(),
                            m_session->library().info().tagColors.value(core::tagKey(tag)));
    });
    menu.addSeparator();
    const QAction *remove =
        menu.addAction(ui::icon(QStringLiteral("Delete"), ui::IconTone::Error), tr("Delete…"));
    connect(remove, &QAction::triggered, this, [this, tag] { deleteTag(tag); });
}

void Sidebar::onItemChanged(QStandardItem *item)
{
    if (m_rebuilding || m_session == nullptr) {
        return;
    }
    const auto kind = static_cast<View::Kind>(item->data(KindRole).toInt());
    if (kind == View::Kind::Tag) {
        renameTag(item->data(IdRole).toString(), item->text());
        return;
    }
    if (kind != View::Kind::Collection) {
        return;
    }
    const QString id = item->data(IdRole).toString();
    const QString name = item->text().trimmed();
    if (id == m_pendingCollection) {
        m_pendingCollection.clear();
    }
    if (name.isEmpty()) {
        reload(); // An empty name is not a name: the old one comes back.
        return;
    }
    succeeded(m_session->updateCollection(id, [&](core::Collection &c) { c.name = name; }));
}

bool Sidebar::eventFilter(QObject *watched, QEvent *event)
{
    // The + of the collections header: it lights up under the pointer, explains itself and
    // creates a collection, while the rest of the header folds the section.
    if (watched != m_tree->viewport()) {
        return QWidget::eventFilter(watched, event);
    }
    const auto addAt = [this](const QPoint &position) {
        const QModelIndex index = m_tree->indexAt(position);
        return index.data(AddRole).toBool() &&
               headerAddRect(m_tree->visualRect(index), index.data().toString()).contains(position);
    };
    switch (event->type()) {
    case QEvent::MouseMove: {
        const QModelIndex index =
            m_tree->indexAt(static_cast<QMouseEvent *>(event)->position().toPoint());
        if (index.data(AddRole).toBool()) {
            m_tree->viewport()->update(m_tree->visualRect(index));
        }
        break;
    }
    case QEvent::MouseButtonRelease: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton && addAt(mouse->position().toPoint())) {
            createCollection(std::nullopt);
            return true;
        }
        break;
    }
    case QEvent::ToolTip: {
        const auto *help = static_cast<QHelpEvent *>(event);
        if (addAt(help->pos())) {
            QToolTip::showText(help->globalPos(), tr("New collection"), m_tree->viewport());
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void Sidebar::deleteCollection(const QString &id)
{
    const QStringList subtree = core::collectionSubtree(m_session->collections(), id);
    const auto found = std::ranges::find(m_session->collections(), id, &core::Collection::id);
    if (found == m_session->collections().end()) {
        return;
    }
    const auto members = m_session->index().notes(
        {.view = core::NoteFilter::View::Collections, .ids = subtree, .tag = {}, .anyState = true},
        {});
    if (!members) {
        Q_EMIT failed(core::describe(members.error()));
        return;
    }
    const int notes = static_cast<int>(members->size());
    // Notes in no collection outside the deleted ones, which the trash choice moves there.
    const int orphans = static_cast<int>(std::ranges::count_if(*members, [&](const auto &note) {
        return !note.trashed && std::ranges::all_of(note.collections, [&](const QString &other) {
            return subtree.contains(other);
        });
    }));
    const int children = static_cast<int>(subtree.size()) - 1;
    QStringList text;
    if (children > 0) {
        text.append(tr("Its %n subcollection(s) are deleted too.", nullptr, children));
    }
    text.append(notes > 0 ? tr("What should happen to its %n note(s)?", nullptr, notes)
                          : tr("It holds no notes."));

    // The notes stay in the library, move into another collection, or those in no
    // other collection move to the trash.
    using Notes = core::CollectionNotes;
    Notes choice;
    QWidget *choices = nullptr;
    if (notes > 0) {
        choices = new QWidget;
        auto *keep = new QRadioButton(tr("Keep them in the library and their other collections"));
        auto *move = new QRadioButton(tr("Move them to a collection"));
        auto *target = new QComboBox;
        addCollectionItems(*target, m_session->collections(), subtree);
        auto *trash = new QRadioButton(
            tr("Move to the trash the %n note(s) in no other collection", nullptr, orphans));
        keep->setChecked(true);
        move->setEnabled(target->count() > 0);
        target->setEnabled(false);
        trash->setEnabled(orphans > 0);
        auto *group = new QButtonGroup(choices);
        const std::array options{std::pair{keep, Notes::Kind::Keep},
                                 std::pair{move, Notes::Kind::Move},
                                 std::pair{trash, Notes::Kind::TrashOrphans}};
        for (const auto &[button, kind] : options) {
            group->addButton(button);
            connect(button, &QRadioButton::toggled, choices, [&choice, target, kind](bool on) {
                if (on) {
                    choice.kind = kind;
                    target->setEnabled(kind == Notes::Kind::Move);
                }
            });
        }
        choice.target = target->currentData().toString();
        connect(target, &QComboBox::currentIndexChanged, choices,
                [&choice, target] { choice.target = target->currentData().toString(); });
        auto *layout = new QVBoxLayout(choices);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *moveRow = new QHBoxLayout;
        moveRow->addWidget(move);
        moveRow->addWidget(target, 1);
        layout->addWidget(keep);
        layout->addLayout(moveRow);
        layout->addWidget(trash);
    }
    if (!ui::confirmDestructive(this, tr("Delete the collection “%1”?").arg(found->name),
                                text.join(u' '), tr("Delete"), choices)) {
        return;
    }
    const bool showing = m_current.kind == View::Kind::Collection && subtree.contains(m_current.id);
    const QPointer<Sidebar> self(this);
    m_session->mutate(
        [id, choice](core::LibrarySession &worker) { return worker.deleteCollection(id, choice); },
        [self, showing](const core::Result<void> &result) {
            if (self && result && showing) {
                self->switchTo({});
            }
        });
}

void Sidebar::renameTag(const QString &tag, const QString &typed)
{
    // The editor holds the last part of the path; the new name takes its place under the same
    // parent, and a typed / nests it deeper.
    const qsizetype slash = tag.lastIndexOf(u'/');
    const QString parent = slash < 0 ? QString() : tag.left(slash + 1);
    QString part = typed.trimmed();
    while (part.startsWith(u'#')) {
        part.remove(0, 1);
    }
    const QString name = part.isEmpty() ? QString() : core::normalizedTag(parent + part);
    if (name.isEmpty() || name == tag) {
        reload(); // The old name comes back.
        return;
    }
    const QPointer<Sidebar> self(this);
    auto count = std::make_shared<int>(0);
    m_session->mutate(
        [tag, name, count](core::LibrarySession &worker) -> core::Result<void> {
            auto result = worker.renameTag(tag, name);
            if (!result) {
                return std::unexpected(result.error());
            }
            *count = *result;
            return {};
        },
        [self, tag, name, count](const core::Result<void> &result) {
            if (!self) {
                return;
            }
            if (!result) {
                self->reload();
                return;
            }
            Q_EMIT self->message(tr("Tag renamed in %n note(s).", nullptr, *count));
            if (self->m_current.kind == View::Kind::Tag) {
                if (const auto current = core::renamedTag(self->m_current.id, tag, name)) {
                    self->switchTo({.kind = View::Kind::Tag, .id = *current});
                }
            }
        });
}

void Sidebar::deleteTag(const QString &tag)
{
    const int notes =
        m_session->index()
            .count({.view = core::NoteFilter::View::Tag, .ids = {}, .tag = tag, .anyState = true})
            .value_or(0);
    if (!ui::confirmDestructive(
            this, tr("Delete the tag #%1?").arg(tag),
            tr("It and its subtags are removed from %n note(s); tags typed in the text stay as "
               "plain words.",
               nullptr, notes),
            tr("Delete"))) {
        return;
    }
    const QPointer<Sidebar> self(this);
    m_session->mutate(
        [tag](core::LibrarySession &worker) -> core::Result<void> {
            auto result = worker.deleteTag(tag);
            return result ? core::Result<void>() : std::unexpected(result.error());
        },
        [self, tag](const core::Result<void> &result) {
            if (self && result && self->m_current.kind == View::Kind::Tag &&
                core::isTagWithin(self->m_current.id, tag)) {
                self->switchTo({});
            }
        });
}

void Sidebar::dropCollection(const QString &id, const std::optional<QString> &parent,
                             const QString &before)
{
    if (m_session == nullptr || before == id) {
        return;
    }
    succeeded(m_session->moveCollection(id, parent, before));
}

void Sidebar::dropFavorite(const QString &key, const QString &before)
{
    if (m_session == nullptr || key == before) {
        return;
    }
    // Favourites are keyed in the tree by their view: `collection:<id>` or `favorite:<id>`.
    const auto refOf = [](const QString &viewKey) -> std::optional<core::FavoriteRef> {
        const View view = View::fromKey(viewKey);
        if (view.kind == View::Kind::Collection) {
            return core::FavoriteRef{.kind = core::FavoriteRef::Kind::Collection, .id = view.id};
        }
        if (view.kind == View::Kind::FavoriteNote) {
            return core::FavoriteRef{.kind = core::FavoriteRef::Kind::Note, .id = view.id};
        }
        return std::nullopt;
    };
    const auto moved = refOf(key);
    if (!moved) {
        return;
    }
    const auto target = refOf(before);
    succeeded(m_session->updateInfo([&](core::LibraryInfo &info) {
        info.favorites.removeAll(*moved);
        const qsizetype at = target ? info.favorites.indexOf(*target) : -1;
        info.favorites.insert(at < 0 ? info.favorites.size() : at, *moved);
    }));
}

void Sidebar::dropNotes(const QStringList &ids, const QString &collection, bool move)
{
    if (m_session == nullptr) {
        return;
    }
    const QStringList remove =
        move && m_current.kind == View::Kind::Collection && m_current.id != collection
            ? QStringList{m_current.id}
            : QStringList();
    m_session->mutate([ids, collection, remove](core::LibrarySession &worker) {
        return worker.changeCollections(ids, {collection}, remove);
    });
}

bool Sidebar::succeeded(const core::Result<void> &result)
{
    if (!result) {
        Q_EMIT failed(core::describe(result.error()));
    }
    return result.has_value();
}

void Sidebar::switchTo(const View &view)
{
    selectView(view);
    Q_EMIT viewSelected(view);
}
