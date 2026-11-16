#include "core/Attachments.h"
#include "core/Files.h"
#include "core/Index.h"
#include "core/Library.h"
#include "core/Merge.h"
#include "core/Rules.h"
#include "editor/HtmlCodec.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>

#include <optional>
#include <tuple>

using namespace Qt::StringLiterals;

namespace {

/// The shared fixtures in the repository's `tests/fixtures/`.
QString fixturePath(const QString &path)
{
    return QStringLiteral(JOTTER_FIXTURES_DIR "/") + path;
}

/// Tells whether the test rewrites the fixtures in the form the application writes, set by
/// `JOTTER_UPDATE_FIXTURES`, instead of checking that they already are in it.
bool updating()
{
    return qEnvironmentVariableIsSet("JOTTER_UPDATE_FIXTURES");
}

/// Reads a body into a fresh document and writes it back, as the editor does when a note is
/// opened and saved.
QString editorRoundTrip(const QString &body)
{
    QTextDocument document;
    editor::readBody(document, body);
    return editor::writeBody(document);
}

/// Names an error code as `problems/expected.json` does.
QString codeName(core::ErrorCode code)
{
    switch (code) {
    case core::ErrorCode::Malformed:
        return u"malformed"_s;
    case core::ErrorCode::NewerFormat:
        return u"newerFormat"_s;
    default:
        return u"other"_s;
    }
}

/// Copies a folder with its subfolders.
///
/// @param source The folder to copy.
/// @param target The folder to create the copy in; it may already exist.
/// @return Whether every file was copied.
bool copyTree(const QString &source, const QString &target)
{
    QDirIterator it(source, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString copy = target + u'/' + QDir(source).relativeFilePath(file);
        if (!QDir().mkpath(QFileInfo(copy).path()) || !QFile::copy(file, copy)) {
            return false;
        }
    }
    return true;
}

/// The versions of one library file in a merge case of `merge/`.
struct Versions
{
    /// The version both sides started from; none for a first connection.
    std::optional<QByteArray> base;
    QByteArray local;
    QByteArray remote;
};

/// Parses the versions of one file with @p parse.
///
/// @return The base, local and remote values, or no value when a version cannot be read.
template <typename T, typename Parse>
std::optional<std::tuple<std::optional<T>, T, T>> parseVersions(Parse parse, const QString &path,
                                                                const Versions &versions)
{
    auto local = parse(versions.local, path);
    auto remote = parse(versions.remote, path);
    if (!local || !remote) {
        return std::nullopt;
    }
    std::optional<T> base;
    if (versions.base) {
        auto parsed = parse(*versions.base, path);
        if (!parsed) {
            return std::nullopt;
        }
        base = std::move(*parsed);
    }
    return std::tuple(std::move(base), std::move(*local), std::move(*remote));
}

/// Merges the versions of a library file as a sync run does; the file's
/// name tells its kind.
///
/// @param path The file, named as in a library, as notes must be named after their identifier.
/// @param versions The versions to merge.
/// @param now The time of the merge, which expires tombstones.
/// @param conflict Set when the note bodies conflict and the local note becomes a copy.
/// @return The merged file, or no value when a version cannot be read.
std::optional<QByteArray> mergeFile(const QString &path, const Versions &versions,
                                    const QDateTime &now, bool &conflict)
{
    const QString name = QFileInfo(path).fileName();
    if (name == u"library.json"_s) {
        auto parsed = parseVersions<core::LibraryInfo>(core::parseLibraryInfo, path, versions);
        if (!parsed) {
            return std::nullopt;
        }
        const auto &[base, local, remote] = *parsed;
        return core::serializeLibraryInfo(core::mergeLibraryInfo(base, local, remote, now));
    }
    if (name == u"collections.json"_s) {
        auto parsed =
            parseVersions<QList<core::Collection>>(core::parseCollections, path, versions);
        if (!parsed) {
            return std::nullopt;
        }
        const auto &[base, local, remote] = *parsed;
        return core::serializeCollections(
            core::mergeCollections(base.value_or(QList<core::Collection>()), local, remote));
    }
    if (name == u"rules.json"_s) {
        auto parsed = parseVersions<QList<core::Rule>>(core::parseRules, path, versions);
        if (!parsed) {
            return std::nullopt;
        }
        const auto &[base, local, remote] = *parsed;
        return core::serializeRules(
            core::mergeRules(base.value_or(QList<core::Rule>()), local, remote));
    }
    auto parsed = parseVersions<core::Note>(core::parseNote, path, versions);
    if (!parsed) {
        return std::nullopt;
    }
    const auto &[base, local, remote] = *parsed;
    const core::NoteMerge merged = core::mergeNote(base, local, remote);
    conflict = merged.conflict;
    return core::serializeNote(merged.note);
}

/// Rewrites a library file in the form the application writes, as JOTTER_UPDATE_FIXTURES does.
///
/// @return The rewritten file, or no value when it cannot be read.
std::optional<QByteArray> rewritten(const QString &path, const QByteArray &data)
{
    const QString name = QFileInfo(path).fileName();
    if (name == u"library.json"_s) {
        auto info = core::parseLibraryInfo(data, path);
        return info ? std::optional(core::serializeLibraryInfo(*info)) : std::nullopt;
    }
    if (name == u"collections.json"_s) {
        auto collections = core::parseCollections(data, path);
        return collections ? std::optional(core::serializeCollections(*collections)) : std::nullopt;
    }
    if (name == u"rules.json"_s) {
        auto rules = core::parseRules(data, path);
        return rules ? std::optional(core::serializeRules(*rules)) : std::nullopt;
    }
    auto note = core::parseNote(data, path);
    return note ? std::optional(core::serializeNote(*note)) : std::nullopt;
}

} // namespace

/// Checks the shared library fixtures that every client of the format reads and writes back
/// byte for byte: they are exactly what this application writes, the editor
/// keeps every note body unchanged, and damaged files give the problems they are listed with.
/// With `JOTTER_UPDATE_FIXTURES` set, the fixtures are rewritten instead of checked.
class FixturesTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    /// Every library file and note of the readable libraries.
    void files_data()
    {
        QTest::addColumn<QString>("path");
        for (const QString &library : {u"library"_s, u"problems"_s}) {
            for (const QString &file :
                 {u"library.json"_s, u"collections.json"_s, u"rules.json"_s}) {
                const QString path = library + u'/' + file;
                QTest::newRow(qPrintable(path)) << path;
            }
        }
        const QDir notes(fixturePath(u"library/notes"_s));
        for (const QString &file : notes.entryList({u"*.html"_s}, QDir::Files, QDir::Name)) {
            const QString path = u"library/notes/"_s + file;
            QTest::newRow(qPrintable(path)) << path;
        }
    }

    /// A file reads and writes back unchanged; a note body also survives the editor.
    void files()
    {
        QFETCH(QString, path);
        const QString file = fixturePath(path);
        const auto data = core::readFile(file);
        QVERIFY(data);

        QByteArray written;
        const QString name = QFileInfo(file).fileName();
        if (name == u"library.json"_s) {
            const auto info = core::parseLibraryInfo(*data, file);
            QVERIFY2(info, qPrintable(info ? QString() : core::describe(info.error())));
            written = core::serializeLibraryInfo(*info);
        } else if (name == u"collections.json"_s) {
            const auto collections = core::parseCollections(*data, file);
            QVERIFY2(collections,
                     qPrintable(collections ? QString() : core::describe(collections.error())));
            written = core::serializeCollections(*collections);
        } else if (name == u"rules.json"_s) {
            const auto rules = core::parseRules(*data, file);
            QVERIFY2(rules, qPrintable(rules ? QString() : core::describe(rules.error())));
            written = core::serializeRules(*rules);
        } else {
            auto note = core::parseNote(*data, file);
            QVERIFY2(note, qPrintable(note ? QString() : core::describe(note.error())));
            const QString body = editorRoundTrip(note->body);
            if (!updating()) {
                QCOMPARE(body, note->body);
            }
            note->body = body;
            written = core::serializeNote(*note);
        }

        if (updating()) {
            if (written != *data) {
                QVERIFY(core::writeFileAtomic(file, written));
            }
            return;
        }
        QCOMPARE(QString::fromUtf8(written), QString::fromUtf8(*data));
    }

    /// Every image and attachment a note shows is in its folder, images are of an accepted kind
    /// by their bytes, and no attachment file is left unused.
    void attachments()
    {
        const QString root = fixturePath(u"library"_s);
        QSet<QString> referenced;
        const QDir notes(root + u"/notes"_s);
        for (const QFileInfo &file : notes.entryInfoList({u"*.html"_s}, QDir::Files)) {
            const auto note = core::parseNote(*core::readFile(file.filePath()), file.filePath());
            QVERIFY(note);
            for (const core::AttachmentRef &ref : core::bodyReferences(note->body).attachments) {
                QCOMPARE(ref.noteId, note->meta.id);
                const QString path = core::attachmentsDir(root, ref.noteId) + u'/' + ref.fileName;
                const auto data = core::readFile(path);
                QVERIFY2(data, qPrintable(path));
                QCOMPARE(core::imageKind(*data) != core::ImageKind::None, ref.image);
                referenced.insert(path);
            }
        }
        QDirIterator files(root + u"/attachments"_s, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString path = files.next();
            QVERIFY2(referenced.contains(path), qPrintable(path));
        }
    }

    /// The damaged notes of the problems library are listed with the codes it expects, and a
    /// file not named after an identifier is not a note.
    void problems()
    {
        const QTemporaryDir dir;
        const QString root = dir.filePath(u"problems"_s);
        QVERIFY(copyTree(fixturePath(u"problems"_s), root));
        auto library = core::Library::open(root);
        QVERIFY(library);
        auto index = core::Index::open(dir.filePath(u"index.sqlite"_s));
        QVERIFY(index);
        const auto scanned = index->scan(*library);
        QVERIFY2(scanned, qPrintable(scanned ? QString() : core::describe(scanned.error())));
        const auto problems = index->problems();
        QVERIFY(problems);

        QJsonObject found;
        for (const core::Problem &problem : *problems) {
            found.insert(problem.path, codeName(problem.error.code));
        }
        const auto expected = core::parseJsonObject(
            *core::readFile(fixturePath(u"problems/expected.json"_s)), u"expected.json"_s);
        QVERIFY(expected);
        QCOMPARE(found, expected->value(u"problems"_s).toObject());
    }

    /// The library written by a newer version opens read-only.
    void newerLibrary()
    {
        const auto library = core::Library::open(fixturePath(u"newer-library"_s));
        QVERIFY(library);
        QVERIFY(library->isReadOnly());
    }

    /// Every merge case of `merge/`.
    void merges_data()
    {
        QTest::addColumn<QString>("name");
        const QDir cases(fixturePath(u"merge"_s));
        for (const QString &name : cases.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            QTest::newRow(qPrintable(name)) << name;
        }
    }

    /// The local and remote versions of every file of a merge case, merged against the base
    /// (none for a first connection) at the case's time, give the merged version byte for byte,
    /// and the note bodies conflict exactly where the case says.
    void merges()
    {
        QFETCH(QString, name);
        const QString dir = fixturePath(u"merge/"_s + name);
        const auto description = core::parseJsonObject(
            core::readFile(dir + u"/case.json"_s).value_or(QByteArray()), u"case.json"_s);
        QVERIFY(description);
        const auto now = core::parseTime(description->value(u"now"_s).toString());
        QVERIFY(now);
        QStringList expectedConflicts;
        for (const QJsonValue path : description->value(u"conflicts"_s).toArray()) {
            expectedConflicts.append(path.toString());
        }

        const QDir local(dir + u"/local"_s);
        QDirIterator files(local.path(), QDir::Files, QDirIterator::Subdirectories);
        QStringList paths;
        while (files.hasNext()) {
            paths.append(local.relativeFilePath(files.next()));
        }
        paths.sort();
        QVERIFY(!paths.isEmpty());
        QStringList conflicts;
        for (const QString &path : std::as_const(paths)) {
            const auto read = [&](const QString &side) {
                return core::readFile(dir + u'/' + side + u'/' + path);
            };
            if (updating()) {
                for (const QString &side : {u"base"_s, u"local"_s, u"remote"_s}) {
                    if (const auto data = read(side)) {
                        const auto written = rewritten(path, *data);
                        QVERIFY2(written, qPrintable(side + u'/' + path));
                        if (*written != *data) {
                            QVERIFY(
                                core::writeFileAtomic(dir + u'/' + side + u'/' + path, *written));
                        }
                    }
                }
            }
            const auto localData = read(u"local"_s);
            const auto remoteData = read(u"remote"_s);
            QVERIFY2(localData && remoteData, qPrintable(path));
            const auto baseData = read(u"base"_s);
            const Versions versions{.base = baseData ? std::optional(*baseData) : std::nullopt,
                                    .local = *localData,
                                    .remote = *remoteData};
            bool conflict = false;
            const auto merged = mergeFile(path, versions, *now, conflict);
            QVERIFY2(merged, qPrintable(path));
            if (conflict) {
                conflicts.append(path);
            }
            const QString mergedPath = dir + u"/merged/"_s + path;
            if (updating()) {
                QVERIFY(core::writeFileAtomic(mergedPath, *merged));
                continue;
            }
            const auto expected = core::readFile(mergedPath);
            QVERIFY2(expected, qPrintable(mergedPath));
            QCOMPARE(QString::fromUtf8(*merged), QString::fromUtf8(*expected));
        }
        QCOMPARE(conflicts, expectedConflicts);
    }
};

QTEST_MAIN(FixturesTest)
#include "tst_fixtures.moc"
