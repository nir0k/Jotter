#pragma once

#include <QDialog>

#include <optional>

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

/// Chooses the collections or the tags of one or several notes.
///
/// Each entry shows whether all, some or none of the notes have it. Ticking adds it to all of
/// them, clearing removes it from all, and a partly ticked entry left as it is changes nothing.
/// The tag variant can also create tags.
class MembershipDialog : public QDialog
{
    Q_OBJECT

public:
    /// One collection or tag.
    struct Entry
    {
        QString id;
        QString label;
        QString iconName;
        std::optional<QString> color;
        /// Nesting level, for indenting collections.
        int depth = 0;
        Qt::CheckState state = Qt::Unchecked;
    };

    /// @param title The window title, such as "Collections".
    /// @param entries The choices in display order.
    /// @param newEntryHint Placeholder of the field that adds entries; empty hides the field.
    MembershipDialog(const QString &title, const QList<Entry> &entries, const QString &newEntryHint,
                     QWidget *parent = nullptr);

    /// The entries to add to every note.
    QStringList added() const;
    /// The entries to remove from every note.
    QStringList removed() const;

private:
    void addEntry(const Entry &entry);
    void filter(const QString &text);
    void addTyped();

    QLineEdit *m_search;
    QTreeWidget *m_list;
    QLineEdit *m_newEntry;
};
