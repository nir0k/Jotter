#pragma once

#include "app/SyncStatus.h"
#include "app/View.h"
#include "core/Error.h"

#include <QStandardItemModel>
#include <QWidget>

#include <optional>

namespace core {
class LibrarySession;
}
namespace ui {
class ColorPopup;
class IconPicker;
} // namespace ui
class QMenu;
class QPushButton;
class QTreeView;

/// The sidebar: All notes, Unfiled notes, Favourites, the collection tree, the
/// tag tree, Archive and Trash, with Settings in the footer.
///
/// Collections are created, renamed, coloured, moved by dragging and deleted here, and tags
/// renamed and deleted; notes dragged from the list onto a collection
/// join it.
class Sidebar : public QWidget
{
    Q_OBJECT

public:
    explicit Sidebar(QWidget *parent = nullptr);

    /// Shows the library of @p session; nullptr clears the sidebar.
    void setSession(core::LibrarySession *session);

    /// Selects a view without emitting viewSelected().
    void selectView(const View &view);

    /// Expanded collections, tags and sections, for saving the window state.
    QStringList expandedItems() const;
    void setExpandedItems(const QStringList &items);

    /// Rebuilds the entries with fresh counts, keeping selection and expansion.
    void reload();

    /// Shows the state of synchronisation in the footer; an empty text
    /// hides it, for a library that is not synchronised.
    ///
    /// @param status The state in words, with its glyph.
    /// @param toolTip More words, such as the error of a failed run.
    void setSyncStatus(const SyncStatus &status, const QString &toolTip);

    /// Creates a collection inside @p parent, or at the top level, and starts editing its name;
    /// Escape removes it again.
    void createCollection(const std::optional<QString> &parent);

    /// Handles the + of the collections header in the tree: it lights up under the pointer,
    /// shows its tooltip and creates a collection when clicked.
    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    void viewSelected(const View &view);
    /// Emitted when an entry is clicked, even the current one, so a narrow window can move on
    /// to the list.
    void entryClicked();
    void settingsRequested();
    /// Emitted when the person clicks the sync status: Sync now, or Retry after an error.
    void syncRequested();
    /// Emitted when an operation failed, with a message for the person.
    void failed(const QString &message);
    /// Emitted with a transient message about a finished operation.
    void message(const QString &text);
    /// Emitted when Empty trash is chosen in the trash's menu.
    void emptyTrashRequested();

protected:
    void changeEvent(QEvent *event) override;

private:
    void retranslate();
    void onCurrentChanged(const QModelIndex &index);
    void showContextMenu(const QPoint &position);
    void addCollectionActions(QMenu &menu, const QModelIndex &index);
    /// Adds Rename (F2), which edits the entry's name in place, for an editable entry.
    void addRenameAction(QMenu &menu, const QModelIndex &index);
    void addTagActions(QMenu &menu, const QModelIndex &index);
    void onItemChanged(QStandardItem *item);
    void deleteCollection(const QString &id);
    /// Renames a tag after its name was edited in the tree, and reports in how many notes.
    ///
    /// @param tag The tag's full path.
    /// @param typed The new last part of the path, as typed.
    void renameTag(const QString &tag, const QString &typed);
    void deleteTag(const QString &tag);
    void dropCollection(const QString &id, const std::optional<QString> &parent,
                        const QString &before);
    void dropNotes(const QStringList &ids, const QString &collection, bool move);
    void dropFavorite(const QString &key, const QString &before);
    /// Shows the message of a failed operation; returns whether it succeeded.
    bool succeeded(const core::Result<void> &result);
    /// Selects a view and shows it, as if the person had chosen it.
    void switchTo(const View &view);

    core::LibrarySession *m_session = nullptr;
    QStandardItemModel *m_model;
    ui::IconPicker *m_picker;
    /// The collection just created, removed again when its name editor is cancelled.
    QString m_pendingCollection;
    /// The collection whose icon and colour the picker changes.
    QString m_pickerCollection;
    ui::ColorPopup *m_tagColors;
    /// The tag whose colour the colour popup changes.
    QString m_colorTag;
    QTreeView *m_tree;
    QPushButton *m_sync;
    QPushButton *m_settings;
    View m_current;
    QStringList m_expanded;
    bool m_rebuilding = false;
};
