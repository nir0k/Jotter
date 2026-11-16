#pragma once

#include "core/Library.h"
#include "core/Rules.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QVBoxLayout;
namespace core {
class LibrarySession;
}
namespace ui {
class MultiChoicePopup;
class SegmentedControl;
} // namespace ui

/// The rule editor (form dialog): a name, conditions on the tags
/// joined by All or Any, the action with its collections and Stop after this rule. Save checks
/// every field and shows what is wrong in place; nothing is saved while a field is invalid.
class RuleEditorDialog : public QDialog
{
    Q_OBJECT

public:
    /// @param rule The rule to edit, or a new one with its identifier set.
    /// @param collections The collection tree the action chooses from.
    /// @param tags The library's tags, completed in the tag of a condition.
    /// @param parent The window to centre on.
    RuleEditorDialog(const core::Rule &rule, QList<core::Collection> collections, QStringList tags,
                     QWidget *parent = nullptr);

    /// The edited rule; its identifier, times and unknown fields are those it was opened with.
    core::Rule rule() const;

private:
    /// The controls of one condition.
    struct ConditionRow
    {
        QWidget *row;
        QComboBox *kind;
        QLineEdit *value;
        QToolButton *remove;
        QLabel *error;
    };

    void addCondition(const core::RuleCondition &condition);
    void removeCondition(QWidget *row);
    /// Shows the value field of a condition only where its kind takes one.
    void updateConditionRow(const ConditionRow &row);
    /// Shows the controls of the chosen action and the chosen collections.
    void updateAction();
    /// Checks every field, showing what is wrong next to it.
    ///
    /// @return Whether the rule can be saved.
    bool validate();

    core::Rule m_rule;
    QList<core::Collection> m_collections;
    QStringList m_tags;
    QStringList m_targets;
    QLineEdit *m_name;
    QLabel *m_nameError;
    ui::SegmentedControl *m_match;
    QVBoxLayout *m_conditionsLayout;
    QList<ConditionRow> m_conditions;
    QLabel *m_conditionsError;
    QComboBox *m_action;
    QPushButton *m_addTargets;
    QComboBox *m_moveTarget;
    QLabel *m_actionError;
    QCheckBox *m_stop;
    ui::MultiChoicePopup *m_targetsPopup;
};

/// Opens the rule editor for @p rule and saves the result: an existing rule is replaced in
/// place, a new one goes to the end of the list. A failure is reported in a message box.
///
/// @param parent The window to centre on.
/// @param session The open library.
/// @param rule The rule, or a new one with its identifier and any prefilled fields.
/// @return Whether the rule was saved.
bool editRule(QWidget *parent, core::LibrarySession &session, const core::Rule &rule);
