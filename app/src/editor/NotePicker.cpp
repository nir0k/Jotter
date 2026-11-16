#include "editor/NotePicker.h"

#include "core/LibrarySession.h"
#include "ui/Icons.h"
#include "ui/TagColor.h"

#include <QEvent>

namespace editor {

namespace {

/// The list stays short enough to scan; a longer search narrows it.
constexpr qsizetype MaxNotes = 30;

} // namespace

NotePicker::NotePicker(QWidget *parent)
    : ui::SearchPopup(parent)
{
    setPlaceholder(tr("Search note titles"));
}

void NotePicker::setSession(const core::LibrarySession *session)
{
    m_session = session;
}

void NotePicker::setExcluded(const QString &id)
{
    m_excluded = id;
}

QList<ui::SearchPopup::Entry> NotePicker::entries(const QString &text)
{
    if (m_session == nullptr) {
        return {};
    }
    core::Result<QList<core::NoteSummary>> notes;
    if (text.isEmpty()) {
        notes = m_session->index().notes({}, {});
    } else {
        core::Search search;
        search.query = text;
        search.scopes = core::SearchScope::Title;
        search.includeArchived = true;
        notes = m_session->index().search({}, search, m_session->collections(), {});
    }
    QList<Entry> list;
    for (const core::NoteSummary &note : notes.value_or(QList<core::NoteSummary>())) {
        if (note.id == m_excluded) {
            continue;
        }
        QIcon glyph;
        if (note.icon) {
            const auto color = ui::colorFromKey(note.color.value_or(QString()));
            glyph = color ? ui::icon(*note.icon, *color) : ui::icon(*note.icon);
        }
        list.append({.key = note.id,
                     .text = note.title.isEmpty() ? tr("Untitled") : note.title,
                     .icon = glyph});
        if (list.size() == MaxNotes) {
            break;
        }
    }
    return list;
}

void NotePicker::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        setPlaceholder(tr("Search note titles"));
    }
    ui::SearchPopup::changeEvent(event);
}

} // namespace editor
