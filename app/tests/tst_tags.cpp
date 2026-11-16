#include "core/Tags.h"

#include <QTest>

using namespace Qt::StringLiterals;

/// Checks the tag rules.
class TagsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    /// Typed tags lose a leading `#`, empty path parts and the spaces around parts.
    void normalizedTag_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("tag");
        QTest::newRow("plain") << QStringLiteral("work") << QStringLiteral("work");
        QTest::newRow("hash") << QStringLiteral("##work") << QStringLiteral("work");
        QTest::newRow("nested") << QStringLiteral(" work / meetings ")
                                << QStringLiteral("work/meetings");
        QTest::newRow("empty parts") << QStringLiteral("/a//b/") << QStringLiteral("a/b");
        QTest::newRow("spaces kept inside")
            << QStringLiteral("reading list") << QStringLiteral("reading list");
        QTest::newRow("nothing left") << QStringLiteral("# / ") << QString();
    }

    void normalizedTag()
    {
        QFETCH(QString, typed);
        QFETCH(QString, tag);
        QCOMPARE(core::normalizedTag(typed), tag);
    }

    /// Duplicates differing in case keep their first spelling.
    void mergeTags()
    {
        QCOMPARE(core::mergeTags({u"Home"_s}, {u"home"_s, u"work"_s}),
                 (QStringList{u"Home"_s, u"work"_s}));
    }
};

QTEST_GUILESS_MAIN(TagsTest)
#include "tst_tags.moc"
