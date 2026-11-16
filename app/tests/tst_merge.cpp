#include "core/Merge.h"

#include <QTest>

using namespace Qt::StringLiterals;

/// Checks how note bodies split into the blocks of the three-way merge; the
/// merges themselves are checked with the shared fixtures in `tests/fixtures/merge/`.
class MergeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    /// Top-level blocks keep nested elements, void elements and text outside elements apart,
    /// and a `>` inside an attribute does not end a tag.
    void bodyBlocks()
    {
        const QString body = u"<p>One</p>\n<ul><li>a<ul><li>b</li></ul></li></ul>\n<hr>\n"
                             u"loose text\n<p><a href=\"x?a>b\">link</a><img src=\"i.png\"></p>"_s;
        QCOMPARE(core::bodyBlocks(body),
                 (QStringList{u"<p>One</p>"_s, u"<ul><li>a<ul><li>b</li></ul></li></ul>"_s,
                              u"<hr>"_s, u"loose text"_s,
                              u"<p><a href=\"x?a>b\">link</a><img src=\"i.png\"></p>"_s}));
    }
};

QTEST_GUILESS_MAIN(MergeTest)
#include "tst_merge.moc"
