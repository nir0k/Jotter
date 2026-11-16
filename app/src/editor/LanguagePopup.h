#pragma once

#include "ui/Popups.h"

namespace editor {

/// A popup that chooses the language of a code block: a search field over
/// the syntax definitions with Plain text first.
class LanguagePopup : public ui::SearchPopup
{
    Q_OBJECT

public:
    explicit LanguagePopup(QWidget *parent = nullptr);

    /// Shows the popup at a point of the screen with the current language marked.
    ///
    /// @param globalPosition The top left corner, moved to stay on the screen.
    /// @param current The stored language, empty for plain text.
    void open(const QPoint &globalPosition, const QString &current);

protected:
    /// Lists Plain text, the current language when the list does not offer it, then the
    /// offered languages whose name or key contains @p text.
    QList<Entry> entries(const QString &text) override;
    void changeEvent(QEvent *event) override;

private:
    /// The key of the block's language.
    QString m_current;
};

} // namespace editor
