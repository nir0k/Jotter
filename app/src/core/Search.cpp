#include "core/Search.h"

#include "core/Tags.h"

#include <algorithm>

namespace core {

QList<SearchTerm> parseQuery(QStringView query)
{
    QList<SearchTerm> terms;
    const qsizetype size = query.size();
    qsizetype i = 0;
    while (i < size) {
        if (query[i].isSpace()) {
            ++i;
            continue;
        }
        SearchTerm term;
        if (query[i] == u'-' && i + 1 < size && !query[i + 1].isSpace()) {
            term.exclude = true;
            ++i;
        }
        bool open = true;
        if (query[i] == u'"') {
            const qsizetype end = query.indexOf(u'"', i + 1);
            term.kind = SearchTerm::Kind::Phrase;
            term.text = query.sliced(i + 1, (end < 0 ? size : end) - i - 1).toString();
            open = end < 0;
            i = end < 0 ? size : end + 1;
        } else {
            qsizetype end = i;
            while (end < size && !query[end].isSpace() && query[end] != u'"') {
                ++end;
            }
            const QStringView word = query.sliced(i, end - i);
            i = end;
            if (word.startsWith(u'#')) {
                term.kind = SearchTerm::Kind::Tag;
                term.text = normalizedTag(word);
            } else {
                term.text = word.toString();
            }
        }
        // Only a term that ends the query is still being typed.
        term.prefix = open && i >= size;
        const bool meaningful = term.kind == SearchTerm::Kind::Tag
                                    ? !term.text.isEmpty()
                                    : !splitWords(term.text).isEmpty();
        if (meaningful) {
            terms.append(term);
        }
    }
    return terms;
}

bool Search::hasFilters() const
{
    return createdFrom || createdUntil || updatedFrom || updatedUntil || !collections.isEmpty() ||
           !tags.isEmpty() || withAttachments || withLocation || includeArchived;
}

bool Search::isActive() const
{
    return hasFilters() || !parseQuery(query).isEmpty();
}

TermMatcher::TermMatcher(const QList<SearchTerm> &terms)
    : m_stemmer(std::make_unique<Stemmer>())
{
    for (const SearchTerm &term : terms) {
        if (term.exclude || term.kind == SearchTerm::Kind::Tag) {
            continue;
        }
        QStringList stems = m_stemmer->stems(term.text);
        if (!stems.isEmpty()) {
            m_patterns.append({.stems = std::move(stems), .prefix = term.prefix});
        }
    }
}

TermMatcher::~TermMatcher() = default;

QList<TextSpan> TermMatcher::find(QStringView text) const
{
    if (m_patterns.isEmpty()) {
        return {};
    }
    const QList<TextSpan> words = splitWords(text);
    QStringList stems;
    stems.reserve(words.size());
    for (const TextSpan &word : words) {
        stems.append(m_stemmer->stem(text.sliced(word.start, word.length)));
    }
    QList<bool> matched(words.size(), false);
    for (const Pattern &pattern : m_patterns) {
        const qsizetype length = pattern.stems.size();
        for (qsizetype at = 0; at + length <= stems.size(); ++at) {
            bool same = true;
            for (qsizetype k = 0; k < length && same; ++k) {
                const QString &stem = stems.at(at + k);
                const QString &wanted = pattern.stems.at(k);
                same =
                    (pattern.prefix && k == length - 1) ? stem.startsWith(wanted) : stem == wanted;
            }
            if (same) {
                std::fill_n(matched.begin() + at, length, true);
            }
        }
    }
    QList<TextSpan> spans;
    for (qsizetype i = 0; i < words.size(); ++i) {
        if (matched.at(i)) {
            spans.append(words.at(i));
        }
    }
    return spans;
}

} // namespace core
