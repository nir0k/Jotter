#include "ui/Icons.h"
#include "ui/Palette.h"
#include "ui/Popups.h"
#include "ui/StyleSheet.h"
#include "ui/TagColor.h"

#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QTest>

/// Checks the interface look: stored keys, variant resolution, derived roles and the
/// generated style sheet, the glyphs of the icon picker and how stored icon names are drawn.
class PaletteTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    /// Stored palette keys survive a round trip; unknown keys are rejected so callers fall back
    /// to Warm.
    void paletteKeys()
    {
        for (const auto id : ui::allPalettes()) {
            QCOMPARE(ui::paletteFromKey(ui::paletteKey(id)), id);
        }
        QVERIFY(!ui::paletteFromKey(QStringLiteral("github-classic")).has_value());
    }

    /// GitHub Dimmed and Monokai have only a dark variant, so asking for light still gives dark
    /// roles.
    void singleVariantPalette()
    {
        QVERIFY(ui::roles(ui::PaletteId::GitHubDimmed, false).dark);
        QVERIFY(ui::roles(ui::PaletteId::Monokai, false).dark);
        QVERIFY(!ui::roles(ui::PaletteId::GitHub, false).dark);
    }

    /// Derived roles follow the reference mix: Warm light's primary container is 14% of the
    /// primary colour over white.
    void derivedRoles()
    {
        const QColor container = ui::roles(ui::PaletteId::Warm, false).primaryContainer;
        QVERIFY(qAbs(container.red() - 246) <= 1);
        QVERIFY(qAbs(container.green() - 228) <= 1);
        QVERIFY(qAbs(container.blue() - 221) <= 1);
    }

    /// Tag colour keys and custom `#rrggbb` keys resolve to their colour, anything else to none,
    /// and the text colour differs between light and dark so it stays readable on both.
    void tagColors()
    {
        for (const ui::TagColor color : ui::allTagColors()) {
            QCOMPARE(ui::colorFromKey(ui::tagColorKey(color)), ui::tagColorValue(color));
        }
        QCOMPARE(ui::allTagColors().size(), std::size_t{29});
        QCOMPARE(ui::colorFromKey(QStringLiteral("#Ff8800")), QColor(0xFF, 0x88, 0x00));
        for (const QString &key :
             {QStringLiteral("magenta"), QStringLiteral("#ff880"), QStringLiteral("#ff88000"),
              QStringLiteral("#ff88g0"), QStringLiteral("ff8800"), QString()}) {
            QVERIFY2(!ui::colorFromKey(key).has_value(), qPrintable(key));
        }
        const auto light = ui::roles(ui::PaletteId::Warm, false);
        const auto dark = ui::roles(ui::PaletteId::Warm, true);
        const QColor indigo = ui::tagColorValue(ui::TagColor::Indigo);
        QVERIFY(ui::tagTextColor(indigo, dark).lightness() >
                ui::tagTextColor(indigo, light).lightness());
    }

    /// Every placeholder of the style sheet template is replaced in every palette variant.
    void styleSheetComplete()
    {
        static const QRegularExpression placeholder(QStringLiteral(R"(\{\w+(\*[0-9.]+)?\})"));
        for (const auto id : ui::allPalettes()) {
            for (const bool dark : {false, true}) {
                const ui::Roles roles = ui::roles(id, dark);
                const QString sheet =
                    ui::buildStyleSheet(roles, ui::roles(id, true), 16, QStringLiteral("/tmp"));
                QVERIFY2(!placeholder.match(sheet).hasMatch(), qPrintable(ui::paletteKey(id)));
                QVERIFY(sheet.contains(roles.primary.name()));
            }
        }
    }

    /// Every glyph of the icon picker is offered once and has its drawing among the icon
    /// resources, so no choice shows an empty button.
    void pickerGlyphs()
    {
        const QStringList &glyphs = ui::IconPicker::glyphs();
        QCOMPARE(glyphs.size(), 120);
        QCOMPARE(QSet<QString>(glyphs.cbegin(), glyphs.cend()).size(), glyphs.size());
        for (const QString &name : glyphs) {
            QVERIFY2(QFile::exists(QStringLiteral(":/icons/%1.svg").arg(name)), qPrintable(name));
        }
    }

    /// A stored icon name is drawn only with Jotter's glyph of that name: a name the picker no
    /// longer offers and Jotter no longer has draws nothing, and a collection shows Folder
    /// instead. Glyphs the interface draws stay although the picker no longer offers them.
    void storedIconNames()
    {
        QTest::ignoreMessage(QtWarningMsg, "Unknown icon Office");
        QVERIFY(ui::icon(QStringLiteral("Office")).isNull());
        QVERIFY(!ui::icon(QStringLiteral("Favorite")).isNull());
        QVERIFY(!ui::icon(QStringLiteral("Settings")).isNull());

        QCOMPARE(ui::collectionGlyph(QStringLiteral("Calendar")), QStringLiteral("Calendar"));
        QCOMPARE(ui::collectionGlyph(QStringLiteral("Settings")), QStringLiteral("Settings"));
        QCOMPARE(ui::collectionGlyph(QStringLiteral("Bin")), QStringLiteral("Folder"));
        QCOMPARE(ui::collectionGlyph(QString()), QStringLiteral("Folder"));
    }
};

QTEST_GUILESS_MAIN(PaletteTest)
#include "tst_palette.moc"
