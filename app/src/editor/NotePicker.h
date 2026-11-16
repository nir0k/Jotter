#pragma once

#include "ui/Popups.h"

namespace core {
class LibrarySession;
}

namespace editor {

/// A popup that chooses a note to link to: titles are searched in every word
/// form as the person types; with an empty search the recently updated notes are offered.
/// Notes in the trash are not offered.
class NotePicker : public ui::SearchPopup
{
    Q_OBJECT

public:
    explicit NotePicker(QWidget *parent = nullptr);

    /// Sets the library the notes come from; nullptr offers none.
    void setSession(const core::LibrarySession *session);

    /// Leaves a note out of the list, such as the note being edited.
    void setExcluded(const QString &id);

protected:
    QList<Entry> entries(const QString &text) override;
    void changeEvent(QEvent *event) override;

private:
    const core::LibrarySession *m_session = nullptr;
    QString m_excluded;
};

} // namespace editor
