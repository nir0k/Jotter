#include "core/Numbering.h"

#include "core/Html.h"

#include <array>
#include <set>

namespace core {

namespace {

constexpr std::array<NumberingStyle, 4> Styles{{
    {.id = DefaultNumberingStyle, .counter = Counter::Decimal, .includeParents = false},
    {.id = u"upper-alpha", .counter = Counter::UpperLatin, .includeParents = false},
    {.id = u"upper-roman", .counter = Counter::UpperRoman, .includeParents = false},
    {.id = u"multilevel", .counter = Counter::Decimal, .includeParents = true},
}};

/// Returns the CSS counter style that draws a counter.
QLatin1StringView cssCounter(Counter counter)
{
    switch (counter) {
    case Counter::Decimal:
        break;
    case Counter::UpperLatin:
        return QLatin1StringView("upper-latin");
    case Counter::UpperRoman:
        return QLatin1StringView("upper-roman");
    }
    return QLatin1StringView("decimal");
}

/// Writes a number in bijective base 26: 1 is `A`, one past `Z` is `AA`.
QString alphabetic(int value)
{
    QString text;
    while (value > 0) {
        --value;
        text.prepend(QChar(u'A' + (value % 26)));
        value /= 26;
    }
    return text;
}

QString roman(int value)
{
    static constexpr std::array<std::pair<int, const char *>, 13> Numerals{{
        {1000, "M"},
        {900, "CM"},
        {500, "D"},
        {400, "CD"},
        {100, "C"},
        {90, "XC"},
        {50, "L"},
        {40, "XL"},
        {10, "X"},
        {9, "IX"},
        {5, "V"},
        {4, "IV"},
        {1, "I"},
    }};
    QString text;
    for (const auto &[amount, numeral] : Numerals) {
        while (value >= amount) {
            text += QLatin1StringView(numeral);
            value -= amount;
        }
    }
    return text;
}

/// Returns a selector for lists at exactly one nesting level: lists sit in the body or in a
/// table cell, and nested lists in an item of the list above.
QString levelSelector(int depth, const QString &list)
{
    QString selector = QStringLiteral(":is(.jotter-body,td,th)>");
    for (int i = 1; i < depth; ++i) {
        selector += QStringLiteral(":is(ul,ol)>li>");
    }
    return selector + list;
}

} // namespace

std::span<const NumberingStyle> numberingStyles()
{
    return Styles;
}

const NumberingStyle *findNumberingStyle(QStringView id)
{
    const auto *const found = std::ranges::find(Styles, id, &NumberingStyle::id);
    return found != Styles.end() ? found : nullptr;
}

QString formatCounter(Counter counter, int value)
{
    if (value < 1) {
        return QString::number(value);
    }
    switch (counter) {
    case Counter::Decimal:
        break;
    case Counter::UpperLatin:
        return alphabetic(value);
    case Counter::UpperRoman:
        return value <= 3999 ? roman(value) : QString::number(value);
    }
    return QString::number(value);
}

QString numberingMarker(const NumberingStyle &style, const QList<int> &numbers)
{
    if (numbers.isEmpty()) {
        return {};
    }
    QString marker;
    if (style.includeParents) {
        for (qsizetype i = 0; i + 1 < numbers.size(); ++i) {
            marker += formatCounter(style.counter, numbers.at(i)) + u'.';
        }
    }
    return marker + formatCounter(style.counter, numbers.last()) + u'.';
}

QString numberingStyleSheet(QStringView body)
{
    std::set<std::pair<const NumberingStyle *, int>> styled; // style, depth
    std::set<std::pair<int, int>> starts;                    // depth, start
    int depth = 0;
    int deepest = 0;
    for (const html::Token &token : html::tokenize(body)) {
        const bool list =
            token.name == QLatin1StringView("ul") || token.name == QLatin1StringView("ol");
        if (!list) {
            continue;
        }
        if (token.kind == html::Token::Kind::End) {
            depth = std::max(0, depth - 1);
            continue;
        }
        if (token.kind != html::Token::Kind::Start) {
            continue;
        }
        ++depth;
        if (token.name != QLatin1StringView("ol")) {
            continue;
        }
        const int start = token.attribute(u"start").toInt();
        if (start > 1) {
            starts.insert({depth, start});
        }
        const NumberingStyle *style = findNumberingStyle(token.attribute(u"data-style"));
        if (style == nullptr || style->id == DefaultNumberingStyle || depth > MaxListLevels) {
            continue;
        }
        styled.insert({style, depth});
        deepest = std::max(deepest, depth);
    }
    if (styled.empty()) {
        return {};
    }

    // Every list level keeps a counter of its own, so a marker can include the numbers of the
    // items above it.
    QString css;
    for (int d = 1; d <= deepest; ++d) {
        const QString list = levelSelector(d, QStringLiteral(":is(ul,ol)"));
        css += QStringLiteral("%1{counter-reset:jotter-%2}%1>li{counter-increment:jotter-%2}")
                   .arg(list)
                   .arg(d);
    }
    for (const auto &[d, start] : starts) {
        if (d <= deepest) {
            css += QStringLiteral("%1{counter-reset:jotter-%2 %3}")
                       .arg(levelSelector(d, QStringLiteral("ol[start=\"%1\"]").arg(start)))
                       .arg(d)
                       .arg(start - 1);
        }
    }
    for (const auto &[style, d] : styled) {
        const auto counter = [&](int level) {
            return QStringLiteral("counter(jotter-%1,%2)")
                .arg(level)
                .arg(cssCounter(style->counter));
        };
        QStringList parts;
        if (style->includeParents) {
            for (int i = 1; i < d; ++i) {
                parts << counter(i) << QStringLiteral("\".\"");
            }
        }
        parts << counter(d) << QStringLiteral("\". \"");
        css += QStringLiteral("%1>li::marker{content:%2}")
                   .arg(levelSelector(d, QStringLiteral("ol[data-style=\"%1\"]").arg(style->id)),
                        parts.join(u' '));
    }
    return css;
}

} // namespace core
