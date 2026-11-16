#pragma once

#include "core/Index.h"
#include "editor/NoteAction.h"

#include <QAbstractListModel>
#include <QMap>
#include <QSet>
#include <QTimer>
#include <QWidget>

class FilterToggle;
class SearchPanel;
class QLabel;
class QLineEdit;
class QListView;
class QMenu;
class QPushButton;
class QStackedWidget;
class QToolButton;
namespace ui {
class EmptyState;
class Notice;
} // namespace ui

/// Returns a short date for lists: the time for today, "Yesterday", the day and month within
/// this year, otherwise the full date, in the interface language.
QString shortDate(const QDateTime &time);

/// The notes of the current view, in their order, for the list.
class NoteListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    using QAbstractListModel::QAbstractListModel;

    void setNotes(QList<core::NoteSummary> notes, core::NoteSort::Key dateKey);
    /// Sets the favourite notes, which rows mark with a star.
    void setFavorites(QSet<QString> favorites);
    bool isFavorite(const QString &id) const { return m_favorites.contains(id); }
    /// Sets the tag colours of the library, which the rows' tag pills are drawn in.
    void setTagColors(QMap<QString, QString> colors);
    const QMap<QString, QString> &tagColors() const { return m_tagColors; }
    const core::NoteSummary &summary(int row) const { return m_notes.at(row); }
    core::NoteSort::Key dateKey() const { return m_dateKey; }

    /// Returns the row of a note, or -1.
    int rowOf(const QString &id) const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    /// Notes can be dragged onto collections in the sidebar.
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;

private:
    QList<core::NoteSummary> m_notes;
    QSet<QString> m_favorites;
    QMap<QString, QString> m_tagColors;
    core::NoteSort::Key m_dateKey = core::NoteSort::Key::Updated;
};

/// The note list pane: the view's title and count, the New note button, the
/// search field with the filter toggle and the sort menu, the filter panel, the notes as compact
/// rows, the selection bar for several notes, and the empty, no-matches and unavailable states.
class NoteList : public QWidget
{
    Q_OBJECT

public:
    /// Which actions the list offers: ordinary views, the archive or the trash.
    enum class Mode { Notes, Archive, Trash };

    explicit NoteList(QWidget *parent = nullptr);

    /// Sets the actions of the current view and whether notes can be created and changed.
    void setMode(Mode mode, bool writable);

    /// Sets the favourite notes, which rows mark with a star.
    void setFavorites(QSet<QString> favorites);

    /// Sets the tag colours of the library (tag colour keys by tag key), which the rows' tag
    /// pills are drawn in.
    void setTagColors(QMap<QString, QString> colors);

    /// Shows the toggle between a collection's own notes and those of its subcollections too.
    ///
    /// @param visible Whether the current view is a collection with subcollections.
    /// @param included Whether notes of subcollections are listed.
    void setSubcollectionsToggle(bool visible, bool included);

    /// Returns the selected notes in list order.
    QStringList selectedNotes() const;

    /// Clears a selection of several notes on Escape in the list.
    bool eventFilter(QObject *watched, QEvent *event) override;

    /// Shows the notes of a view; @p empty describes the view when it has none.
    void setNotes(const QString &title, QList<core::NoteSummary> notes, const core::NoteSort &sort);

    /// Sets what the empty view says.
    void setEmptyText(const QString &iconName, const QString &title, const QString &text);

    /// Selects a note without emitting noteSelected(); an unknown id clears the selection.
    void selectNote(const QString &id);
    QString currentNote() const;

    /// Tells the always visible sidebar button whether the sidebar is shown, for its label.
    void setSidebarShown(bool shown);

    /// Shows the notice for a library folder that cannot be reached.
    void setUnavailable(bool unavailable, const QString &path);

    /// Returns the search of the list: the query with the filter panel's scopes and filters.
    core::Search search() const;

    /// Tells whether the search covers all notes instead of the view (Search everywhere).
    bool searchesEverywhere() const;

    /// Sets the collections and tags the filters offer.
    void setSearchSources(QList<core::Collection> collections, QList<core::TagCount> tags);

    /// Offers the filters that widen a search where they apply (see SearchPanel::setWidening()).
    void setSearchWidening(bool everywhere, bool archived);

    /// Moves the keyboard focus to the search field with its text selected (Ctrl+Shift+F).
    void focusSearch();

    /// Removes the query and every filter without emitting searchChanged().
    void clearSearch();

Q_SIGNALS:
    void noteSelected(const QString &id);
    void sortChanged(const core::NoteSort &sort);
    void sidebarRequested();
    void retryRequested();
    void newNoteRequested();
    void emptyTrashRequested();
    /// Emitted when the person switches notes of subcollections on or off.
    void subcollectionsToggled(bool included);
    void actionRequested(editor::NoteAction action, const QStringList &ids);
    /// Emitted when the query (after a short pause in typing) or a filter changed.
    void searchChanged();

protected:
    void changeEvent(QEvent *event) override;

private:
    void retranslate();
    void updateSortMenu();
    /// Shows the selection bar instead of the header while several notes are selected.
    void updateSelectionBar();
    /// Fills a menu with the actions for @p ids in the current mode.
    void addActions(QMenu *menu, const QStringList &ids);
    /// Marks the filter toggle while any filter is applied.
    void updateFilterToggle();

    NoteListModel m_model;
    core::NoteSort m_sort;
    QToolButton *m_sidebarButton;
    QLabel *m_title;
    QLabel *m_count;
    QLineEdit *m_search;
    FilterToggle *m_filterToggle;
    SearchPanel *m_panel;
    /// Searches 150 ms after typing stops.
    QTimer m_searchDelay;
    QToolButton *m_sortButton;
    QMenu *m_sortMenu;
    QToolButton *m_newButton;
    QToolButton *m_subcollections;
    QPushButton *m_emptyTrash;
    QWidget *m_headerBox;
    QWidget *m_selectionBar;
    QLabel *m_selectionCount;
    QToolButton *m_selectionActions;
    QToolButton *m_clearSelection;
    Mode m_mode = Mode::Notes;
    bool m_writable = true;
    ui::Notice *m_notice;
    QStackedWidget *m_stack;
    QListView *m_list;
    ui::EmptyState *m_empty;
    ui::EmptyState *m_noMatches;
    QString m_unavailablePath;
    bool m_sidebarShown = true;
};
