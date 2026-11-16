#include "app/RuleEditorDialog.h"

#include "app/CollectionCombo.h"
#include "core/LibrarySession.h"
#include "core/Tags.h"
#include "ui/Icons.h"
#include "ui/Popups.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

using Kind = core::RuleCondition::Kind;

/// The condition kinds in the order the editor offers them.
constexpr std::array ConditionOrder{Kind::TagEquals, Kind::NoTags};

/// Creates an inline error label, hidden until a check fails.
QLabel *makeError(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setProperty("inlineError", true);
    label->setWordWrap(true);
    label->hide();
    return label;
}

} // namespace

RuleEditorDialog::RuleEditorDialog(const core::Rule &rule, QList<core::Collection> collections,
                                   QStringList tags, QWidget *parent)
    : QDialog(parent)
    , m_rule(rule)
    , m_collections(std::move(collections))
    , m_tags(std::move(tags))
    , m_targets(rule.action.kind == core::RuleAction::Kind::Add ? rule.action.collections
                                                                : QStringList())
    , m_name(new QLineEdit(rule.name, this))
    , m_nameError(makeError(this))
    , m_match(new ui::SegmentedControl(this))
    , m_conditionsLayout(new QVBoxLayout)
    , m_conditionsError(makeError(this))
    , m_action(new QComboBox(this))
    , m_addTargets(new QPushButton(this))
    , m_moveTarget(new QComboBox(this))
    , m_actionError(makeError(this))
    , m_stop(new QCheckBox(tr("Stop after this rule"), this))
    , m_targetsPopup(new ui::MultiChoicePopup(this))
{
    const ui::Theme &theme = *ui::Theme::current();
    setWindowTitle(rule.name.isEmpty() ? tr("New rule") : tr("Edit rule"));

    m_match->addOption(tr("All conditions"), true);
    m_match->addOption(tr("Any condition"), false);
    m_match->setValue(rule.matchAll);
    m_conditionsLayout->setContentsMargins(0, 0, 0, 0);
    m_conditionsLayout->setSpacing(theme.px(0.4));
    for (const core::RuleCondition &condition : rule.conditions) {
        addCondition(condition);
    }
    if (m_conditions.isEmpty()) {
        addCondition({.kind = Kind::TagEquals, .value = {}});
    }
    auto *addCondition =
        new QPushButton(ui::icon(QStringLiteral("Add")), tr("Add condition"), this);

    m_action->addItem(tr("Add to collections"), static_cast<int>(core::RuleAction::Kind::Add));
    m_action->addItem(tr("Move to collection"), static_cast<int>(core::RuleAction::Kind::Move));
    m_action->setCurrentIndex(rule.action.kind == core::RuleAction::Kind::Move ? 1 : 0);
    addCollectionItems(*m_moveTarget, m_collections);
    if (rule.action.kind == core::RuleAction::Kind::Move && !rule.action.collections.isEmpty()) {
        m_moveTarget->setCurrentIndex(m_moveTarget->findData(rule.action.collections.first()));
    }
    m_stop->setChecked(rule.stop);
    m_stop->setToolTip(tr("Rules after this one are skipped for a note that matches it."));

    auto *actionRow = new QHBoxLayout;
    actionRow->addWidget(m_action);
    actionRow->addWidget(m_addTargets, 1);
    actionRow->addWidget(m_moveTarget, 1);

    auto *form = new QFormLayout;
    form->setVerticalSpacing(theme.px(0.5));
    form->addRow(tr("Name"), m_name);
    form->addRow(QString(), m_nameError);
    form->addRow(tr("Notes match"), m_match);
    auto *conditions = new QVBoxLayout;
    conditions->addLayout(m_conditionsLayout);
    conditions->addWidget(m_conditionsError);
    auto *conditionButtons = new QHBoxLayout;
    conditionButtons->addWidget(addCondition);
    conditionButtons->addStretch();
    conditions->addLayout(conditionButtons);
    form->addRow(tr("Conditions"), conditions);
    form->addRow(tr("Action"), actionRow);
    form->addRow(QString(), m_actionError);
    form->addRow(QString(), m_stop);

    auto *hint = new QLabel(tr("A rule acts when a note starts matching it: when the note is "
                               "created, or its tags change. Archived and trashed notes are left "
                               "alone."),
                            this);
    hint->setWordWrap(true);
    ui::setTextRole(hint, ui::TextRole::Hint);

    auto *buttons = new QDialogButtonBox(this);
    QPushButton *save = buttons->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
    save->setProperty("role", QStringLiteral("primary"));
    save->setDefault(true);
    buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(theme.px(0.75));
    layout->addLayout(form);
    layout->addWidget(hint);
    layout->addStretch();
    layout->addWidget(buttons);
    resize(theme.px(38), sizeHint().height());

    connect(addCondition, &QPushButton::clicked, this, [this] {
        this->addCondition({.kind = Kind::TagEquals, .value = {}});
        m_conditions.last().value->setFocus();
    });
    connect(m_action, &QComboBox::currentIndexChanged, this, &RuleEditorDialog::updateAction);
    connect(m_addTargets, &QPushButton::clicked, this, [this] {
        QList<ui::MultiChoicePopup::Option> options;
        for (const auto &[collection, depth] : core::collectionTree(m_collections)) {
            options.append({.key = collection->id,
                            .label = collection->name,
                            .iconName = collection->icon,
                            .color = collection->color,
                            .depth = depth});
        }
        m_targetsPopup->open(m_addTargets, options, m_targets);
    });
    connect(m_targetsPopup, &ui::MultiChoicePopup::changed, this,
            [this](const QStringList &chosen) {
                m_targets = chosen;
                updateAction();
            });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, [this] {
        if (validate()) {
            accept();
        }
    });
    updateAction();
}

core::Rule RuleEditorDialog::rule() const
{
    core::Rule rule = m_rule;
    rule.name = m_name->text().trimmed();
    rule.matchAll = m_match->value().toBool();
    rule.conditions.clear();
    for (const ConditionRow &row : m_conditions) {
        const auto kind = static_cast<Kind>(row.kind->currentData().toInt());
        rule.conditions.append(
            {.kind = kind, .value = kind == Kind::NoTags ? QString() : row.value->text()});
    }
    rule.action.kind = static_cast<core::RuleAction::Kind>(m_action->currentData().toInt());
    if (rule.action.kind == core::RuleAction::Kind::Move) {
        const QString target = m_moveTarget->currentData().toString();
        rule.action.collections = target.isEmpty() ? QStringList() : QStringList{target};
    } else {
        rule.action.collections = m_targets;
    }
    rule.stop = m_stop->isChecked();
    return rule;
}

void RuleEditorDialog::addCondition(const core::RuleCondition &condition)
{
    auto *row = new QWidget(this);
    auto *rowLayout = new QVBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(ui::Theme::current()->px(0.2));
    auto *fields = new QHBoxLayout;
    ConditionRow controls{.row = row,
                          .kind = new QComboBox(row),
                          .value = new QLineEdit(condition.value, row),
                          .remove = new QToolButton(row),
                          .error = makeError(row)};
    for (const Kind kind : ConditionOrder) {
        QString text;
        switch (kind) {
        case Kind::TagEquals:
            text = tr("Has the tag");
            break;
        case Kind::NoTags:
            text = tr("Has no tags");
            break;
        }
        controls.kind->addItem(text, static_cast<int>(kind));
    }
    controls.kind->setCurrentIndex(controls.kind->findData(static_cast<int>(condition.kind)));
    auto *completer = new QCompleter(m_tags, controls.value);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->popup()->setProperty("popupList", true);
    controls.value->setCompleter(completer);
    controls.value->setPlaceholderText(tr("Tag, such as work/meetings"));
    controls.remove->setIcon(ui::icon(QStringLiteral("Close")));
    controls.remove->setToolTip(tr("Remove condition"));
    controls.remove->setAccessibleName(tr("Remove condition"));
    fields->addWidget(controls.kind);
    fields->addWidget(controls.value, 1);
    fields->addWidget(controls.remove);
    rowLayout->addLayout(fields);
    rowLayout->addWidget(controls.error);
    m_conditionsLayout->addWidget(row);
    m_conditions.append(controls);
    connect(controls.kind, &QComboBox::currentIndexChanged, this,
            [this, controls] { updateConditionRow(controls); });
    connect(controls.remove, &QToolButton::clicked, this, [this, row] { removeCondition(row); });
    updateConditionRow(controls);
}

void RuleEditorDialog::removeCondition(QWidget *row)
{
    m_conditions.removeIf([row](const ConditionRow &controls) { return controls.row == row; });
    row->deleteLater();
}

void RuleEditorDialog::updateConditionRow(const ConditionRow &row)
{
    const auto kind = static_cast<Kind>(row.kind->currentData().toInt());
    row.value->setVisible(kind != Kind::NoTags);
    row.error->hide();
}

void RuleEditorDialog::updateAction()
{
    const bool move =
        m_action->currentData().toInt() == static_cast<int>(core::RuleAction::Kind::Move);
    m_addTargets->setVisible(!move);
    m_moveTarget->setVisible(move);
    QStringList names;
    for (const QString &id : std::as_const(m_targets)) {
        const auto found = std::ranges::find(m_collections, id, &core::Collection::id);
        if (found != m_collections.end()) {
            names.append(found->name);
        }
    }
    m_addTargets->setText(names.isEmpty() ? tr("Choose collections…")
                                          : names.join(QStringLiteral(", ")));
    m_actionError->hide();
}

bool RuleEditorDialog::validate()
{
    QWidget *firstInvalid = nullptr;
    const auto report = [&firstInvalid](QLabel *error, const QString &text, QWidget *field) {
        error->setText(text);
        error->setVisible(!text.isEmpty());
        if (!text.isEmpty() && firstInvalid == nullptr) {
            firstInvalid = field;
        }
    };
    report(m_nameError,
           m_name->text().trimmed().isEmpty() ? tr("Enter a name for the rule.") : QString(),
           m_name);
    report(m_conditionsError, m_conditions.isEmpty() ? tr("Add a condition.") : QString(), m_name);
    for (const ConditionRow &row : std::as_const(m_conditions)) {
        const auto kind = static_cast<Kind>(row.kind->currentData().toInt());
        report(row.error,
               kind != Kind::NoTags && core::normalizedTag(row.value->text()).isEmpty()
                   ? tr("Enter a tag.")
                   : QString(),
               row.value);
    }
    QString actionError;
    if (m_collections.isEmpty()) {
        actionError = tr("Create a collection first: rules file notes into collections.");
    } else if (rule().action.collections.isEmpty()) {
        actionError = tr("Choose a collection.");
    }
    report(m_actionError, actionError, m_action);
    if (firstInvalid != nullptr) {
        firstInvalid->setFocus();
        return false;
    }
    return true;
}

bool editRule(QWidget *parent, core::LibrarySession &session, const core::Rule &rule)
{
    QStringList tags;
    for (const core::TagCount &tag : session.index().tags().value_or(QList<core::TagCount>())) {
        tags.append(tag.name);
    }
    RuleEditorDialog dialog(rule, session.collections(), tags, parent);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    const core::Rule edited = dialog.rule();
    QList<core::Rule> rules = session.rules();
    const auto found = std::ranges::find(rules, edited.id, &core::Rule::id);
    if (found != rules.end()) {
        *found = edited;
    } else {
        rules.append(edited);
    }
    if (auto saved = session.saveRules(rules); !saved) {
        QMessageBox::warning(parent, RuleEditorDialog::tr("Rules"), core::describe(saved.error()));
        return false;
    }
    return true;
}
