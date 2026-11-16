#pragma once

#include "core/Error.h"

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace core {

/// The note format version this build reads and writes.
inline constexpr int NoteFormat = 1;

/// Coordinates attached to a note when it was created.
struct GeoLocation
{
    double latitude = 0.0;
    double longitude = 0.0;
    /// Accuracy in metres, when the position service reported one.
    std::optional<double> accuracy;
    QDateTime captured;

    bool operator==(const GeoLocation &) const = default;
};

/// The metadata block of a note file.
struct NoteMeta
{
    QString id;
    QString title;
    /// Glyph name from the note icon set.
    std::optional<QString> icon;
    /// Tag colour key of the title.
    std::optional<QString> color;
    QDateTime created;
    QDateTime updated;
    /// Identifiers of the collections the note belongs to.
    QStringList collections;
    /// Tags, with `/` between the parts of a nested tag.
    QStringList tags;
    std::optional<QDateTime> archived;
    std::optional<QDateTime> trashed;
    std::optional<GeoLocation> location;
    /// Fields this build does not know, kept so that rewriting a note does not lose them.
    QJsonObject extra;

    bool operator==(const NoteMeta &) const = default;
};

/// A note: its metadata and the HTML of its body (the content of `<article class="jotter-body">`).
struct Note
{
    NoteMeta meta;
    QString body;

    bool operator==(const Note &) const = default;
};

/// Creates an empty note with a new identifier and the current time.
Note makeNote();

/// Parses a note file.
///
/// @param data The file's contents.
/// @param path The file, for errors; its base name must equal the note's identifier.
/// @return The note, or Malformed (with the reason in the detail) or NewerFormat.
Result<Note> parseNote(const QByteArray &data, const QString &path);

/// Reads valid metadata and raw body exclusively for explicit, original-preserving normalisation.
/// The caller must never save the result over the source file.
Result<Note> parseNoteForNormalization(const QByteArray &data, const QString &path);

/// Serialises a note into a complete HTML document: the metadata block, a generated header
/// with the title and tags for reading in a browser, a generated style sheet and the body.
///
/// @param note The note.
/// @return The file's contents.
QByteArray serializeNote(const Note &note);

/// Builds the readable document of a note for an HTML export: the generated
/// header, style sheet and body of serializeNote(), without the metadata block.
///
/// @param meta The note's metadata, for the title and tags.
/// @param body The body HTML, with the addresses of the export.
/// @param subtitle A line shown under the title, such as the updated date; empty for none.
/// @return The document in UTF-8.
QByteArray exportNoteDocument(const NoteMeta &meta, QStringView body, const QString &subtitle);

} // namespace core
