#include "app/SearchPanel.h"

#include "core/Tags.h"
#include "ui/Icons.h"
#include "ui/Popups.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QCoreApplication>
#include <QDateEdit>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

/// The proposed range of a date filter that is off: the last month.
constexpr int ProposedDays = 30;

/// Creates a filter chip: one that opens a choice shows a chevron after its value, a toggle
/// chip is checked while its filter is on.
QToolButton *makeChip(QWidget *parent, bool toggle)
{
    auto *chip = new QToolButton(parent);
    chip->setProperty("chip", true);
    chip->setCheckable(toggle);
    if (toggle) {
        chip->setToolButtonStyle(Qt::ToolButtonTextOnly);
    } else {
        chip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        chip->setIcon(ui::icon(QStringLiteral("ChevronDown")));
        const int size = ui::Theme::current()->px(0.7);
        chip->setIconSize(QSize(size, size));
        // The chevron follows the value.
        chip->setLayoutDirection(Qt::RightToLeft);
    }
    return chip;
}

/// The scopes in the order the filter lists them, with their keys.
constexpr std::array ScopeKeys{std::pair{core::SearchScope::Title, "title"},
                               std::pair{core::SearchScope::Text, "text"},
                               std::pair{core::SearchScope::Tags, "tags"},
                               std::pair{core::SearchScope::Collections, "collections"}};

/// Returns the name of a scope in the interface language.
QString scopeName(core::SearchScope scope)
{
    switch (scope) {
    case core::SearchScope::Title:
        return QCoreApplication::translate("SearchPanel", "Title");
    case core::SearchScope::Text:
        return QCoreApplication::translate("SearchPanel", "Text");
    case core::SearchScope::Tags:
        return QCoreApplication::translate("SearchPanel", "Tags");
    case core::SearchScope::Collections:
        return QCoreApplication::translate("SearchPanel", "Collections");
    }
    return {};
}

} // namespace

DateRangePopup::DateRangePopup(QWidget *parent)
    : QFrame(parent, Qt::Popup)
    , m_fromLabel(new QLabel(this))
    , m_from(new QDateEdit(this))
    , m_toLabel(new QLabel(this))
    , m_to(new QDateEdit(this))
    , m_clear(new QPushButton(this))
    , m_apply(new QPushButton(this))
{
    setProperty("popup", true);
    setAttribute(Qt::WA_StyledBackground);
    const ui::Theme &theme = *ui::Theme::current();
    for (QDateEdit *edit : {m_from, m_to}) {
        edit->setCalendarPopup(true);
    }
    for (QLabel *label : {m_fromLabel, m_toLabel}) {
        ui::setTextRole(label, ui::TextRole::Label);
    }
    m_fromLabel->setBuddy(m_from);
    m_toLabel->setBuddy(m_to);
    m_apply->setProperty("role", QStringLiteral("primary"));
    m_apply->setDefault(true);

    auto *fields = new QGridLayout;
    fields->setHorizontalSpacing(theme.px(0.6));
    fields->setVerticalSpacing(theme.px(0.5));
    fields->addWidget(m_fromLabel, 0, 0);
    fields->addWidget(m_from, 0, 1);
    fields->addWidget(m_toLabel, 1, 0);
    fields->addWidget(m_to, 1, 1);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_clear);
    buttons->addWidget(m_apply);
    auto *layout = new QVBoxLayout(this);
    const int pad = theme.px(0.8);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(theme.px(0.8));
    layout->addLayout(fields);
    layout->addLayout(buttons);

    connect(m_apply, &QPushButton::clicked, this, [this] {
        hide();
        const QDate from = m_from->date();
        const QDate to = m_to->date();
        Q_EMIT chosen(std::min(from, to), std::max(from, to));
    });
    connect(m_clear, &QPushButton::clicked, this, [this] {
        hide();
        Q_EMIT cleared();
    });
    retranslate();
}

void DateRangePopup::open(QWidget *anchor, QDate from, QDate to)
{
    const QDate today = QDate::currentDate();
    m_from->setDate(from.isValid() ? from : today.addDays(-ProposedDays));
    m_to->setDate(from.isValid() ? to : today);
    ui::showNear(this, anchor);
    m_from->setFocus();
}

void DateRangePopup::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QFrame::changeEvent(event);
}

void DateRangePopup::retranslate()
{
    m_fromLabel->setText(tr("From"));
    m_toLabel->setText(tr("To"));
    m_clear->setText(tr("Clear"));
    m_apply->setText(tr("Apply"));
}

SearchPanel::SearchPanel(QWidget *parent)
    : QFrame(parent)
    , m_scopesChip(makeChip(this, false))
    , m_createdChip(makeChip(this, false))
    , m_updatedChip(makeChip(this, false))
    , m_collectionsChip(makeChip(this, false))
    , m_tagsChip(makeChip(this, false))
    , m_attachmentsChip(makeChip(this, true))
    , m_locationChip(makeChip(this, true))
    , m_everywhereChip(makeChip(this, true))
    , m_archivedChip(makeChip(this, true))
    , m_clear(new QToolButton(this))
    , m_choices(new ui::MultiChoicePopup(this))
    , m_dates(new DateRangePopup(this))
{
    setProperty("filterPanel", true);
    setAttribute(Qt::WA_StyledBackground);
    const ui::Theme &theme = *ui::Theme::current();
    m_clear->setIcon(ui::icon(QStringLiteral("Close")));
    m_clear->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto *layout = new ui::FlowLayout(this, theme.px(0.4));
    const int pad = theme.px(0.5);
    layout->setContentsMargins(pad, pad, pad, pad);
    for (QWidget *chip :
         {m_scopesChip, m_createdChip, m_updatedChip, m_collectionsChip, m_tagsChip,
          m_attachmentsChip, m_locationChip, m_everywhereChip, m_archivedChip, m_clear}) {
        layout->addWidget(chip);
    }
    m_everywhereChip->hide();
    m_archivedChip->hide();

    connect(m_scopesChip, &QToolButton::clicked, this, [this] { choose(Choosing::Scopes); });
    connect(m_collectionsChip, &QToolButton::clicked, this,
            [this] { choose(Choosing::Collections); });
    connect(m_tagsChip, &QToolButton::clicked, this, [this] { choose(Choosing::Tags); });
    connect(m_createdChip, &QToolButton::clicked, this, [this] { chooseDates(true); });
    connect(m_updatedChip, &QToolButton::clicked, this, [this] { chooseDates(false); });
    for (const QToolButton *toggle :
         {m_attachmentsChip, m_locationChip, m_everywhereChip, m_archivedChip}) {
        connect(toggle, &QToolButton::toggled, this, [this] {
            updateChips();
            Q_EMIT changed();
        });
    }
    connect(m_clear, &QToolButton::clicked, this, [this] {
        clear();
        Q_EMIT changed();
    });
    connect(m_choices, &ui::MultiChoicePopup::changed, this, [this](const QStringList &chosen) {
        switch (m_choosing) {
        case Choosing::Scopes:
            m_scopes = {};
            for (const auto &[scope, key] : ScopeKeys) {
                if (chosen.contains(QLatin1StringView(key))) {
                    m_scopes |= scope;
                }
            }
            // Ticking none switches the filter off: every scope is searched again.
            if (m_scopes == core::SearchScopes()) {
                m_scopes = core::AllScopes;
            }
            break;
        case Choosing::Collections:
            m_chosenCollections = chosen;
            break;
        case Choosing::Tags:
            m_chosenTags = chosen;
            break;
        }
        updateChips();
        Q_EMIT changed();
    });
    connect(m_dates, &DateRangePopup::chosen, this, [this](QDate from, QDate to) {
        (m_datesCreated ? m_created : m_updated) = {.from = from, .to = to};
        updateChips();
        Q_EMIT changed();
    });
    connect(m_dates, &DateRangePopup::cleared, this, [this] {
        (m_datesCreated ? m_created : m_updated) = {};
        updateChips();
        Q_EMIT changed();
    });
    retranslate();
}

void SearchPanel::setSources(QList<core::Collection> collections, QList<core::TagCount> tags)
{
    m_collections = std::move(collections);
    m_tags = std::move(tags);
    const qsizetype chosen = m_chosenCollections.size();
    m_chosenCollections.removeIf([this](const QString &id) {
        return std::ranges::none_of(m_collections, [&id](const core::Collection &collection) {
            return collection.id == id;
        });
    });
    if (m_chosenCollections.size() != chosen) {
        updateChips();
    }
}

void SearchPanel::setWidening(bool everywhere, bool archived)
{
    m_offerEverywhere = everywhere;
    m_offerArchived = archived;
    m_everywhereChip->setVisible(everywhere);
    m_archivedChip->setVisible(archived);
    updateChips();
}

void SearchPanel::apply(core::Search &search) const
{
    search.scopes = m_scopes;
    const auto startOf = [](QDate day) { return day.startOfDay().toUTC(); };
    for (const auto &[range, from, until] :
         {std::tuple{&m_created, &search.createdFrom, &search.createdUntil},
          std::tuple{&m_updated, &search.updatedFrom, &search.updatedUntil}}) {
        if (range->from.isValid()) {
            *from = startOf(range->from);
            *until = startOf(range->to.addDays(1));
        } else {
            *from = std::nullopt;
            *until = std::nullopt;
        }
    }
    search.collections = m_chosenCollections;
    search.tags = m_chosenTags;
    search.withAttachments = m_attachmentsChip->isChecked();
    search.withLocation = m_locationChip->isChecked();
    search.includeArchived = m_offerArchived && m_archivedChip->isChecked();
}

bool SearchPanel::searchesEverywhere() const
{
    return m_offerEverywhere && m_everywhereChip->isChecked();
}

bool SearchPanel::isFiltering() const
{
    return m_scopes != core::AllScopes || m_created.from.isValid() || m_updated.from.isValid() ||
           !m_chosenCollections.isEmpty() || !m_chosenTags.isEmpty() ||
           m_attachmentsChip->isChecked() || m_locationChip->isChecked() || searchesEverywhere() ||
           (m_offerArchived && m_archivedChip->isChecked());
}

void SearchPanel::clear()
{
    m_scopes = core::AllScopes;
    m_created = {};
    m_updated = {};
    m_chosenCollections.clear();
    m_chosenTags.clear();
    for (QToolButton *toggle :
         {m_attachmentsChip, m_locationChip, m_everywhereChip, m_archivedChip}) {
        const QSignalBlocker blocker(toggle);
        toggle->setChecked(false);
    }
    updateChips();
}

void SearchPanel::choose(Choosing list)
{
    m_choosing = list;
    QList<ui::MultiChoicePopup::Option> options;
    QStringList chosen;
    QToolButton *anchor = m_scopesChip;
    switch (list) {
    case Choosing::Scopes:
        for (const auto &[scope, key] : ScopeKeys) {
            options.append({.key = QString::fromLatin1(key),
                            .label = scopeName(scope),
                            .iconName = {},
                            .color = std::nullopt,
                            .depth = 0});
            // Every scope is the filter switched off, shown with nothing ticked.
            if (m_scopes != core::AllScopes && m_scopes.testFlag(scope)) {
                chosen.append(QString::fromLatin1(key));
            }
        }
        break;
    case Choosing::Collections:
        anchor = m_collectionsChip;
        for (const auto &[collection, depth] : core::collectionTree(m_collections)) {
            options.append({.key = collection->id,
                            .label = collection->name,
                            .iconName = collection->icon,
                            .color = collection->color,
                            .depth = depth});
        }
        chosen = m_chosenCollections;
        break;
    case Choosing::Tags:
        anchor = m_tagsChip;
        for (const core::TagCount &tag : std::as_const(m_tags)) {
            options.append({.key = tag.name,
                            .label = tag.name,
                            .iconName = QStringLiteral("Tag"),
                            .color = std::nullopt,
                            .depth = 0});
        }
        // A chosen tag that is now only on archived notes stays choosable.
        for (const QString &tag : std::as_const(m_chosenTags)) {
            if (std::ranges::none_of(m_tags, [&tag](const core::TagCount &known) {
                    return core::tagKey(known.name) == core::tagKey(tag);
                })) {
                options.append({.key = tag,
                                .label = tag,
                                .iconName = QStringLiteral("Tag"),
                                .color = std::nullopt,
                                .depth = 0});
            }
        }
        chosen = m_chosenTags;
        break;
    }
    m_choices->open(anchor, options, chosen);
}

void SearchPanel::chooseDates(bool created)
{
    m_datesCreated = created;
    const DateRange &range = created ? m_created : m_updated;
    m_dates->open(created ? m_createdChip : m_updatedChip, range.from, range.to);
}

void SearchPanel::updateChips()
{
    const auto setValue = [](QToolButton *chip, const QString &text, bool active) {
        chip->setText(text);
        chip->setAccessibleName(text);
        if (chip->property("active").toBool() != active) {
            ui::setStyleProperty(chip, "active", active);
        }
    };

    const QLocale locale;
    // A chip of a filter with several values shows the one chosen value, or how many there are.
    const auto withValues = [&locale](const QString &text, const QStringList &values) {
        if (values.size() == 1) {
            return text.arg(values.first());
        }
        return text.arg(locale.toString(values.size()));
    };
    const bool allScopes = m_scopes == core::AllScopes;
    QStringList scopeNames;
    for (const auto &[scope, key] : ScopeKeys) {
        if (m_scopes.testFlag(scope)) {
            scopeNames.append(scopeName(scope));
        }
    }
    setValue(m_scopesChip,
             allScopes ? tr("Search in: everywhere") : withValues(tr("Search in: %1"), scopeNames),
             !allScopes);

    const auto rangeText = [&locale](const DateRange &range) {
        const QString from = locale.toString(range.from, QLocale::ShortFormat);
        return range.from == range.to
                   ? from
                   : tr("%1 – %2").arg(from, locale.toString(range.to, QLocale::ShortFormat));
    };
    setValue(m_createdChip,
             m_created.from.isValid() ? tr("Created: %1").arg(rangeText(m_created)) : tr("Created"),
             m_created.from.isValid());
    setValue(m_updatedChip,
             m_updated.from.isValid() ? tr("Updated: %1").arg(rangeText(m_updated)) : tr("Updated"),
             m_updated.from.isValid());

    QStringList collectionNames;
    for (const QString &id : std::as_const(m_chosenCollections)) {
        const auto found = std::ranges::find(m_collections, id, &core::Collection::id);
        collectionNames.append(found != m_collections.end() ? found->name : QString());
    }
    setValue(m_collectionsChip,
             collectionNames.isEmpty() ? tr("Collections")
                                       : withValues(tr("Collections: %1"), collectionNames),
             !collectionNames.isEmpty());
    setValue(m_tagsChip,
             m_chosenTags.isEmpty() ? tr("Tags") : withValues(tr("Tags: %1"), m_chosenTags),
             !m_chosenTags.isEmpty());
    m_clear->setVisible(isFiltering());
}

void SearchPanel::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QFrame::changeEvent(event);
}

void SearchPanel::retranslate()
{
    m_attachmentsChip->setText(tr("With attachments"));
    m_locationChip->setText(tr("With geotag"));
    m_everywhereChip->setText(tr("Search everywhere"));
    m_everywhereChip->setToolTip(tr("Search all notes instead of this view"));
    m_archivedChip->setText(tr("Include archived"));
    m_clear->setText(tr("Clear"));
    m_clear->setToolTip(tr("Remove all filters"));
    updateChips();
}
