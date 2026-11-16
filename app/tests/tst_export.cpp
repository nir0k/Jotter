#include "core/Attachments.h"
#include "core/Export.h"
#include "core/Files.h"
#include "core/Html.h"
#include "core/Library.h"
#include "core/NoteLinks.h"
#include "editor/Export.h"
#include "ui/Theme.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <QtConcurrentRun>

#include <KZip>

#include <optional>

using namespace Qt::StringLiterals;

/// Checks exports: self-contained HTML with its attachments and note links,
/// limited to the note vocabulary, the body HTML scanner it uses, unique file names, archives of
/// several files and JPG parts of a tall note.
class ExportTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        m_theme.emplace();
        m_theme->apply();
    }

    void init()
    {
        m_dir.emplace();
        auto library = core::Library::create(m_dir->path());
        QVERIFY(library);
        m_library.emplace(*std::move(library));
    }

    void cleanup()
    {
        m_library.reset();
        m_dir.reset();
    }

    /// HTML embeds images, puts attachment files in `<name>_files/`, points note links to the
    /// files of the same export, makes other note links plain text and holds no scripts.
    void htmlIsSelfContained()
    {
        core::Note other = addNote(u"Budget"_s, u"<p>Numbers</p>"_s);
        core::Note note = core::makeNote();
        note.meta.title = u"Trip"_s;
        const QString dir = m_library->attachmentsDir(note.meta.id);
        QImage pixel(1, 1, QImage::Format_RGB32);
        pixel.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(pixel.save(&buffer, "PNG"));
        QVERIFY(core::storeData(dir, u"pic.png"_s, png));
        QVERIFY(core::storeData(dir, u"plan #1.txt"_s, "plan"));
        note.body = u"<p><img src=\"%1\"></p>"
                    "<p><a class=\"attachment\" href=\"%2\" data-size=\"4\">plan #1.txt</a></p>"
                    "<p><a class=\"note-link\" href=\"%3\">Budget</a> and "
                    "<a class=\"note-link\" href=\"%4\">Gone</a></p>"_s.arg(
                        core::attachmentSource(note.meta.id, u"pic.png"_s),
                        core::attachmentSource(note.meta.id, u"plan #1.txt"_s),
                        core::noteLinkAddress(other.meta.id), core::noteLinkAddress(core::newId()));
        QVERIFY(m_library->writeNote(note));

        const QList<core::ExportFile> files = core::exportHtml(
            *m_library, note, u"Trip"_s, {{note.meta.id, u"Trip"_s}, {other.meta.id, u"Budget"_s}},
            u"Updated today"_s);
        QCOMPARE(files.size(), 2);
        QCOMPARE(files.at(0).path, u"Trip.html"_s);
        QCOMPARE(files.at(1).path, u"Trip_files/plan #1.txt"_s);
        QCOMPARE(files.at(1).source, dir + u"/plan #1.txt"_s);
        const QString html = QString::fromUtf8(files.at(0).data);
        QVERIFY(html.contains(u"src=\"data:image/png;base64,"_s));
        QVERIFY(html.contains(u"href=\"Trip_files/plan%20%231.txt\""_s));
        QVERIFY(html.contains(u"<a class=\"note-link\" href=\"Budget.html\">Budget</a>"_s));
        QVERIFY(html.contains(u"and Gone</p>"_s));
        QVERIFY(html.contains(u"Updated today"_s));
        QVERIFY(!html.contains(u"<script"_s));
        QVERIFY(!html.contains(u"../attachments"_s));
    }

    /// The scanner decodes references in text and attributes and skips comments, and a stray
    /// `<` stays text.
    void tokenize()
    {
        const auto tokens = core::html::tokenize(
            u"<!-- c --><a href=\"x?a=1&amp;b=2\" data-x='y'>a &lt; b &#x2014; c</a> 1 < 2");
        QCOMPARE(tokens.size(), 4);
        QCOMPARE(tokens.at(0).name, u"a"_s);
        QCOMPARE(tokens.at(0).attribute(u"href"), u"x?a=1&b=2"_s);
        QCOMPARE(tokens.at(0).attribute(u"data-x"), u"y"_s);
        QCOMPARE(tokens.at(1).text, u"a < b — c"_s);
        QCOMPARE(tokens.at(2).kind, core::html::Token::Kind::End);
        QCOMPARE(tokens.at(3).text, u" 1 < 2"_s);
    }

    /// A note file changed outside Jotter brings no scripts, event handlers, remote images or
    /// links with other schemes into an HTML export; unknown elements keep their text.
    void htmlKeepsOnlyTheVocabulary()
    {
        core::Note note = core::makeNote();
        note.body = u"<p onclick=\"x()\">Hi<script>alert(1)</script><style>p{}</style> "
                    "<img src=\"https://example.com/t.png\" onerror=\"x()\"> "
                    "<a href=\"javascript:x()\">bad</a> <a href=\"file:///etc/passwd\">file</a> "
                    "<a href=\"https://example.com/\">web</a> <blink>old</blink> "
                    "<span style=\"background: url(https://example.com/t)\">styled</span></p>"_s;
        const QList<core::ExportFile> files =
            core::exportHtml(*m_library, note, u"Note"_s, {{note.meta.id, u"Note"_s}}, {});
        const QString html = QString::fromUtf8(files.at(0).data);
        const qsizetype start = html.indexOf(u"<p>Hi"_s);
        QVERIFY(start >= 0);
        QCOMPARE(html.mid(start, html.indexOf(u"</p>"_s, start) + 4 - start),
                 u"<p>Hi  bad file <a href=\"https://example.com/\">web</a> old "
                 "<span>styled</span></p>"_s);
    }

    /// Several notes are packed into one archive, one file per note named after its title, with
    /// repeated and unusable names made unique and safe.
    void namesInArchiveAreUnique()
    {
        const QStringList ids{addNote(u"Plan"_s).meta.id, addNote(u"plan"_s).meta.id,
                              addNote(u"a/b: c?"_s).meta.id, addNote(u" . "_s).meta.id};
        const auto result = run(ids, editor::ExportFormat::Pdf);
        QVERIFY(result);
        QCOMPARE(result->fileName, u"Notes.zip"_s);
        QStringList names = archiveEntries(result->temporaryPath);
        names.sort();
        QCOMPARE(names, (QStringList{u"Plan.pdf"_s, u"Untitled.pdf"_s, u"a_b_ c_.pdf"_s,
                                     u"plan (2).pdf"_s}));
        QFile::remove(result->temporaryPath);
    }

    /// One note exported as PDF is a single file on the chosen page size, saved where the person
    /// chose without a temporary file left behind.
    void singlePdf()
    {
        const auto result = run({addNote(u"Letter"_s, u"<p>Text</p>"_s).meta.id},
                                editor::ExportFormat::Pdf, QPageSize::Letter);
        QVERIFY(result);
        QCOMPARE(result->fileName, u"Letter.pdf"_s);
        const QString target = m_dir->filePath(u"Letter.pdf"_s);
        QVERIFY(editor::saveExport(result->temporaryPath, target));
        QVERIFY(!QFile::exists(result->temporaryPath));
        QFile file(target);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray pdf = file.readAll();
        QVERIFY(pdf.startsWith("%PDF"));
        QVERIFY(pdf.contains("/MediaBox [0 0 612.000000 792.000000]"));
    }

    /// A note taller than 16,000 pixels at twice its size is split into numbered JPG parts,
    /// which makes the export an archive.
    void tallJpgIsSplit()
    {
        QString body;
        for (int i = 0; i < 600; ++i) {
            body += u"<p>Line %1</p>"_s.arg(i);
        }
        const auto result = run({addNote(u"Long"_s, body).meta.id}, editor::ExportFormat::Jpg);
        QVERIFY(result);
        QCOMPARE(result->fileName, u"Long.zip"_s);
        KZip zip(result->temporaryPath);
        QVERIFY(zip.open(QIODevice::ReadOnly));
        const QStringList names = zip.directory()->entries();
        QVERIFY(names.size() >= 2);
        for (int part = 1; part <= names.size(); ++part) {
            const auto *file = zip.directory()->file(u"Long-%1.jpg"_s.arg(part));
            QVERIFY(file != nullptr);
            const QImage image = QImage::fromData(file->data(), "JPG");
            QCOMPARE(image.width(), 1520 + (4 * m_theme->px(2.0)));
            QVERIFY(image.height() <= 16000);
        }
        QFile::remove(result->temporaryPath);
    }

    /// A cancelled export reports no result and leaves no temporary file behind, also when it
    /// was writing an archive.
    void cancelLeavesNothing()
    {
        QStringList ids;
        for (int i = 0; i < 40; ++i) {
            ids.append(addNote(u"Note %1"_s.arg(i), u"<p>Text</p>"_s).meta.id);
        }
        const QDir temp = QDir::temp();
        const QStringList pattern{u"jotter-export-*"_s};
        const QStringList before = temp.entryList(pattern, QDir::Files);
        const editor::ExportRequest request{.library = *m_library,
                                            .noteIds = ids,
                                            .format = editor::ExportFormat::Pdf,
                                            .pageSize = QPageSize::A4};
        QFuture<core::Result<editor::ExportResult>> future =
            QtConcurrent::run(editor::exportNotes, request);
        QTest::qWait(20);
        future.cancel();
        future.waitForFinished();
        QStringList left = temp.entryList(pattern, QDir::Files);
        if (future.resultCount() > 0 && future.result()) {
            // It finished before the cancellation; its export is the person's to save.
            left.removeAll(QFileInfo(future.result()->temporaryPath).fileName());
            QFile::remove(future.result()->temporaryPath);
        }
        QCOMPARE(left, before);
    }

private:
    /// Saves a note with a title and a body to the library.
    core::Note addNote(const QString &title, const QString &body = {})
    {
        core::Note note = core::makeNote();
        note.meta.title = title;
        note.body = body;
        [&] { QVERIFY(m_library->writeNote(note)); }();
        return note;
    }

    /// Runs an export on a worker thread and waits for it.
    std::optional<editor::ExportResult> run(const QStringList &ids, editor::ExportFormat format,
                                            QPageSize::PageSizeId pageSize = QPageSize::A4)
    {
        const editor::ExportRequest request{
            .library = *m_library, .noteIds = ids, .format = format, .pageSize = pageSize};
        QFuture<core::Result<editor::ExportResult>> future =
            QtConcurrent::run(editor::exportNotes, request);
        future.waitForFinished();
        if (future.resultCount() == 0 || !future.result()) {
            return std::nullopt;
        }
        return *future.result();
    }

    /// Returns the names of an archive's files in their order.
    static QStringList archiveEntries(const QString &path)
    {
        KZip zip(path);
        if (!zip.open(QIODevice::ReadOnly)) {
            return {};
        }
        QStringList names;
        for (const QString &name : zip.directory()->entries()) {
            names.append(name);
        }
        return names;
    }

    std::optional<ui::Theme> m_theme;
    std::optional<QTemporaryDir> m_dir;
    std::optional<core::Library> m_library;
};

QTEST_MAIN(ExportTest)
#include "tst_export.moc"
