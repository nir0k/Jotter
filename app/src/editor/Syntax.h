#pragma once

#include <QList>
#include <QString>

namespace KSyntaxHighlighting {
class Definition;
class Repository;
} // namespace KSyntaxHighlighting

/// Code block languages: the syntax definitions of KDE SyntaxHighlighting.
/// A note stores a language as the lower-case name of its definition.
namespace editor {

/// A language offered for code blocks.
struct SyntaxLanguage
{
    /// The value stored in the note.
    QString key;
    /// The name shown, in the interface language.
    QString name;
};

/// Returns the syntax definitions, read from disk once, on first use.
KSyntaxHighlighting::Repository &syntaxRepository();

/// Returns the languages offered for code blocks, sorted by name: a short list of commonly used
/// languages rather than every definition.
QList<SyntaxLanguage> codeLanguages();

/// Finds the definition of a stored or typed language: by name without regard to case, then by
/// the shorter name the list shows, then as a file extension, so ` ```php `, ` ```py ` and
/// `data-language="cpp"` find PHP, Python and C++.
///
/// @param language The language, such as `go` or `py`.
/// @return The definition; invalid for plain text and unknown languages.
KSyntaxHighlighting::Definition syntaxDefinition(const QString &language);

/// Finds the definition of a language in a repository of its own, as syntaxDefinition() does.
/// The shared repository serves the interface thread only; work on another thread, such as an
/// export, reads the definitions into its own.
///
/// @param language The language, such as `go` or `py`.
/// @param repository The definitions to search.
/// @return The definition; invalid for plain text and unknown languages.
KSyntaxHighlighting::Definition syntaxDefinition(const QString &language,
                                                 const KSyntaxHighlighting::Repository &repository);

/// Returns the value to store for a typed language: the key of its definition, or the typed
/// text in lower case when no definition matches.
QString languageKey(const QString &language);

/// Returns the name shown for a stored language: the list's shorter name or the definition's
/// name in the interface language, the stored value itself when no definition matches, or empty
/// for plain text.
QString languageName(const QString &language);

/// Returns the name shown for a stored language, as languageName() does, from a repository of
/// its own (see syntaxDefinition()).
///
/// @param language The stored language.
/// @param repository The definitions to search.
/// @return The name shown; empty for plain text.
QString languageName(const QString &language, const KSyntaxHighlighting::Repository &repository);

} // namespace editor
