#include "app/MembershipDialog.h"

#include "core/Tags.h"
#include "ui/Icons.h"
#include "ui/TagColor.h"
#include "ui/Theme.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

enum Role {
    IdRole = Qt::UserRole + 1,
    InitialRole,
};

} // namespace

MembershipDialog::MembershipDialog(const QString &title, const QList<Entry> &entries,
                                   const QString &newEntryHint, QWidget *parent)
    : QDialog(parent)
    , m_search(new QLineEdit(this))
    , m_list(new QTreeWidget(this))
    , m_newEntry(new QLineEdit(this))
{
    setWindowTitle(title);
    const ui::Theme &theme = *ui::Theme::current();
    m_search->setPlaceholderText(tr("Search"));
    m_search->setProperty("search", true);
    m_search->setClearButtonEnabled(true);
    m_search->addAction(ui::icon(QStringLiteral("Search")), QLineEdit::LeadingPosition);
    m_list->setHeaderHidden(true);
    m_list->setRootIsDecorated(false);
    m_list->setIndentation(0);
    m_list->setMinimumHeight(theme.px(16));
    for (const Entry &entry : entries) {
        addEntry(entry);
    }

    auto *newRow = new QHBoxLayout;
    auto *add = new QPushButton(ui::icon(QStringLiteral("Add")), tr("Add"), this);
    newRow->addWidget(m_newEntry, 1);
    newRow->addWidget(add);
    m_newEntry->setPlaceholderText(newEntryHint);
    m_newEntry->setVisible(!newEntryHint.isEmpty());
    add->setVisible(!newEntryHint.isEmpty());

    auto *cancel = new QPushButton(tr("Cancel"), this);
    auto *save = new QPushButton(tr("Save"), this);
    save->setProperty("role", QStringLiteral("primary"));
    save->setDefault(true);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(save);

    auto *layout = new QVBoxLayout(this);
    const int pad = theme.px(1.0);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(theme.px(0.6));
    layout->addWidget(m_search);
    layout->addWidget(m_list, 1);
    layout->addLayout(newRow);
    layout->addLayout(buttons);
    resize(theme.px(24), theme.px(30));

    connect(m_search, &QLineEdit::textChanged, this, &MembershipDialog::filter);
    connect(m_newEntry, &QLineEdit::returnPressed, this, &MembershipDialog::addTyped);
    connect(add, &QPushButton::clicked, this, &MembershipDialog::addTyped);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, &QDialog::accept);
}

void MembershipDialog::addEntry(const Entry &entry)
{
    auto *item = new QTreeWidgetItem(m_list);
    // Collections are indented by spaces so the tree reads as a tree in one flat list.
    item->setText(0, QString(static_cast<qsizetype>(entry.depth) * 4, u' ') + entry.label);
    if (!entry.iconName.isEmpty()) {
        const auto color = ui::colorFromKey(entry.color.value_or(QString()));
        item->setIcon(0, color ? ui::icon(entry.iconName, *color) : ui::icon(entry.iconName));
    }
    item->setData(0, IdRole, entry.id);
    item->setData(0, InitialRole, static_cast<int>(entry.state));
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsUserCheckable;
    if (entry.state == Qt::PartiallyChecked) {
        // Only a mixed entry offers the third state, so it can be left unchanged.
        flags |= Qt::ItemIsUserTristate;
    }
    item->setFlags(flags);
    item->setCheckState(0, entry.state);
}

QStringList MembershipDialog::added() const
{
    QStringList ids;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *item = m_list->topLevelItem(i);
        if (item->checkState(0) == Qt::Checked &&
            item->data(0, InitialRole).toInt() != Qt::Checked) {
            ids.append(item->data(0, IdRole).toString());
        }
    }
    return ids;
}

QStringList MembershipDialog::removed() const
{
    QStringList ids;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *item = m_list->topLevelItem(i);
        if (item->checkState(0) == Qt::Unchecked &&
            item->data(0, InitialRole).toInt() != Qt::Unchecked) {
            ids.append(item->data(0, IdRole).toString());
        }
    }
    return ids;
}

void MembershipDialog::filter(const QString &text)
{
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_list->topLevelItem(i);
        item->setHidden(!text.isEmpty() && !item->text(0).contains(text, Qt::CaseInsensitive));
    }
}

void MembershipDialog::addTyped()
{
    const QString text = core::normalizedTag(m_newEntry->text());
    if (text.isEmpty()) {
        return;
    }
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_list->topLevelItem(i);
        if (item->data(0, IdRole).toString().compare(text, Qt::CaseInsensitive) == 0) {
            item->setCheckState(0, Qt::Checked);
            m_newEntry->clear();
            return;
        }
    }
    addEntry({.id = text,
              .label = text,
              .iconName = QStringLiteral("Tag"),
              .color = std::nullopt,
              .depth = 0,
              .state = Qt::Unchecked});
    m_list->topLevelItem(m_list->topLevelItemCount() - 1)->setCheckState(0, Qt::Checked);
    m_newEntry->clear();
}
