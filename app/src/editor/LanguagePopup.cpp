#include "editor/LanguagePopup.h"

#include "editor/Syntax.h"

#include <QEvent>

#include <algorithm>

namespace editor {

LanguagePopup::LanguagePopup(QWidget *parent)
    : ui::SearchPopup(parent)
{
    setPlaceholder(tr("Search languages"));
}

void LanguagePopup::open(const QPoint &globalPosition, const QString &current)
{
    m_current = current.isEmpty() ? QString() : languageKey(current);
    openAt(globalPosition, m_current);
}

QList<ui::SearchPopup::Entry> LanguagePopup::entries(const QString &text)
{
    const auto matches = [&text](const QString &name, const QString &key) {
        return text.isEmpty() || name.contains(text, Qt::CaseInsensitive) ||
               key.contains(text, Qt::CaseInsensitive);
    };
    QList<Entry> list;
    const QString plain = tr("Plain text");
    if (matches(plain, {})) {
        list.append({.key = {}, .text = plain, .icon = {}});
    }
    const QList<SyntaxLanguage> languages = codeLanguages();
    // A language outside the list, typed or from a file, stays choosable while it is current.
    if (!m_current.isEmpty() &&
        std::ranges::find(languages, m_current, &SyntaxLanguage::key) == languages.end() &&
        matches(languageName(m_current), m_current)) {
        list.append({.key = m_current, .text = languageName(m_current), .icon = {}});
    }
    for (const SyntaxLanguage &language : languages) {
        if (matches(language.name, language.key)) {
            list.append({.key = language.key, .text = language.name, .icon = {}});
        }
    }
    return list;
}

void LanguagePopup::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        setPlaceholder(tr("Search languages"));
    }
    ui::SearchPopup::changeEvent(event);
}

} // namespace editor
