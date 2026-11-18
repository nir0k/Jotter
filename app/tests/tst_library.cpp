#include "core/Attachments.h"
#include "core/Directory.h"
#include "core/Files.h"
#include "core/Library.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

/// Checks the library format: creating and opening libraries, note files, collections, and
/// how damaged or newer files are handled.
class LibraryTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    /// A created library opens again with the same identity and layout.
    void createAndOpen()
    {
        const QTemporaryDir dir;
        const QString root = dir.filePath(QStringLiteral("Jotter"));
        auto created = core::Library::create(root);
        QVERIFY(created);
        for (const char *name :
             {"library.json", "collections.json", "rules.json", "notes", "attachments"}) {
            QVERIFY2(QFileInfo::exists(QDir(root).filePath(QString::fromLatin1(name))), name);
        }
        auto opened = core::Library::open(root);
        QVERIFY(opened);
        QCOMPARE(opened->info(), created->info());
        QVERIFY(!opened->isReadOnly());
        QCOMPARE(opened->info().trashRetentionDays, std::optional<int>(30));
    }

    /// A new library is never created over existing files.
    void createRefusesNonEmptyFolder()
    {
        const QTemporaryDir dir;
        QFile file(dir.filePath(QStringLiteral("notes.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const auto created = core::Library::create(dir.path());
        QVERIFY(!created);
        QCOMPARE(created.error().code, core::ErrorCode::NotEmpty);
    }

    /// A note with every field set, including text that could end the metadata script early,
    /// reads back unchanged.
    void noteRoundTrip()
    {
        const QTemporaryDir dir;
        auto library = core::Library::create(dir.path());
        QVERIFY(library);
        core::Note note = core::makeNote();
        note.meta.title = QStringLiteral("План </script><b>«недели»</b>");
        note.meta.icon = QStringLiteral("Calendar");
        note.meta.color = QStringLiteral("blue");
        note.meta.collections = {core::newId(), core::newId()};
        note.meta.tags = {QStringLiteral("work/planning"), QStringLiteral("reading list")};
        note.meta.archived = core::now();
        note.meta.location = core::GeoLocation{
            .latitude = 55.75583, .longitude = 37.6173, .accuracy = 35.0, .captured = core::now()};
        note.meta.extra.insert(QStringLiteral("futureField"), QStringLiteral("kept"));
        note.body = QStringLiteral("<p>Hello <strong>world</strong></p>\n<ul><li>one</li></ul>\n"
                                   "<p><img src=\"../attachments/a/b.png\" data-width=\"37\"></p>");

        QVERIFY(library->writeNote(note));
        // A browser gets the widths of the note's images from the generated style sheet.
        QVERIFY(core::readFile(library->notePath(note.meta.id))
                    ->contains(R"(img[data-width="37"]{max-width:37%})"));
        const auto read = library->readNote(note.meta.id);
        QVERIFY2(read, qPrintable(read ? QString() : read.error().detail));
        QCOMPARE(read->meta, note.meta);
        QCOMPARE(read->body, note.body);
    }

    /// Damaged note files are reported as malformed with a reason, never read half-way.
    void malformedNotes_data()
    {
        QTest::addColumn<QByteArray>("contents");
        QTest::addColumn<QString>("reason");
        QTest::newRow("no metadata")
            << QByteArray("<html><body>text</body></html>") << QStringLiteral("no metadata block");
        QTest::newRow("bad json")
            << QByteArray(R"(<script type="application/json" id="jotter-meta">{"format":</script>)")
            << QString();
        QTest::newRow("wrong type")
            << QByteArray(
                   R"(<script type="application/json" id="jotter-meta">{"format":1,"tags":"x"}</script>)")
            << QString();
    }

    void malformedNotes()
    {
        QFETCH(QByteArray, contents);
        QFETCH(QString, reason);
        const auto parsed =
            core::parseNote(contents, QStringLiteral("/lib/notes/%1.html").arg(core::newId()));
        QVERIFY(!parsed);
        QCOMPARE(parsed.error().code, core::ErrorCode::Malformed);
        if (!reason.isEmpty()) {
            QCOMPARE(parsed.error().detail, reason);
        }
    }

    /// A note whose identifier differs from its file name is rejected, so that one file cannot
    /// shadow another note.
    void noteIdMustMatchFileName()
    {
        const core::Note note = core::makeNote();
        const auto parsed = core::parseNote(
            core::serializeNote(note), QStringLiteral("/lib/notes/%1.html").arg(core::newId()));
        QVERIFY(!parsed);
        QCOMPARE(parsed.error().code, core::ErrorCode::Malformed);
    }

    /// A library written by a newer Jotter opens read-only and refuses writes.
    void newerLibraryIsReadOnly()
    {
        const QTemporaryDir dir;
        const QString id = core::newId();
        QVERIFY(core::writeFileAtomic(
            dir.filePath(QStringLiteral("library.json")),
            core::toJsonText(QJsonObject{
                {QStringLiteral("format"), 99},
                {QStringLiteral("id"), id},
                {QStringLiteral("created"), QStringLiteral("2026-10-23T10:15:00.000Z")}})));
        auto library = core::Library::open(dir.path());
        QVERIFY(library);
        QVERIFY(library->isReadOnly());
        QCOMPARE(library->info().id, id);
        const auto written = library->writeNote(core::makeNote());
        QVERIFY(!written);
        QCOMPARE(written.error().code, core::ErrorCode::ReadOnly);
        QCOMPARE(library->readOnlyReason(), core::ReadOnlyReason::NewerFormat);
    }

    /// A library whose root or notes folder the person may not write to opens read-only with
    /// NoPermission and refuses writes before touching a file; once writable it opens as usual.
    void unwritableLibraryIsReadOnly_data()
    {
        QTest::addColumn<QString>("folder");
        QTest::newRow("root") << QString();
        QTest::newRow("notes") << QStringLiteral("notes");
    }

    void unwritableLibraryIsReadOnly()
    {
        QFETCH(QString, folder);
        QTemporaryDir dir;
        QVERIFY(core::Library::create(dir.path()));
        const QString target = folder.isEmpty() ? dir.path() : dir.filePath(folder);
        const auto writable = QFile::permissions(target);
        QVERIFY(QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const auto restore = qScopeGuard([&] { QFile::setPermissions(target, writable); });
        if (QFileInfo(target).isWritable()) {
            QSKIP("Folder permissions do not apply to this user (root).");
        }
        auto library = core::Library::open(dir.path());
        QVERIFY(library);
        QVERIFY(library->isReadOnly());
        QCOMPARE(library->readOnlyReason(), core::ReadOnlyReason::NoPermission);
        const auto written = library->writeNote(core::makeNote());
        QVERIFY(!written);
        QCOMPARE(written.error().code, core::ErrorCode::ReadOnly);

        QVERIFY(QFile::setPermissions(target, writable));
        library = core::Library::open(dir.path());
        QVERIFY(library);
        QCOMPARE(library->readOnlyReason(), core::ReadOnlyReason::None);
    }

    /// The collection tree keeps its order, parents and unknown fields.
    void collectionsRoundTrip()
    {
        const QTemporaryDir dir;
        auto library = core::Library::create(dir.path());
        QVERIFY(library);
        QVERIFY(library->readCollections()->isEmpty());

        const core::Collection work{.id = core::newId(),
                                    .parent = std::nullopt,
                                    .name = QStringLiteral("Work"),
                                    .icon = QStringLiteral("Folder"),
                                    .color = QStringLiteral("orange"),
                                    .created = core::now(),
                                    .updated = core::now(),
                                    .extra = {}};
        core::Collection meetings = work;
        meetings.id = core::newId();
        meetings.parent = work.id;
        meetings.name = QStringLiteral("Встречи");
        meetings.color = std::nullopt;
        meetings.extra.insert(QStringLiteral("futureField"), 1);

        QVERIFY(library->writeCollections({work, meetings}));
        const auto read = library->readCollections();
        QVERIFY(read);
        QCOMPARE(*read, (QList<core::Collection>{work, meetings}));
    }
    /// A stale settings edit preserves tombstones and colours downloaded since it was read.
    void metadataWriteMergesCurrentFile()
    {
        const QTemporaryDir dir;
        auto stale = core::Library::create(dir.path());
        QVERIFY(stale);
        auto remote = core::Library::open(dir.path());
        QVERIFY(remote);
        core::LibraryInfo downloaded = remote->info();
        downloaded.tombstones.append({.id = core::newId(), .deleted = core::now()});
        downloaded.tagColors.insert(QStringLiteral("remote"), QStringLiteral("red"));
        QVERIFY(remote->writeInfo(downloaded));
        core::LibraryInfo local = stale->info();
        local.trashRetentionDays = 45;
        QVERIFY(stale->writeInfo(local));
        QCOMPARE(stale->info().tombstones, downloaded.tombstones);
        QCOMPARE(stale->info().tagColors, downloaded.tagColors);
        QCOMPARE(stale->info().trashRetentionDays, std::optional<int>(45));
    }

    /// Wrong container types are corruption, never an empty collection or rules list.
    void rejectsWrongContainerTypes()
    {
        for (const QByteArray &data :
             {QByteArray(R"({"format":1,"rules":{}})"), QByteArray(R"({"rules":[]})")}) {
            QVERIFY(!core::parseRules(data, QStringLiteral("rules.json")));
        }
        QVERIFY(!core::parseCollections(R"({"format":1,"collections":{}})",
                                        QStringLiteral("collections.json")));
        const QTemporaryDir dir;
        auto library = core::Library::create(dir.path());
        QVERIFY(library);
        auto object = QJsonDocument::fromJson(core::serializeLibraryInfo(library->info())).object();
        for (const QString &key : {QStringLiteral("favorites"), QStringLiteral("tombstones"),
                                   QStringLiteral("tagColors"), QStringLiteral("settings")}) {
            auto damaged = object;
            damaged.insert(key, QStringLiteral("wrong"));
            QVERIFY(
                !core::parseLibraryInfo(core::toJsonText(damaged), QStringLiteral("library.json")));
        }
    }

    /// Unsupported HTML is readable only by explicit normalisation and stays byte-for-byte intact.
    void unsupportedBodyIsProtected()
    {
        core::Note note = core::makeNote();
        note.body = QStringLiteral("<p>before</p><video src=\"outside\">kept</video>");
        const QByteArray original = core::serializeNote(note);
        const QString path = note.meta.id + QStringLiteral(".html");
        const auto parsed = core::parseNote(original, path);
        QVERIFY(!parsed);
        QVERIFY(parsed.error().detail.startsWith(QLatin1StringView("unsupported body:")));
        QCOMPARE(core::parseNoteForNormalization(original, path)->body, note.body);
    }

    /// Symlinked parents never expose or delete files outside the library boundary.
    void linkedDirectoriesAreRefused()
    {
        const QTemporaryDir dir;
        const QTemporaryDir outside;
        auto library = core::Library::create(dir.path());
        QVERIFY(library);
        QVERIFY(core::writeFileAtomic(outside.filePath(QStringLiteral("keep")), "private"));
        const QString folder = library->attachmentsDir(core::newId());
        QVERIFY(QFile::link(outside.path(), folder));
        QVERIFY(!core::readFile(folder + QStringLiteral("/keep")));
        QVERIFY(!core::writeFileAtomic(folder + QStringLiteral("/keep"), "changed"));
        QVERIFY(!core::listDirectory(folder));
        QVERIFY(core::removePath(folder, true));
        QCOMPARE(core::readFile(outside.filePath(QStringLiteral("keep"))).value(),
                 QByteArray("private"));
    }

    /// A library in a folder reached through a symbolic link, such as a documents folder on
    /// another disk, is created and opened; links inside the library stay refused.
    void linkedParentOfLibrary()
    {
        const QTemporaryDir dir;
        QVERIFY(core::makeDirectory(dir.filePath(QStringLiteral("disk/Documents"))));
        QVERIFY(QFile::link(dir.filePath(QStringLiteral("disk/Documents")),
                            dir.filePath(QStringLiteral("Documents"))));
        const QString root = dir.filePath(QStringLiteral("Documents/Jotter"));
        auto created = core::Library::create(root);
        QVERIFY(created);
        auto opened = core::Library::open(root);
        QVERIFY(opened);
        QCOMPARE(opened->info(), created->info());
        QCOMPARE(opened->root(),
                 core::resolvedPath(dir.filePath(QStringLiteral("disk/Documents/Jotter"))));
    }

    /// A `..` in a path never leads out of the folder it names, although the folders exist.
    void dotDotNeverLeavesTheFolder()
    {
        const QTemporaryDir dir;
        QVERIFY(core::makeDirectory(dir.filePath(QStringLiteral("library/attachments/x"))));
        const QString escape = dir.filePath(QStringLiteral("library/attachments/x/../../escaped"));
        QVERIFY(!core::writeFileAtomic(escape, "data"));
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("library/escaped"))));
        QVERIFY(!core::readFile(dir.filePath(QStringLiteral("library/attachments/x/../x"))));
    }

    /// A rewritten file keeps the read and write permissions it had and never becomes
    /// executable.
    void rewriteKeepsPermissions()
    {
        const QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("note.html"));
        QVERIFY(core::writeFileAtomic(path, "old"));
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                QFileDevice::ExeOwner));
        QVERIFY(core::writeFileAtomic(path, "new"));
        QCOMPARE(QFile::permissions(path) &
                     (QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ExeUser |
                      QFileDevice::ReadGroup | QFileDevice::ReadOther),
                 QFileDevice::ReadUser | QFileDevice::WriteUser);
    }

    /// A failed download staging attempt leaves the target reusable and cannot replace new data.
    void stagingPublicationRefusesOccupiedTarget()
    {
        const QTemporaryDir dir;
        const QString staging = dir.filePath(QStringLiteral("stage"));
        const QString target = dir.filePath(QStringLiteral("target"));
        QVERIFY(core::makeDirectory(staging));
        QVERIFY(core::writeFileAtomic(staging + QStringLiteral("/downloaded"), "complete"));
        QVERIFY(core::makeDirectory(target));
        QVERIFY(core::writeFileAtomic(target + QStringLiteral("/keep"), "user data"));
        QVERIFY(!core::publishDirectory(staging, target));
        QCOMPARE(core::readFile(target + QStringLiteral("/keep")).value(), QByteArray("user data"));
        QVERIFY(core::removePath(target + QStringLiteral("/keep")));
        QVERIFY(core::publishDirectory(staging, target));
        QCOMPARE(core::readFile(target + QStringLiteral("/downloaded")).value(),
                 QByteArray("complete"));
    }
    /// Save as retains existing bytes on failure and safely supports the original path.
    void atomicCopyRetainsDestination()
    {
        const QTemporaryDir dir;
        const QString original = dir.filePath(QStringLiteral("original"));
        const QString target = dir.filePath(QStringLiteral("target"));
        QVERIFY(core::writeFileAtomic(original, "source bytes"));
        QVERIFY(core::writeFileAtomic(target, "old target"));
        QVERIFY(core::copyFileAtomic(original, original));
        QCOMPARE(core::readFile(original).value(), QByteArray("source bytes"));
        QVERIFY(!core::copyFileAtomic(dir.filePath(QStringLiteral("missing")), target));
        QCOMPARE(core::readFile(target).value(), QByteArray("old target"));
        QVERIFY(core::copyFileAtomic(original, target));
        QCOMPARE(core::readFile(target).value(), QByteArray("source bytes"));
    }

    /// An error names the whole path, whether a folder or the file itself is missing, so the
    /// message tells where the problem is.
    void errorsNameTheWholePath()
    {
        const QTemporaryDir dir;
        const QString root = core::resolvedPath(dir.path());
        const auto noFolder = core::readFile(root + QStringLiteral("/gone/file.txt"));
        QCOMPARE(noFolder.error().code, core::ErrorCode::NotFound);
        QCOMPARE(noFolder.error().path, root + QStringLiteral("/gone"));
        const auto noFile = core::readFile(root + QStringLiteral("/file.txt"));
        QCOMPARE(noFile.error().code, core::ErrorCode::NotFound);
        QCOMPARE(noFile.error().path, root + QStringLiteral("/file.txt"));
    }
};

QTEST_GUILESS_MAIN(LibraryTest)
#include "tst_library.moc"
