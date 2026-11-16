#pragma once

#include <QFrame>
#include <QIcon>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <optional>

class QCheckBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

namespace ui {

/// Shows a sized popup at a point of the screen, moved so it stays inside the screen.
///
/// @param popup The popup window.
/// @param position The top left corner in global coordinates.
void showOnScreen(QWidget *popup, QPoint position);

/// Shows a popup below @p anchor, or above it when it would leave the window, as from the
/// formatting toolbar at the bottom of the editor.
void showNear(QWidget *popup, QWidget *anchor);

/// A transient message (snackbar): one line at the bottom of a window,
/// with Undo or another action when the operation can be reverted. One at a time; a new message
/// replaces the shown one.
class Toast : public QFrame
{
    Q_OBJECT

public:
    /// Creates the toast floating over @p host, which it follows when resized.
    explicit Toast(QWidget *host);

    /// Shows a message for a few seconds.
    ///
    /// @param text The message.
    /// @param action The action's label, such as Undo; empty for none.
    /// @param onAction Called when the action is chosen; the toast then closes.
    void showMessage(const QString &text, const QString &action = {},
                     std::function<void()> onAction = {});

    /// Follows the host's size, so the toast stays at the bottom centre.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void place();

    QLabel *m_text;
    QPushButton *m_action;
    std::function<void()> m_onAction;
    QTimer m_hide;
};

/// A grid of colour tiles: Default, then the tag colours, each drawn as what it will look like,
/// and below them a custom colour chosen in the colour dialog. The current
/// choice is outlined.
class ColorGrid : public QWidget
{
    Q_OBJECT

public:
    /// What the tiles show.
    enum class Style {
        /// A letter in the colour, for text colour.
        Text,
        /// A letter on the colour, for highlights.
        Highlight,
        /// The colour itself, for the colour of a title.
        Swatch,
    };

    explicit ColorGrid(Style style, QWidget *parent = nullptr);

    /// Marks the current colour key; empty for Default.
    void setCurrent(const QString &key);

Q_SIGNALS:
    /// Emitted with the chosen colour key: a tag colour key, a custom colour as `#rrggbb`, or
    /// empty for Default.
    void chosen(const QString &key);

private:
    void rebuild();

    Style m_style;
    QString m_current;
    QGridLayout *m_grid;
};

/// A popup with a colour grid, opened from the editor's text colour and highlight buttons and
/// from a tag's menu in the sidebar.
class ColorPopup : public QFrame
{
    Q_OBJECT

public:
    ColorPopup(ColorGrid::Style style, QWidget *parent = nullptr);

    /// Shows the popup below @p anchor, or above it near the bottom of the window, with
    /// @p current marked.
    void open(QWidget *anchor, const QString &current);

    /// Shows the popup at a point on the screen, such as where a context menu was opened, with
    /// @p current marked.
    void openAt(const QPoint &globalPosition, const QString &current);

Q_SIGNALS:
    void chosen(const QString &key);

private:
    ColorGrid *m_grid;
};

/// A popup that chooses the size of a new table, up to 6 × 6, from a grid of cells that
/// light up under the pointer or the arrow keys, and whether it has a header row, on by
/// default.
class TableSizePopup : public QFrame
{
    Q_OBJECT

public:
    explicit TableSizePopup(QWidget *parent = nullptr);

    /// Shows the popup below @p anchor, or above it near the bottom of the window.
    void open(QWidget *anchor);

Q_SIGNALS:
    void chosen(int rows, int columns, bool header);

protected:
    void changeEvent(QEvent *event) override;

private:
    class Grid;

    void retranslate();

    Grid *m_grid;
    QLabel *m_size;
    QCheckBox *m_header;
};

/// A popup that chooses the icon and the title colour of a note or collection.
/// Both choices apply at once; choosing an icon closes the popup. The glyphs are shown
/// in titled groups and searched by their localized name, their glyph name and their group's
/// title; Return in the search field chooses the first glyph found.
class IconPicker : public QFrame
{
    Q_OBJECT

public:
    explicit IconPicker(QWidget *parent = nullptr);

    /// Shows the popup below @p anchor with the current choices marked.
    ///
    /// @param anchor The button the popup opens from.
    /// @param icon The current glyph name, empty for none.
    /// @param color The current tag colour key, empty for the default colour.
    /// @param allowNoIcon Whether "No icon" is offered (notes), or an icon is required.
    void open(QWidget *anchor, const QString &icon, const QString &color, bool allowNoIcon);

    /// Shows the popup at a point of the screen, for entries that have no button of their own.
    void openAt(const QPoint &globalPosition, const QString &icon, const QString &color,
                bool allowNoIcon);

    /// The glyphs offered for notes and collections, in their order.
    static const QStringList &glyphs();

    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    /// Emitted with the chosen glyph name, or empty for no icon.
    void iconChosen(const QString &icon);
    /// Emitted with the chosen tag colour key, or empty for the default colour.
    void colorChosen(const QString &color);

private:
    /// A glyph's button, its name, the untranslated text of its label and the index of its
    /// group.
    struct GlyphButton
    {
        QToolButton *button;
        QString name;
        const char *label;
        int group;
    };

    /// Updates the texts, tints and marks of the current choices, in the current language.
    void refresh();
    /// Lays out the glyphs matching the search text: all of them under their group titles for
    /// an empty text, otherwise the matches without titles.
    void filter(const QString &text);

    QString m_icon;
    QString m_color;
    bool m_allowNoIcon = true;
    QLabel *m_colorTitle;
    QLabel *m_iconTitle;
    ColorGrid *m_colors;
    QLineEdit *m_search;
    QScrollArea *m_scroll;
    QWidget *m_content;
    QToolButton *m_none;
    QGridLayout *m_icons;
    QLabel *m_noMatches;
    QList<QLabel *> m_groupTitles;
    QList<GlyphButton> m_buttons;
};

/// A multiple-choice filter: a popup list with a checkbox
/// per option, a search field when the list is long and Clear while any option is ticked. Each
/// change applies at once; ticking none switches the filter off.
class MultiChoicePopup : public QFrame
{
    Q_OBJECT

public:
    /// One choice of the list.
    struct Option
    {
        QString key;
        QString label;
        /// Glyph name, or empty for none.
        QString iconName;
        /// Tag colour key of the glyph.
        std::optional<QString> color;
        /// Nesting level, for indenting a tree such as the collections.
        int depth = 0;
    };

    explicit MultiChoicePopup(QWidget *parent = nullptr);

    /// Shows the options below @p anchor with @p chosen ticked.
    void open(QWidget *anchor, const QList<Option> &options, const QStringList &chosen);

    /// The keys of the ticked options in list order.
    QStringList chosen() const;

Q_SIGNALS:
    void changed(const QStringList &chosen);

protected:
    void changeEvent(QEvent *event) override;

private:
    void retranslate();
    /// Shows the options whose label contains @p text.
    void filter(const QString &text);
    /// Shows Clear while any option is ticked.
    void updateClear();

    QLineEdit *m_search;
    QScrollArea *m_scroll;
    QWidget *m_options;
    QVBoxLayout *m_optionsLayout;
    QLabel *m_noMatches;
    QPushButton *m_clear;
    QList<QCheckBox *> m_boxes;
};

/// A popup that chooses one entry of a long list through a search field, as every long list is
/// searchable. Up, Down, Page Up and Page Down move through the list from the field,
/// Return or a click chooses, Escape closes without a choice. Subclasses supply the entries for
/// the text searched.
class SearchPopup : public QFrame
{
    Q_OBJECT

public:
    /// One entry of the list.
    struct Entry
    {
        QString key;
        QString text;
        QIcon icon;
    };

    explicit SearchPopup(QWidget *parent = nullptr);

    /// Shows the popup at a point of the screen with an empty search and the entry @p current
    /// selected, when the list has it.
    ///
    /// @param globalPosition The top left corner, moved to stay on the screen.
    /// @param current The key of the entry to select.
    void openAt(const QPoint &globalPosition, const QString &current = {});

    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    /// Emitted with the chosen entry.
    void chosen(const QString &key, const QString &text);

protected:
    /// Returns the entries for the search text, in the order shown.
    virtual QList<Entry> entries(const QString &text) = 0;

    /// Sets the hint of the search field.
    void setPlaceholder(const QString &text);

private:
    /// Lists the entries for the current search text, selecting the first.
    void refresh();
    void choose();

    QLineEdit *m_search;
    QListWidget *m_list;
};

/// Asks to confirm a destructive action, which stays quiet until confirmed. The confirm button
/// repeats the action in the error colour; Cancel keeps all.
///
/// @param parent The window to centre on.
/// @param title The question, such as "Delete 3 notes permanently?".
/// @param text The consequence.
/// @param action The confirm label, such as "Delete".
/// @param choices Optional controls shown between the text and the buttons, such as what to do
///     with a collection's notes. The dialog takes them as its child and deletes them when it
///     closes, so the caller follows their state through their signals.
/// @return true when the person confirmed.
bool confirmDestructive(QWidget *parent, const QString &title, const QString &text,
                        const QString &action, QWidget *choices = nullptr);

} // namespace ui
