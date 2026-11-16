#include "core/Stemmer.h"

#include <libstemmer.h>

namespace core {

namespace {

/// Combining marks belong to the word they follow, so a decomposed letter stays one word.
bool isWordCharacter(char32_t c)
{
    if (QChar::isLetterOrNumber(c)) {
        return true;
    }
    const QChar::Category category = QChar::category(c);
    return category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining ||
           category == QChar::Mark_Enclosing;
}

/// Lowercases a word and drops its diacritics. The breve of `й` is kept, as Russian treats it
/// as a letter of its own; the diaeresis of `ё` goes, so it compares as `е`.
QString normalized(QStringView word)
{
    const QString decomposed = word.toString().toLower().normalized(QString::NormalizationForm_D);
    QString result;
    result.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.category() == QChar::Mark_NonSpacing) {
            if (c == u'̆' && result.endsWith(u'и')) {
                result += c;
            }
            continue;
        }
        result += c;
    }
    return result.normalized(QString::NormalizationForm_C);
}

/// Runs a Snowball stemmer over a normalised word; without a stemmer the word stays as it is.
QString stemWith(sb_stemmer *stemmer, const QString &word)
{
    if (stemmer == nullptr) {
        return word;
    }
    const QByteArray utf8 = word.toUtf8();
    const sb_symbol *stem =
        sb_stemmer_stem(stemmer, reinterpret_cast<const sb_symbol *>(utf8.constData()),
                        static_cast<int>(utf8.size()));
    if (stem == nullptr) {
        return word; // Out of memory: the unstemmed word still matches itself.
    }
    return QString::fromUtf8(reinterpret_cast<const char *>(stem), sb_stemmer_length(stemmer));
}

/// Joins Russian word forms that Snowball leaves apart: it keeps the fleeting vowel of forms
/// such as «заметок» and «отец», which the other forms lose («заметки», «отца»). An `о` or `е`
/// between a consonant and a final `к` or `ц` is dropped from every stem, so all forms meet.
QString withoutFleetingVowel(QString stem)
{
    static const QString consonants = QStringLiteral("бвгджзйклмнпрстфхцчшщ");
    const qsizetype n = stem.size();
    if (n >= 4 && (stem.at(n - 1) == u'\u043A' || stem.at(n - 1) == u'\u0446') &&
        (stem.at(n - 2) == u'\u043E' || stem.at(n - 2) == u'\u0435') &&
        consonants.contains(stem.at(n - 3))) {
        stem.remove(n - 2, 1);
    }
    return stem;
}

} // namespace

QList<TextSpan> splitWords(QStringView text)
{
    QList<TextSpan> words;
    qsizetype start = -1;
    qsizetype i = 0;
    while (i < text.size()) {
        char32_t c = text[i].unicode();
        qsizetype width = 1;
        if (QChar::isHighSurrogate(c) && i + 1 < text.size() && text[i + 1].isLowSurrogate()) {
            c = QChar::surrogateToUcs4(text[i], text[i + 1]);
            width = 2;
        }
        if (isWordCharacter(c)) {
            if (start < 0) {
                start = i;
            }
        } else if (start >= 0) {
            words.append({.start = start, .length = i - start});
            start = -1;
        }
        i += width;
    }
    if (start >= 0) {
        words.append({.start = start, .length = text.size() - start});
    }
    return words;
}

Stemmer::Stemmer()
    : m_russian(sb_stemmer_new("russian", "UTF_8"))
    , m_english(sb_stemmer_new("english", "UTF_8"))
{}

Stemmer::~Stemmer()
{
    sb_stemmer_delete(m_russian);
    sb_stemmer_delete(m_english);
}

QString Stemmer::stem(QStringView word)
{
    const QString plain = normalized(word);
    bool latin = false;
    for (const QChar c : plain) {
        if (c.script() == QChar::Script_Cyrillic) {
            return withoutFleetingVowel(stemWith(m_russian, plain));
        }
        latin = latin || c.script() == QChar::Script_Latin;
    }
    return latin ? stemWith(m_english, plain) : plain;
}

QStringList Stemmer::stems(QStringView text)
{
    QStringList result;
    for (const TextSpan &word : splitWords(text)) {
        result.append(stem(text.sliced(word.start, word.length)));
    }
    return result;
}

QString Stemmer::fingerprint()
{
    return stems(u"Заметками заметок ёлками йогурта notes running happily généralement").join(u' ');
}

} // namespace core
