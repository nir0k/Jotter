#pragma once

#include "core/Index.h"
#include "core/Library.h"
#include "core/Search.h"

#include <QDate>
#include <QFrame>

class QDateEdit;
class QLabel;
class QPushButton;
class QToolButton;
namespace ui {
class MultiChoicePopup;
}

/// A popup that chooses a range of days, from the first to the last day inclusive, for the
/// Created and Updated filters.
class DateRangePopup : public QFrame
{
    Q_OBJECT

public:
    explicit DateRangePopup(QWidget *parent = nullptr);

    /// Shows the popup below @p anchor with the range; an invalid @p from proposes the last
    /// month.
    void open(QWidget *anchor, QDate from, QDate to);

Q_SIGNALS:
    /// Emitted by Apply with the range in order.
    void chosen(QDate from, QDate to);
    /// Emitted by Clear.
    void cleared();

protected:
    void changeEvent(QEvent *event) override;

private:
    void retranslate();

    QLabel *m_fromLabel;
    QDateEdit *m_from;
    QLabel *m_toLabel;
    QDateEdit *m_to;
    QPushButton *m_clear;
    QPushButton *m_apply;
};

/// The filter panel of the note list: one
/// chip per filter showing its value — Search in, Created, Updated, Collections, Tags, With
/// attachments, With geotag, Search everywhere, Include archived — and Clear while any filter is
/// applied. Every change applies at once; closing the panel keeps the filters.
class SearchPanel : public QFrame
{
    Q_OBJECT

public:
    explicit SearchPanel(QWidget *parent = nullptr);

    /// Sets the collections and tags the filters offer. Chosen collections that no longer
    /// exist are dropped from the filter.
    void setSources(QList<core::Collection> collections, QList<core::TagCount> tags);

    /// Shows the chips that widen a search where they apply.
    ///
    /// @param everywhere Whether Search everywhere is offered: in views other than All notes,
    ///        the archive and the trash.
    /// @param archived Whether Include archived is offered: outside the archive and the trash,
    ///        which are searched only in their own views.
    void setWidening(bool everywhere, bool archived);

    /// Copies the scopes and the filters into @p search.
    void apply(core::Search &search) const;

    /// Tells whether Search everywhere is offered and on.
    bool searchesEverywhere() const;

    /// Tells whether any filter differs from its default, the scopes included.
    bool isFiltering() const;

    /// Removes every filter without emitting changed().
    void clear();

Q_SIGNALS:
    void changed();

protected:
    void changeEvent(QEvent *event) override;

private:
    /// A range of days; an invalid start means the filter is off.
    struct DateRange
    {
        QDate from;
        QDate to;
    };

    /// The list the shared multiple-choice popup is choosing.
    enum class Choosing { Scopes, Collections, Tags };

    void retranslate();
    /// Shows each chip's value and the active state, and Clear while filtering.
    void updateChips();
    /// Opens the multiple-choice popup for a list under its chip.
    void choose(Choosing list);
    /// Opens the date range popup for Created or Updated.
    void chooseDates(bool created);

    QList<core::Collection> m_collections;
    QList<core::TagCount> m_tags;
    core::SearchScopes m_scopes = core::AllScopes;
    DateRange m_created;
    DateRange m_updated;
    QStringList m_chosenCollections;
    QStringList m_chosenTags;
    bool m_offerEverywhere = false;
    bool m_offerArchived = false;
    Choosing m_choosing = Choosing::Scopes;
    bool m_datesCreated = true;

    QToolButton *m_scopesChip;
    QToolButton *m_createdChip;
    QToolButton *m_updatedChip;
    QToolButton *m_collectionsChip;
    QToolButton *m_tagsChip;
    QToolButton *m_attachmentsChip;
    QToolButton *m_locationChip;
    QToolButton *m_everywhereChip;
    QToolButton *m_archivedChip;
    QToolButton *m_clear;
    ui::MultiChoicePopup *m_choices;
    DateRangePopup *m_dates;
};
