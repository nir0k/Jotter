#pragma once

#include "core/Stemmer.h"

#include <QDateTime>
#include <QFlags>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>

namespace core {

/// Where the words of a query are looked for.
enum class SearchScope {
    Title = 0x1,
    Text = 0x2,
    Tags = 0x4,
    /// Collection names: a match finds the notes of the collection and its descendants.
    Collections = 0x8,
};
Q_DECLARE_FLAGS(SearchScopes, SearchScope)
Q_DECLARE_OPERATORS_FOR_FLAGS(SearchScopes)

/// Every scope, the default ("Everywhere").
inline constexpr SearchScopes AllScopes = SearchScopes(0xF);

/// One part of a query.
struct SearchTerm
{
    enum class Kind {
        /// A bare word; punctuation inside it (`e-mail`) makes it a short phrase.
        Word,
        /// Words in double quotes, matched in their order.
        Phrase,
        /// `#tag`: a filter by the tag and its children, not a word of the text.
        Tag,
    };

    Kind kind = Kind::Word;
    /// The word or phrase as typed, or the tag in its stored form.
    QString text;
    /// The last word still being typed matches as a prefix.
    bool prefix = false;
    /// `-word`: notes that match are left out.
    bool exclude = false;

    bool operator==(const SearchTerm &) const = default;
};

/// Splits a query into terms: words must all match, `"exact phrase"`, `-word`
/// excludes, `#tag` filters by a tag. The last word or an unclosed phrase matches as a prefix
/// unless the query ends with a space. Terms without a single letter or digit are dropped.
///
/// @param query The query as typed.
/// @return The terms in their order.
QList<SearchTerm> parseQuery(QStringView query);

/// A search of the note list: the query, its scopes and the filters.
struct Search
{
    QString query;
    SearchScopes scopes = AllScopes;
    /// Creation and update time ranges, from inclusive to until exclusive.
    std::optional<QDateTime> createdFrom;
    std::optional<QDateTime> createdUntil;
    std::optional<QDateTime> updatedFrom;
    std::optional<QDateTime> updatedUntil;
    /// Notes in any of these collections or their descendants.
    QStringList collections;
    /// Notes with any of these tags or their children.
    QStringList tags;
    bool withAttachments = false;
    bool withLocation = false;
    /// Archived notes are found too, besides the notes of the view.
    bool includeArchived = false;

    /// Tells whether any filter narrows the notes, regardless of the query.
    bool hasFilters() const;

    /// Tells whether the search narrows the notes at all: a query with terms or a filter.
    bool isActive() const;
};

/// Finds the words of positive Word and Phrase terms in a text, in every word form, as the
/// index matches them: for the highlights in the editor and for collection names.
class TermMatcher
{
public:
    /// @param terms The query's terms; Tag terms and excluded terms are ignored.
    explicit TermMatcher(const QList<SearchTerm> &terms);
    ~TermMatcher();
    TermMatcher(const TermMatcher &) = delete;
    TermMatcher &operator=(const TermMatcher &) = delete;

    /// Tells whether there is nothing to find.
    bool isEmpty() const { return m_patterns.isEmpty(); }

    /// Finds the matches in @p text: each matching word, and the words of a phrase only where
    /// they follow each other.
    ///
    /// @param text The text, such as one paragraph.
    /// @return The matching words in their order.
    QList<TextSpan> find(QStringView text) const;

private:
    /// The stems of one term; with prefix, the last stem matches the start of a word.
    struct Pattern
    {
        QStringList stems;
        bool prefix = false;
    };

    std::unique_ptr<Stemmer> m_stemmer;
    QList<Pattern> m_patterns;
};

} // namespace core
