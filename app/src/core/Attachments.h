#pragma once

#include "core/Error.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>

/// Images and files inserted into notes. Every function here touches only the
/// attachment folder it is given and runs on worker threads, where Qt Gui is not available.
namespace core {

struct Note;

/// Copies every referenced attachment into this note's own folder and rewrites its addresses.
/// A reference to a missing file is rewritten into the note's folder too and stays broken, so a
/// note with a broken reference can still be copied. A failure leaves the source files untouched
/// and must prevent publishing the note.
///
/// @param libraryRoot The library folder.
/// @param note The copy, with its own identifier; its body is rewritten.
/// @return The names of the files copied into the note's folder, or the error of reading or
///         writing one (a missing file is not an error).
Result<QStringList> copyNoteAttachments(const QString &libraryRoot, Note &note);

/// The largest file a note accepts, image or attachment.
inline constexpr qint64 MaxFileBytes = 50LL * 1024 * 1024;

/// The most pixels an inserted image may have. Qt decodes PNG, GIF and WebP at full size, four
/// bytes a pixel, before scaling them, and refuses an image over its 256 MB allocation limit;
/// JPEG is decoded already scaled down, so it has no such limit.
inline constexpr qint64 MaxImagePixels = qint64{8192} * 8192;

/// Days an attachment file stays after its note stopped referring to it, so undo and an
/// accidental deletion can still bring it back.
inline constexpr int OrphanGraceDays = 30;

/// The image kinds a note shows as images; any other file is an attachment.
enum class ImageKind { None, Png, Jpeg, Gif, WebP };

/// Detects an accepted image kind from the first bytes of a file, never from its name.
///
/// @param head At least the first 12 bytes of the file.
/// @return The kind, or None for any other content.
ImageKind imageKind(QByteArrayView head);

/// Returns the MIME type of an accepted image kind, or an empty string for None.
QString imageMimeType(ImageKind kind);

/// A reference of a note body to a file of an attachment folder.
struct AttachmentRef
{
    /// The note whose folder holds the file.
    QString noteId;
    QString fileName;
    /// Whether the body shows it as an image rather than as an attachment chip.
    bool image = false;

    bool operator==(const AttachmentRef &) const = default;
};

/// Returns a note's attachment folder: `<library-root>/attachments/<note-id>`.
QString attachmentsDir(const QString &libraryRoot, const QString &noteId);

/// Returns the address a note body uses for a file: `../attachments/<note-id>/<file>`, with the
/// characters that would break a URL percent-encoded.
QString attachmentSource(const QString &noteId, const QString &fileName);

/// Parses an address written by attachmentSource().
///
/// @return The note and the decoded file name, or no value for any other address.
std::optional<AttachmentRef> parseAttachmentSource(QStringView source);

/// The parts of the library a note body refers to.
struct BodyReferences
{
    /// Images and attachment chips, in body order; a file used twice is listed twice.
    QList<AttachmentRef> attachments;
    /// The notes it links to, each once.
    QStringList noteLinks;
};

/// Finds the attachment files and notes a note body refers to.
BodyReferences bodyReferences(QStringView body);

/// Makes a file name safe to store: path separators and control characters become `_`, and an
/// empty name becomes `file`.
QString safeFileName(const QString &name);

/// Returns @p name, or the first of `name (2).ext`, `name (3).ext`… that @p dir does not hold.
QString uniqueFileName(const QString &dir, const QString &name);

/// A file stored in an attachment folder.
struct StoredFile
{
    QString fileName;
    qint64 size = 0;
    ImageKind kind = ImageKind::None;
};

/// How storeFile() treats the file it copies.
enum class StoreAs {
    /// Any file, stored as an attachment.
    File,
    /// An image when it is an accepted image kind, otherwise an attachment.
    Image,
    /// Only an accepted image kind; anything else is refused before it is copied. Used for local
    /// files named by pasted HTML, which the person did not choose themselves.
    ImageOnly,
};

/// Copies a regular file into an attachment folder under its own name, with a suffix when the
/// name is taken. The copy has no executable permission bits. Directories, pipes and devices are
/// refused, so a copy never blocks on them.
///
/// @param dir The note's attachment folder; created when missing.
/// @param source The file to copy.
/// @param as Whether the file is meant as an image; an image over MaxImagePixels is stored as an
/// attachment, or refused for ImageOnly.
/// @return The stored file, or NotFound / Refused / TooLarge / Io.
Result<StoredFile> storeFile(const QString &dir, const QString &source, StoreAs as);

/// Stores bytes, such as a pasted image, as a new file of an attachment folder. An image over
/// MaxImagePixels is stored with the kind None.
///
/// @param dir The note's attachment folder; created when missing.
/// @param name The file name to use, with a suffix when it is taken.
/// @param data The file's contents.
/// @return The stored file, or TooLarge / Io.
Result<StoredFile> storeData(const QString &dir, const QString &name, const QByteArray &data);

/// Tells whether the desktop would run a file as a program (executables, scripts, desktop
/// entries, AppImages, Java archives, Windows programs), so Jotter must not open it.
bool isRunnable(const QString &path);

} // namespace core
