#pragma once

#include "ui/Icons.h"
#include "ui/Palette.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QFrame>
#include <QLayout>
#include <QVariant>
#include <QWidget>

class QLabel;
class QPushButton;

namespace ui {

/// An empty state: a glyph, a short title, the next step and, when the person
/// may act, one action.
class EmptyState : public QWidget
{
    Q_OBJECT

public:
    explicit EmptyState(QWidget *parent = nullptr);

    /// Sets the content; an empty @p action hides the button.
    void setContent(const QString &iconName, const QString &title, const QString &text,
                    const QString &action = {});

Q_SIGNALS:
    void actionTriggered();

private:
    void updateIcon();

    QString m_iconName;
    QLabel *m_icon;
    QLabel *m_title;
    QLabel *m_text;
    QPushButton *m_action;
};

/// A tinted notice panel that explains a state in more words, with an optional action
/// (`error`, `warning` or `info`).
class Notice : public QFrame
{
    Q_OBJECT

public:
    enum class Kind { Error, Warning, Info };

    explicit Notice(QWidget *parent = nullptr);

    /// Sets the content and shows the notice; an empty action hides its button.
    void showNotice(Kind kind, const QString &text, const QString &action = {},
                    const QString &secondaryAction = {});

Q_SIGNALS:
    void actionTriggered();
    void secondaryActionTriggered();

private:
    void updateIcon();

    Kind m_kind = Kind::Info;
    QLabel *m_icon;
    QLabel *m_text;
    QPushButton *m_action;
    QPushButton *m_secondaryAction;
};

/// Segmented buttons: one control that picks exactly one of two to four short options shown
/// side by side. A choice applies at once.
class SegmentedControl : public QFrame
{
    Q_OBJECT

public:
    explicit SegmentedControl(QWidget *parent = nullptr);

    /// Appends an option carrying @p value.
    void addOption(const QString &text, const QVariant &value);

    /// Renames the option carrying @p value, for retranslation.
    void setOptionText(const QVariant &value, const QString &text);

    /// Selects the option carrying @p value without emitting changed().
    void setValue(const QVariant &value);
    QVariant value() const;

Q_SIGNALS:
    /// Emitted when the person picks another option.
    void changed(const QVariant &value);

private:
    QButtonGroup m_group;
    QList<QVariant> m_values;
};

/// A theme picker: every palette drawn in its own colours, both variants side
/// by side, so the person chooses by sight. Choosing applies at once.
class ThemePicker : public QWidget
{
    Q_OBJECT

public:
    explicit ThemePicker(QWidget *parent = nullptr);

    void setPalette(PaletteId palette);

Q_SIGNALS:
    void paletteChosen(ui::PaletteId palette);

private:
    QButtonGroup m_group;
};

/// Wraps rows into a bordered section with a title: a settings section.
///
/// @param title The section title; it gets the section title role.
/// @param rows The rows, top to bottom.
/// @return The section, without a parent.
QFrame *settingsSection(QLabel *title, std::initializer_list<QWidget *> rows);

/// Puts settings sections into a scrollable page that scrolls vertically only.
QWidget *settingsPage(std::initializer_list<QWidget *> sections);

/// Lays widgets out in a row, left-aligned.
QWidget *settingsRow(std::initializer_list<QWidget *> widgets);

/// Lays widgets out in rows that wrap at the available width, such as tag pills. Items keep
/// their size hints and are centred vertically in their row.
class FlowLayout : public QLayout
{
public:
    /// @param parent The widget to lay out, or nullptr.
    /// @param spacing The gap between items, horizontally and between rows.
    explicit FlowLayout(QWidget *parent, int spacing);
    ~FlowLayout() override;

    void addItem(QLayoutItem *item) override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QLayoutItem *takeAt(int index) override;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect &rect) override;

private:
    /// Places the items in @p rect, or only measures them when @p apply is false.
    ///
    /// @return The height the rows take.
    int arrange(const QRect &rect, bool apply) const;

    QList<QLayoutItem *> m_items;
    int m_gap;
};

} // namespace ui
