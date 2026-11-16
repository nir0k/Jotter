#include "core/Attachments.h"

#include "core/Directory.h"
#include "core/Files.h"
#include "core/Html.h"
#include "core/Note.h"
#include "core/NoteLinks.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QUrl>
#include <QtEndian>

#include <algorithm>
#include <functional>
#include <memory>

namespace core {

namespace {

constexpr QLatin1StringView SourcePrefix("../attachments/");

/// Files are copied a megabyte at a time, so a large attachment never sits in memory whole.
constexpr qint64 CopyChunkBytes = qint64{1} << 20;

/// Bytes of a file's head that hold its image kind and dimensions.
constexpr qint64 ImageHeadBytes = 30;

/// Returns the kind an inserted file is shown as: its image kind, or None for an image with more
/// than MaxImagePixels by the dimensions in its head. A head too short to hold them counts as
/// fitting; the decoder then fails on it as on any damaged file.
ImageKind shownKind(QByteArrayView head)
{
    const ImageKind kind = imageKind(head);
    const char *data = head.data();
    qint64 width = 0;
    qint64 height = 0;
    if (kind == ImageKind::Png && head.size() >= 24) {
        width = qFromBigEndian<quint32>(data + 16);
        height = qFromBigEndian<quint32>(data + 20);
    } else if (kind == ImageKind::Gif && head.size() >= 10) {
        width = qFromLittleEndian<quint16>(data + 6);
        height = qFromLittleEndian<quint16>(data + 8);
    } else if (kind == ImageKind::WebP && head.size() >= ImageHeadBytes) {
        // The first chunk is VP8X (extended), VP8L (lossless) or VP8 (lossy), each with its own
        // way of writing the dimensions.
        const QByteArrayView chunk = head.sliced(12, 4);
        if (chunk == "VP8X") {
            width = (qFromLittleEndian<quint32>(data + 24) & 0xFFFFFF) + 1;
            height = (qFromLittleEndian<quint32>(data + 26) >> 8) + 1;
        } else if (chunk == "VP8L") {
            const auto bits = qFromLittleEndian<quint32>(data + 21);
            width = (bits & 0x3FFF) + 1;
            height = ((bits >> 14) & 0x3FFF) + 1;
        } else if (chunk == "VP8 ") {
            width = qFromLittleEndian<quint16>(data + 26) & 0x3FFF;
            height = qFromLittleEndian<quint16>(data + 28) & 0x3FFF;
        }
    }
    return width * height > MaxImagePixels ? ImageKind::None : kind;
}

/// Characters percent-encoded in a file name of an address: those that end or change a URL
/// or an attribute. Other characters, Cyrillic letters included, stay readable.
bool needsEncoding(QChar c)
{
    return c.unicode() < 0x20 || c == u'%' || c == u'#' || c == u'?' || c == u' ' || c == u'"' ||
           c == u'<' || c == u'>' || c == u'\\' || c == u'^' || c == u'`' || c == u'{' ||
           c == u'|' || c == u'}' || c == QChar(0x7F);
}

/// Creates a new file that does not exist yet, choosing a free name as uniqueFileName() does.
/// Opening with NewOnly keeps two copies running at once from taking the same name.
///
/// @return The open file, or Io.
Result<std::unique_ptr<QFile>> createUnique(const QString &dir, const QString &name)
{
    if (!makeDirectory(dir)) {
        return fail(ErrorCode::Io, dir, QStringLiteral("cannot create the attachment folder"));
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
        const QString path = dir + u'/' + uniqueFileName(dir, name);
        auto opened = openFile(path, QIODevice::WriteOnly | QIODevice::NewOnly);
        if (opened) {
            auto file = std::move(*opened);
            file->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                 QFileDevice::ReadGroup | QFileDevice::ReadOther);
            return file;
        }
        // Another copy took the name between choosing and creating it; the next free one is
        // chosen. Any other failure is final.
        if (!QFileInfo(path).isFile() || QFileInfo(path).isSymLink()) {
            return std::unexpected(opened.error());
        }
    }
    return fail(ErrorCode::Io, dir + u'/' + name, QStringLiteral("no free file name"));
}

/// Writes the rest of a stored file and closes it; a failed write removes the partial file.
Result<StoredFile> finish(QFile &file, const std::function<bool(QFile &)> &write)
{
    if (!write(file) || !file.flush()) {
        const QString error = file.errorString();
        file.close();
        const auto removed = removePath(file.objectName());
        Q_UNUSED(removed)
        return fail(ErrorCode::Io, file.objectName(), error);
    }
    const qint64 size = file.size();
    file.close();
    return StoredFile{
        .fileName = QFileInfo(file.objectName()).fileName(), .size = size, .kind = ImageKind::None};
}

/// Copies an open file into a new file of a folder a megabyte at a time, whatever its size.
///
/// @param dir The folder, created when missing.
/// @param name The file name, made safe and unique.
/// @param input The open source file.
/// @return The stored file, as an attachment, or Io.
Result<StoredFile> storeStream(const QString &dir, const QString &name, QFile &input)
{
    auto output = createUnique(dir, safeFileName(name));
    if (!output) {
        return std::unexpected(output.error());
    }
    return finish(**output, [&input](QFile &file) {
        QByteArray buffer;
        while (!input.atEnd()) {
            buffer = input.read(CopyChunkBytes);
            if (buffer.isEmpty() || file.write(buffer) != buffer.size()) {
                return false;
            }
        }
        return true;
    });
}

} // namespace

ImageKind imageKind(QByteArrayView head)
{
    if (head.startsWith("\x89PNG\r\n\x1a\n")) {
        return ImageKind::Png;
    }
    if (head.startsWith("\xff\xd8\xff")) {
        return ImageKind::Jpeg;
    }
    if (head.startsWith("GIF87a") || head.startsWith("GIF89a")) {
        return ImageKind::Gif;
    }
    if (head.size() >= 12 && head.startsWith("RIFF") && head.sliced(8, 4) == "WEBP") {
        return ImageKind::WebP;
    }
    return ImageKind::None;
}

QString imageMimeType(ImageKind kind)
{
    switch (kind) {
    case ImageKind::Png:
        return QStringLiteral("image/png");
    case ImageKind::Jpeg:
        return QStringLiteral("image/jpeg");
    case ImageKind::Gif:
        return QStringLiteral("image/gif");
    case ImageKind::WebP:
        return QStringLiteral("image/webp");
    case ImageKind::None:
        break;
    }
    return {};
}

QString attachmentsDir(const QString &libraryRoot, const QString &noteId)
{
    return libraryRoot + QStringLiteral("/attachments/") + noteId;
}

QString attachmentSource(const QString &noteId, const QString &fileName)
{
    QString encoded;
    encoded.reserve(fileName.size());
    for (const QChar c : fileName) {
        if (needsEncoding(c)) {
            encoded +=
                QStringLiteral("%%1").arg(uint{c.unicode()}, 2, 16, QLatin1Char('0')).toUpper();
        } else {
            encoded += c;
        }
    }
    return SourcePrefix + noteId + u'/' + encoded;
}

std::optional<AttachmentRef> parseAttachmentSource(QStringView source)
{
    if (!source.startsWith(SourcePrefix)) {
        return std::nullopt;
    }
    const QStringView rest = source.mid(SourcePrefix.size());
    const qsizetype slash = rest.indexOf(u'/');
    if (slash <= 0) {
        return std::nullopt;
    }
    const QString noteId = rest.left(slash).toString();
    const QString fileName = QUrl::fromPercentEncoding(rest.mid(slash + 1).toUtf8());
    if (fileName.isEmpty() || fileName.contains(u'/') || fileName.contains(u'\\') ||
        fileName.contains(QChar::Null) || fileName == QLatin1StringView(".") ||
        fileName == QLatin1StringView("..") || noteId.contains(u'.') || noteId.contains(u'\\') ||
        noteId.contains(QChar::Null)) {
        return std::nullopt;
    }
    return AttachmentRef{.noteId = noteId, .fileName = fileName, .image = false};
}

BodyReferences bodyReferences(QStringView body)
{
    BodyReferences references;
    for (const html::Token &token : html::tokenize(body)) {
        if (token.kind != html::Token::Kind::Start) {
            continue;
        }
        if (token.name == QLatin1StringView("img")) {
            if (auto ref = parseAttachmentSource(token.attribute(u"src"))) {
                ref->image = true;
                references.attachments.append(*ref);
            }
        } else if (token.name == QLatin1StringView("a") &&
                   token.attribute(u"class") == QLatin1StringView("attachment")) {
            if (auto ref = parseAttachmentSource(token.attribute(u"href"))) {
                references.attachments.append(*ref);
            }
        } else if (token.name == QLatin1StringView("a")) {
            if (const auto target = noteLinkTarget(token.attribute(u"href"));
                target && !references.noteLinks.contains(*target)) {
                references.noteLinks.append(*target);
            }
        }
    }
    return references;
}

QString safeFileName(const QString &name)
{
    QString safe = name.trimmed();
    for (QChar &c : safe) {
        if (c == u'/' || c == u'\\' || c.unicode() < 0x20 || c == QChar(0x7F)) {
            c = u'_';
        }
    }
    if (safe.isEmpty() || safe == QLatin1StringView(".") || safe == QLatin1StringView("..")) {
        return QStringLiteral("file");
    }
    return safe;
}

QString uniqueFileName(const QString &dir, const QString &name)
{
    const QDir folder(dir);
    if (!folder.exists(name)) {
        return name;
    }
    // The suffix goes before the extension: `photo (2).jpg`; a name starting with a dot has no
    // extension of its own.
    const qsizetype dot = name.lastIndexOf(u'.');
    const QString base = dot > 0 ? name.left(dot) : name;
    const QString extension = dot > 0 ? name.mid(dot) : QString();
    for (int i = 2;; ++i) {
        const QString candidate = QStringLiteral("%1 (%2)%3").arg(base).arg(i).arg(extension);
        if (!folder.exists(candidate)) {
            return candidate;
        }
    }
}

Result<StoredFile> storeFile(const QString &dir, const QString &source, StoreAs as)
{
    const QFileInfo info(source);
    if (!info.exists()) {
        return fail(ErrorCode::NotFound, source);
    }
    if (!info.isFile()) {
        return fail(ErrorCode::Refused, source, QStringLiteral("not a regular file"));
    }
    auto opened = openFile(source, QIODevice::ReadOnly);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    QFile &input = **opened;
    const ImageKind kind =
        as != StoreAs::File ? shownKind(input.peek(ImageHeadBytes)) : ImageKind::None;
    if (as == StoreAs::ImageOnly && kind == ImageKind::None) {
        return fail(ErrorCode::Refused, source, QStringLiteral("not an accepted image"));
    }
    if (input.size() > MaxFileBytes) {
        return fail(ErrorCode::TooLarge, source);
    }
    auto stored = storeStream(dir, QFileInfo(source).fileName(), input);
    if (stored) {
        stored->kind = kind;
    }
    return stored;
}

Result<StoredFile> storeData(const QString &dir, const QString &name, const QByteArray &data)
{
    const ImageKind kind = shownKind(data);
    if (data.size() > MaxFileBytes) {
        return fail(ErrorCode::TooLarge, name);
    }
    auto output = createUnique(dir, safeFileName(name));
    if (!output) {
        return std::unexpected(output.error());
    }
    auto stored =
        finish(**output, [&data](QFile &file) { return file.write(data) == data.size(); });
    if (stored) {
        stored->kind = kind;
    }
    return stored;
}

Result<QStringList> copyNoteAttachments(const QString &libraryRoot, Note &note)
{
    const auto tokens = html::tokenize(note.body);
    QHash<QString, QString> copied;
    QStringList names;
    // References to files that are missing, by address, with the name they had.
    QList<std::pair<QString, QString>> missing;
    for (const html::Token &token : tokens) {
        if (token.kind != html::Token::Kind::Start) {
            continue;
        }
        QString source;
        if (token.name == QLatin1StringView("img")) {
            source = token.attribute(u"src");
        } else if (token.name == QLatin1StringView("a") &&
                   token.attribute(u"class") == QLatin1StringView("attachment")) {
            source = token.attribute(u"href");
        }
        auto reference = parseAttachmentSource(source);
        if (!reference || copied.contains(source) ||
            std::ranges::find(missing, source, &std::pair<QString, QString>::first) !=
                missing.end()) {
            continue;
        }
        if (!isValidId(reference->noteId)) {
            return fail(ErrorCode::Refused, source);
        }
        // Files already in the library are copied whatever their size: the size limit applies
        // only to what the person inserts.
        auto input =
            openFile(attachmentsDir(libraryRoot, reference->noteId) + u'/' + reference->fileName,
                     QIODevice::ReadOnly);
        if (!input) {
            if (input.error().code == ErrorCode::NotFound) {
                missing.append({source, reference->fileName});
                continue;
            }
            return std::unexpected(input.error());
        }
        auto stored =
            storeStream(attachmentsDir(libraryRoot, note.meta.id), reference->fileName, **input);
        if (!stored) {
            return std::unexpected(stored.error());
        }
        copied.insert(source, attachmentSource(note.meta.id, stored->fileName));
        names.append(stored->fileName);
    }
    // A missing file stays a broken reference, now into the copy's own folder, under a name no
    // copied file took, so it never shows another file.
    for (const auto &[source, fileName] : std::as_const(missing)) {
        copied.insert(
            source,
            attachmentSource(note.meta.id,
                             uniqueFileName(attachmentsDir(libraryRoot, note.meta.id), fileName)));
    }
    if (!copied.isEmpty()) {
        // Rebuild from decoded tokens so single quotes and alternate entity spellings do not
        // leave a copied note pointing at its original folder.
        QString body;
        for (const html::Token &token : tokens) {
            if (token.kind == html::Token::Kind::Text) {
                body += html::escapeText(token.text);
            } else if (token.kind == html::Token::Kind::End) {
                body += QStringLiteral("</") + token.name + u'>';
            } else {
                body += u'<' + token.name;
                for (const auto &[key, value] : token.attributes) {
                    const QString rewritten =
                        (key == QLatin1StringView("src") || key == QLatin1StringView("href"))
                            ? copied.value(value, value)
                            : value;
                    body += u' ' + key + QStringLiteral("=\"") + html::escapeAttribute(rewritten) +
                            u'"';
                }
                body += u'>';
            }
        }
        note.body = std::move(body);
    }
    return names;
}

bool isRunnable(const QString &path)
{
    static const QStringList runnable{
        QStringLiteral("application/x-executable"),
        QStringLiteral("application/x-pie-executable"),
        QStringLiteral("application/x-sharedlib"),
        QStringLiteral("application/x-shellscript"),
        QStringLiteral("application/x-desktop"),
        QStringLiteral("application/vnd.appimage"),
        QStringLiteral("application/x-java-archive"),
        QStringLiteral("application/x-ms-dos-executable"),
        QStringLiteral("application/x-msdownload"),
        QStringLiteral("application/x-msi"),
    };
    // Both the name and the content are checked: either alone can be misleading.
    const QMimeDatabase database;
    for (const QMimeType &type : {database.mimeTypeForFile(path, QMimeDatabase::MatchExtension),
                                  database.mimeTypeForFile(path, QMimeDatabase::MatchContent)}) {
        if (std::ranges::any_of(runnable,
                                [&](const QString &name) { return type.inherits(name); })) {
            return true;
        }
    }
    return false;
}

} // namespace core
