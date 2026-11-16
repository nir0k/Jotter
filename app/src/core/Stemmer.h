#pragma once

#include <QList>
#include <QString>
#include <QStringView>

struct sb_stemmer;

namespace core {

/// A part of a text, in UTF-16 code units.
struct TextSpan
{
    qsizetype start = 0;
    qsizetype length = 0;

    bool operator==(const TextSpan &) const = default;
};

/// Splits @p text into words: runs of letters, digits and combining marks. Everything else
/// (spaces, punctuation, symbols) separates words, so `e-mail` is two words.
///
/// @param text The text.
/// @return The words in their order.
QList<TextSpan> splitWords(QStringView text);

/// Reduces words to the terms search compares: case and diacritics are
/// dropped, `ё` becomes `е` while `й` stays a letter of its own, and Russian and English words
/// are cut to their stems by the Snowball stemmers of libstemmer, chosen by the word's script.
///
/// The index, the query and the highlighting in the editor use the same reduction, so they
/// agree on every word form. A stemmer is not thread-safe: each thread uses its own.
class Stemmer
{
public:
    Stemmer();
    ~Stemmer();
    Stemmer(const Stemmer &) = delete;
    Stemmer &operator=(const Stemmer &) = delete;

    /// Returns the search term of one word, as splitWords() finds it.
    ///
    /// @param word The word.
    /// @return The normalised stem; a word in another script is only normalised.
    QString stem(QStringView word);

    /// Returns the terms of every word of @p text in their order.
    QStringList stems(QStringView text);

    /// Describes the stemmers' behaviour on sample words. An index built with other stems
    /// cannot remove its old entries correctly, so it is rebuilt when this changes (a new
    /// libstemmer release may change the algorithms).
    QString fingerprint();

private:
    sb_stemmer *m_russian;
    sb_stemmer *m_english;
};

} // namespace core
