#include "editor/Syntax.h"

#include <QCoreApplication>
#include <QHash>

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>

#include <algorithm>
#include <array>
#include <functional>

namespace editor {

namespace {

/// A language offered in the code block's language list.
struct Offered
{
    /// The syntax definition's name.
    const char *definition;
    /// A shorter name shown instead of the definition's, or nullptr.
    const char *name;
};

/// The languages offered for code blocks: those notes commonly hold, not
/// every definition of KDE SyntaxHighlighting, which also covers configuration files of single
/// programs and rare languages.
constexpr std::array<Offered, 50> OfferedLanguages{{
    {"Bash", nullptr},
    {"MS-DOS Batch", QT_TRANSLATE_NOOP("editor::Syntax", "Batch")},
    {"C", nullptr},
    {"C#", nullptr},
    {"C++", nullptr},
    {"Clojure", nullptr},
    {"CMake", nullptr},
    {"CSS", nullptr},
    {"CSV", nullptr},
    {"Dart", nullptr},
    {"Diff", nullptr},
    {"Dockerfile", nullptr},
    {"Elixir", nullptr},
    {"Erlang", nullptr},
    {"Go", nullptr},
    {"GraphQL", nullptr},
    {"Haskell", nullptr},
    {"HTML", nullptr},
    {"Java", nullptr},
    {"JavaScript", nullptr},
    {"JavaScript React (JSX)", QT_TRANSLATE_NOOP("editor::Syntax", "JSX")},
    {"JSON", nullptr},
    {"Julia", nullptr},
    {"Kotlin", nullptr},
    {"Lua", nullptr},
    {"Makefile", nullptr},
    {"Markdown", nullptr},
    {"nginx Configuration", QT_TRANSLATE_NOOP("editor::Syntax", "nginx")},
    {"Objective-C", nullptr},
    {"Perl", nullptr},
    {"PHP/PHP", QT_TRANSLATE_NOOP("editor::Syntax", "PHP")},
    {"PowerShell", nullptr},
    {"Protobuf", nullptr},
    {"Python", nullptr},
    {"QML", nullptr},
    {"R Script", QT_TRANSLATE_NOOP("editor::Syntax", "R")},
    {"Ruby", nullptr},
    {"Rust", nullptr},
    {"Scala", nullptr},
    {"SQL", nullptr},
    {"Swift", nullptr},
    {"systemd unit", nullptr},
    {"TOML", nullptr},
    {"TypeScript", nullptr},
    {"TypeScript React (TSX)", QT_TRANSLATE_NOOP("editor::Syntax", "TSX")},
    {"Vue", nullptr},
    {"XML", nullptr},
    {"YAML", nullptr},
    {"Zig", nullptr},
    {"Zsh", nullptr},
}};

/// The definitions by their stored key.
const QHash<QString, KSyntaxHighlighting::Definition> &definitionsByKey()
{
    static const QHash<QString, KSyntaxHighlighting::Definition> byKey = [] {
        QHash<QString, KSyntaxHighlighting::Definition> definitions;
        for (const KSyntaxHighlighting::Definition &definition : syntaxRepository().definitions()) {
            definitions.insert(definition.name().toLower(), definition);
        }
        return definitions;
    }();
    return byKey;
}

/// Finds the offered language whose shorter name is @p language, without regard to case, so
/// ` ```php ` finds the definition the list offers as PHP.
const Offered *offeredByName(const QString &language)
{
    const auto *const it = std::ranges::find_if(OfferedLanguages, [&](const Offered &offered) {
        return offered.name != nullptr &&
               QString::fromLatin1(offered.name).compare(language, Qt::CaseInsensitive) == 0;
    });
    return it != OfferedLanguages.end() ? &*it : nullptr;
}

/// Returns the shorter name of a stored language, translated, or empty when it has none.
QString shortName(const QString &key)
{
    for (const Offered &offered : OfferedLanguages) {
        if (offered.name != nullptr && QString::fromLatin1(offered.definition).toLower() == key) {
            return QCoreApplication::translate("editor::Syntax", offered.name);
        }
    }
    return {};
}

/// Finds the definition of a stored or typed language, as syntaxDefinition() describes.
///
/// @param language The language.
/// @param repository The definitions searched by file extension.
/// @param byKey Returns the definition whose lower-case name is the given key, or an invalid one.
/// @return The definition; invalid for plain text and unknown languages.
KSyntaxHighlighting::Definition
findDefinition(const QString &language, const KSyntaxHighlighting::Repository &repository,
               const std::function<KSyntaxHighlighting::Definition(const QString &)> &byKey)
{
    if (language.isEmpty()) {
        return {};
    }
    const QString key = language.toLower();
    if (KSyntaxHighlighting::Definition definition = byKey(key); definition.isValid()) {
        return definition;
    }
    if (const Offered *offered = offeredByName(key)) {
        return byKey(QString::fromLatin1(offered->definition).toLower());
    }
    // A bare name has no path, so only the extension patterns of the definitions match it.
    return repository.definitionForFileName(QStringLiteral("code.") + key);
}

/// Returns the name shown for a stored language, as languageName() describes, from its
/// definition.
QString nameOf(const QString &language, const KSyntaxHighlighting::Definition &definition)
{
    if (!definition.isValid()) {
        return language;
    }
    const QString name = shortName(definition.name().toLower());
    return name.isEmpty() ? definition.translatedName() : name;
}

} // namespace

KSyntaxHighlighting::Repository &syntaxRepository()
{
    static KSyntaxHighlighting::Repository repository;
    return repository;
}

QList<SyntaxLanguage> codeLanguages()
{
    QList<SyntaxLanguage> languages;
    for (const Offered &offered : OfferedLanguages) {
        const QString key = QString::fromLatin1(offered.definition).toLower();
        if (definitionsByKey().contains(key)) {
            languages.append({.key = key, .name = languageName(key)});
        }
    }
    std::ranges::sort(languages, [](const SyntaxLanguage &a, const SyntaxLanguage &b) {
        return a.name.localeAwareCompare(b.name) < 0;
    });
    return languages;
}

KSyntaxHighlighting::Definition syntaxDefinition(const QString &language)
{
    return findDefinition(language, syntaxRepository(),
                          [](const QString &key) { return definitionsByKey().value(key); });
}

KSyntaxHighlighting::Definition syntaxDefinition(const QString &language,
                                                 const KSyntaxHighlighting::Repository &repository)
{
    return findDefinition(language, repository, [&repository](const QString &key) {
        const QList<KSyntaxHighlighting::Definition> definitions = repository.definitions();
        const auto it =
            std::ranges::find_if(definitions, [&](const KSyntaxHighlighting::Definition &found) {
                return found.name().toLower() == key;
            });
        return it != definitions.end() ? *it : KSyntaxHighlighting::Definition();
    });
}

QString languageKey(const QString &language)
{
    const KSyntaxHighlighting::Definition definition = syntaxDefinition(language);
    return definition.isValid() ? definition.name().toLower() : language.toLower();
}

QString languageName(const QString &language)
{
    return nameOf(language, syntaxDefinition(language));
}

QString languageName(const QString &language, const KSyntaxHighlighting::Repository &repository)
{
    return nameOf(language, syntaxDefinition(language, repository));
}

} // namespace editor
