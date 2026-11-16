#include "core/Attachments.h"
#include "core/Directory.h"
#include "core/Files.h"
#include "core/Index.h"
#include "core/Note.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

using namespace Qt::StringLiterals;

/// Checks attachment storage: image kinds from bytes, stored copies, names and
/// addresses, and the grace period before files no note refers to are removed.
class AttachmentsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    /// Images are recognised by their bytes; SVG and other files are attachments.
    void imageKinds()
    {
        QCOMPARE(core::imageKind("\x89PNG\r\n\x1a\nrest"), core::ImageKind::Png);
        QCOMPARE(core::imageKind("\xff\xd8\xff\xe0"), core::ImageKind::Jpeg);
        QCOMPARE(core::imageKind("GIF89a.."), core::ImageKind::Gif);
        // The size field after RIFF holds zero bytes, so the views are given their length.
        QCOMPARE(core::imageKind(QByteArrayView("RIFF\x10\0\0\0WEBPVP8 ", 16)),
                 core::ImageKind::WebP);
        QCOMPARE(core::imageKind(QByteArrayView("RIFF\x10\0\0\0WAVEfmt ", 16)),
                 core::ImageKind::None);
        QCOMPARE(core::imageKind("<svg xmlns="), core::ImageKind::None);
    }

    /// A stored file keeps its bytes and name, gets a suffix when the name is taken and loses
    /// executable permission bits; a file posing as an image by name stays an attachment, or is
    /// refused where only an image is accepted.
    void storing()
    {
        QTemporaryDir dir;
        const QString source = dir.filePath(u"photo.jpg"_s);
        {
            QFile file(source);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("not really a jpeg");
            QVERIFY(file.setPermissions(file.permissions() | QFileDevice::ExeOwner));
        }
        const QString folder = dir.filePath(u"attachments/note"_s);
        auto first = core::storeFile(folder, source, core::StoreAs::Image);
        QVERIFY(first);
        QCOMPARE(first->fileName, u"photo.jpg"_s);
        QCOMPARE(first->kind, core::ImageKind::None);
        auto second = core::storeFile(folder, source, core::StoreAs::File);
        QVERIFY(second);
        QCOMPARE(second->fileName, u"photo (2).jpg"_s);
        const QFile stored(folder + u"/photo (2).jpg"_s);
        QVERIFY(!(stored.permissions() & (QFileDevice::ExeOwner | QFileDevice::ExeUser |
                                          QFileDevice::ExeGroup | QFileDevice::ExeOther)));
        QCOMPARE(*core::readFile(stored.fileName()), QByteArray("not really a jpeg"));

        // A file named by pasted HTML must be an image, and a folder is never copied.
        const auto refused = core::storeFile(folder, source, core::StoreAs::ImageOnly);
        QVERIFY(!refused);
        QCOMPARE(refused.error().code, core::ErrorCode::Refused);
        const auto folderCopy = core::storeFile(folder, dir.path(), core::StoreAs::File);
        QVERIFY(!folderCopy);
        QCOMPARE(folderCopy.error().code, core::ErrorCode::Refused);
        QCOMPARE(QDir(folder).entryList(QDir::Files).size(), 2);

        auto pasted = core::storeData(folder, u"a/b.png"_s, "\x89PNG\r\n\x1a\n");
        QVERIFY(pasted);
        QCOMPARE(pasted->fileName, u"a_b.png"_s);
        QCOMPARE(pasted->kind, core::ImageKind::Png);
    }

    /// An image with more pixels than Qt decodes, though small in bytes, is stored as an
    /// attachment, or refused where only an image is accepted; a file over the size limit is
    /// refused.
    void limits()
    {
        QTemporaryDir dir;
        const QString folder = dir.filePath(u"attachments/note"_s);
        // The PNG signature and IHDR chunk header, then big-endian width and height.
        const auto png = [](quint32 width, quint32 height) {
            QByteArray head("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR", 16);
            head.resize(24);
            qToBigEndian(width, head.data() + 16);
            qToBigEndian(height, head.data() + 20);
            return head;
        };
        QCOMPARE(core::storeData(folder, u"a.png"_s, png(8192, 8192))->kind, core::ImageKind::Png);
        QCOMPARE(core::storeData(folder, u"b.png"_s, png(8193, 8192))->kind, core::ImageKind::None);
        // A lossless WebP of 16384 × 16384: the dimensions minus one, 14 bits each.
        const QByteArray webp =
            QByteArray("RIFF\0\0\0\0WEBPVP8L\0\0\0\0\x2f\xff\xff\xff\x0f", 25) + QByteArray(8, 0);
        QCOMPARE(core::storeData(folder, u"c.webp"_s, webp)->kind, core::ImageKind::None);

        const QString source = dir.filePath(u"large.png"_s);
        {
            QFile file(source);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(png(9000, 9000));
        }
        QCOMPARE(core::storeFile(folder, source, core::StoreAs::Image)->kind,
                 core::ImageKind::None);
        QCOMPARE(core::storeFile(folder, source, core::StoreAs::ImageOnly).error().code,
                 core::ErrorCode::Refused);
        {
            QFile file(source);
            QVERIFY(file.resize(core::MaxFileBytes + 1));
        }
        QCOMPARE(core::storeFile(folder, source, core::StoreAs::File).error().code,
                 core::ErrorCode::TooLarge);
    }

    /// Addresses encode what would break a URL and nothing else; other addresses are not
    /// attachments.
    void sources()
    {
        const QString source = core::attachmentSource(u"n1"_s, u"отчёт #2 100%.pdf"_s);
        QCOMPARE(source, u"../attachments/n1/отчёт%20%232%20100%25.pdf"_s);
        const auto parsed = core::parseAttachmentSource(source);
        QVERIFY(parsed);
        QCOMPARE(parsed->noteId, u"n1"_s);
        QCOMPARE(parsed->fileName, u"отчёт #2 100%.pdf"_s);
        QVERIFY(!core::parseAttachmentSource(u"https://example.com/a.png"));
        QVERIFY(!core::parseAttachmentSource(u"../attachments/n1/..%2F..%2Fx"));
        const core::BodyReferences references = core::bodyReferences(
            u"<p><img src=\"../attachments/n1/a.png\" data-width=\"50\"> <a class=\"attachment\" "
            u"href=\"../attachments/n1/b.zip\" data-size=\"3\">b.zip</a></p>");
        QCOMPARE(references.attachments.size(), 2);
        QVERIFY(references.attachments.at(0).image);
        QCOMPARE(references.attachments.at(1).fileName, u"b.zip"_s);
    }

    /// A file no note refers to is kept for the grace period and then removed; a reference
    /// that comes back within it keeps the file.
    void orphans()
    {
        QTemporaryDir dir;
        auto library = core::Library::create(dir.filePath(u"lib"_s));
        QVERIFY(library);
        auto index = core::Index::open(dir.filePath(u"index.sqlite"_s));
        QVERIFY(index);
        core::Note note = core::makeNote();
        const QString folder = library->attachmentsDir(note.meta.id);
        QVERIFY(core::storeData(folder, u"kept.txt"_s, "a"));
        QVERIFY(core::storeData(folder, u"dropped.txt"_s, "b"));
        note.body =
            u"<p><a class=\"attachment\" href=\"%1\" data-size=\"1\">kept.txt</a></p>"_s.arg(
                core::attachmentSource(note.meta.id, u"kept.txt"_s));
        QVERIFY(library->writeNote(note));
        QVERIFY(index->scan(*library));

        const QDateTime start = core::now();
        QCOMPARE(*index->removeOrphans(*library, start), 0);
        QCOMPARE(*index->removeOrphans(*library, start.addDays(29)), 0);
        QVERIFY(QFile::exists(folder + u"/dropped.txt"_s));
        QCOMPARE(*index->removeOrphans(*library, start.addDays(31)), 1);
        QVERIFY(!QFile::exists(folder + u"/dropped.txt"_s));
        QVERIFY(QFile::exists(folder + u"/kept.txt"_s));

        // Unreferenced for a while, then referenced again: the period starts over.
        note.body = u"<p>no files</p>"_s;
        QVERIFY(index->put(note, library->notePath(note.meta.id)));
        QCOMPARE(*index->removeOrphans(*library, start), 0);
        note.body =
            u"<p><img src=\"%1\"></p>"_s.arg(core::attachmentSource(note.meta.id, u"kept.txt"_s));
        QVERIFY(index->put(note, library->notePath(note.meta.id)));
        QCOMPARE(*index->removeOrphans(*library, start.addDays(10)), 0);
        note.body = u"<p>no files</p>"_s;
        QVERIFY(index->put(note, library->notePath(note.meta.id)));
        QCOMPARE(*index->removeOrphans(*library, start.addDays(20)), 0);
        QCOMPARE(*index->removeOrphans(*library, start.addDays(40)), 0);
        QVERIFY(QFile::exists(folder + u"/kept.txt"_s));
    }

    /// Programs are refused for opening; documents are not.
    void runnable()
    {
        QTemporaryDir dir;
        const auto write = [&](const QString &name, const QByteArray &data) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(data);
        };
        write(u"run.sh"_s, "#!/bin/sh\necho hi\n");
        write(u"app.desktop"_s, "[Desktop Entry]\nType=Application\nExec=x\n");
        write(u"notes.txt"_s, "plain text\n");
        write(u"hidden"_s, "#!/bin/sh\necho hi\n");
        QVERIFY(core::isRunnable(dir.filePath(u"run.sh"_s)));
        QVERIFY(core::isRunnable(dir.filePath(u"app.desktop"_s)));
        QVERIFY(core::isRunnable(dir.filePath(u"hidden"_s)));
        QVERIFY(!core::isRunnable(dir.filePath(u"notes.txt"_s)));
    }
    /// A conflict copy remains complete after the original note's attachments are deleted, also
    /// with a file larger than an insertion may be.
    void copyOwnsItsAttachments()
    {
        const QTemporaryDir dir;
        const QString original = core::newId();
        QVERIFY(core::storeData(core::attachmentsDir(dir.path(), original), u"file.txt"_s,
                                "independent"));
        core::Note copy = core::makeNote();
        copy.body =
            u"<p><a class=\"attachment\" href=\"%1\" data-size=\"11\">file.txt</a></p>"_s.arg(
                core::attachmentSource(original, u"file.txt"_s));
        const QString quotedName = u"file & data's.txt"_s;
        QVERIFY(core::storeData(core::attachmentsDir(dir.path(), original), quotedName, "quoted"));
        QString quotedSource = core::attachmentSource(original, quotedName);
        quotedSource.replace(u'&', u"&amp;"_s).replace(u'\'', u"&apos;"_s);
        copy.body +=
            u"<p><a class='attachment' href='%1' data-size='6'>quoted</a></p>"_s.arg(quotedSource);
        QFile large(core::attachmentsDir(dir.path(), original) + u"/large.bin"_s);
        QVERIFY(large.open(QIODevice::WriteOnly) && large.resize(core::MaxFileBytes + 1));
        large.close();
        copy.body += u"<p><a class=\"attachment\" href=\"%1\" data-size=\"1\">large</a></p>"_s.arg(
            core::attachmentSource(original, u"large.bin"_s));
        QVERIFY(core::copyNoteAttachments(dir.path(), copy));
        QCOMPARE(QFileInfo(core::attachmentsDir(dir.path(), copy.meta.id) + u"/large.bin"_s).size(),
                 core::MaxFileBytes + 1);
        QVERIFY(!copy.body.contains(original));
        QVERIFY(copy.body.contains(copy.meta.id));
        QVERIFY(core::removePath(core::attachmentsDir(dir.path(), original), true));
        QCOMPARE(core::readFile(core::attachmentsDir(dir.path(), copy.meta.id) + u'/' + quotedName)
                     .value(),
                 QByteArray("quoted"));
        QCOMPARE(
            core::readFile(core::attachmentsDir(dir.path(), copy.meta.id) + u"/file.txt"_s).value(),
            QByteArray("independent"));
    }

    /// A reference to a missing file does not stop the copy: it moves into the copy's folder,
    /// stays broken and never takes the name of a file copied beside it.
    void copyKeepsBrokenReferences()
    {
        const QTemporaryDir dir;
        const QString original = core::newId();
        const QString other = core::newId();
        QVERIFY(core::storeData(core::attachmentsDir(dir.path(), other), u"a.txt"_s, "present"));
        core::Note copy = core::makeNote();
        const QString gone = core::attachmentSource(original, u"a.txt"_s);
        copy.body = u"<p><a class=\"attachment\" href=\"%1\">gone</a> "
                    "<a class=\"attachment\" href=\"%2\">here</a></p>"_s.arg(
                        gone, core::attachmentSource(other, u"a.txt"_s));
        const auto copied = core::copyNoteAttachments(dir.path(), copy);
        QVERIFY(copied);
        QCOMPARE(*copied, QStringList{u"a.txt"_s});
        QVERIFY(!copy.body.contains(original));
        QVERIFY(!copy.body.contains(other));
        QVERIFY(copy.body.contains(core::attachmentSource(copy.meta.id, u"a.txt"_s)));
        QVERIFY(copy.body.contains(core::attachmentSource(copy.meta.id, u"a (2).txt"_s)));
        QVERIFY(
            !QFileInfo::exists(core::attachmentsDir(dir.path(), copy.meta.id) + u"/a (2).txt"_s));
    }
};

QTEST_GUILESS_MAIN(AttachmentsTest)
#include "tst_attachments.moc"
