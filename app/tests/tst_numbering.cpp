#include "core/Numbering.h"

#include <QTest>

using namespace Qt::StringLiterals;

/// Checks numbering styles: the counters, the markers of every level and the
/// style sheet that makes a browser draw the same numbers.
class NumberingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    /// Letters continue with two letters after the last one; Roman numerals fall back to
    /// decimal past their range, as CSS does.
    void counters()
    {
        using core::Counter;
        QCOMPARE(core::formatCounter(Counter::UpperLatin, 26), u"Z"_s);
        QCOMPARE(core::formatCounter(Counter::UpperLatin, 28), u"AB"_s);
        QCOMPARE(core::formatCounter(Counter::UpperRoman, 1994), u"MCMXCIV"_s);
        QCOMPARE(core::formatCounter(Counter::UpperRoman, 4000), u"4000"_s);
    }

    /// Single-level styles repeat at every level; Multilevel includes the numbers above; a
    /// removed or custom style is unknown and drawn as `1.`.
    void markers()
    {
        const core::NumberingStyle &alpha = *core::findNumberingStyle(u"upper-alpha");
        QCOMPARE(core::numberingMarker(alpha, {2, 3}), u"C."_s);
        const core::NumberingStyle &multilevel = *core::findNumberingStyle(u"multilevel");
        QCOMPARE(core::numberingMarker(multilevel, {1, 2, 3}), u"1.2.3."_s);
        for (const auto *const id :
             {u"outline", u"lower-alpha", u"0192f3a4-5b6c-7d8e-9f01-23456789abcd"}) {
            QVERIFY(core::findNumberingStyle(id) == nullptr);
        }
        QCOMPARE(core::numberingStyles().size(), std::size_t{4});
    }

    /// The style sheet counts every list level, honours `start` and gives each styled level its
    /// marker; a body with plain lists or unknown styles only needs no rules.
    void styleSheet()
    {
        QVERIFY(core::numberingStyleSheet(u"<ol start=\"3\">\n<li>a</li>\n</ol>").isEmpty());
        QVERIFY(
            core::numberingStyleSheet(u"<ol data-style=\"outline\">\n<li>a</li>\n</ol>").isEmpty());
        const QString css = core::numberingStyleSheet(
            u"<ul>\n<li>a\n<ol data-style=\"multilevel\" start=\"4\">\n<li>b</li>\n</ol>\n</li>\n"
            u"</ul>");
        QVERIFY(css.contains(u":is(.jotter-body,td,th)>:is(ul,ol){counter-reset:jotter-1}"));
        QVERIFY(css.contains(u":is(.jotter-body,td,th)>:is(ul,ol)>li>ol[start=\"4\"]"
                             u"{counter-reset:jotter-2 3}"));
        QVERIFY(css.contains(u">ol[data-style=\"multilevel\"]>li::marker{content:"
                             u"counter(jotter-1,decimal) \".\" counter(jotter-2,decimal) \". \"}"));
    }
};

QTEST_GUILESS_MAIN(NumberingTest)
#include "tst_numbering.moc"
