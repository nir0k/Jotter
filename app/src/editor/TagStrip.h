#pragma once

#include <QMap>
#include <QStringList>
#include <QWidget>

#include <functional>

class QCompleter;
class QLabel;
class QLineEdit;
class QStringListModel;
class QToolButton;
namespace ui {
class FlowLayout;
}

namespace editor {

/// The tags of the open note, under its title: the Tag glyph and "Tags:", a
/// pill for each tag and + to add one. + opens a field that completes the library's tags; Enter
/// adds the typed tag and keeps the field open for the next one, and the field closes on Enter
/// while empty, on Escape or when it is left. A pill offers × on hover and is removed with
/// Delete when focused. The row wraps when the tags do not fit.
class TagStrip : public QWidget
{
    Q_OBJECT

public:
    explicit TagStrip(QWidget *parent = nullptr);

    /// Shows the tags of a note, in their order.
    void setTags(const QStringList &tags);

    /// Sets the tag colours of the library (tag colour keys by tag key), which pills are drawn
    /// in.
    void setTagColors(const QMap<QString, QString> &colors);

    /// Sets where the field's completions come from: all tags of the library.
    void setSuggestionSource(std::function<QStringList()> source);

    /// Hides + and ×, for archived and trashed notes; a read-only note without tags hides the
    /// strip.
    void setReadOnly(bool readOnly);

    /// Opens the field for a new tag and moves the focus there.
    void startAdding();

    /// Closes the field when it loses the focus, keeping a typed tag.
    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    /// Emitted with the note's new tags after one was added or removed.
    void tagsChanged(const QStringList &tags);

protected:
    void changeEvent(QEvent *event) override;

private:
    void rebuild();
    void retranslate();
    /// Sets the glyphs and the sizes that follow the theme.
    void updateIcons();
    /// Offers the library's tags the note does not have yet as the field's completions.
    void updateSuggestions();
    /// Adds the typed tag; an empty or present tag changes nothing.
    void addTyped();
    /// Closes the field and shows + again.
    void finishAdding();
    void remove(const QString &tag);

    QStringList m_tags;
    QMap<QString, QString> m_tagColors;
    bool m_readOnly = false;
    std::function<QStringList()> m_suggestionSource;
    ui::FlowLayout *m_layout;
    QLabel *m_icon;
    QLabel *m_caption;
    QToolButton *m_add;
    QLineEdit *m_field;
    QStringListModel *m_suggestions;
    QCompleter *m_completer;
    QList<QWidget *> m_pills;
};

} // namespace editor
