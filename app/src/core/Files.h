#pragma once

#include "core/Error.h"

#include <QByteArray>
#include <QDateTime>
#include <QFileDevice>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace core {

/// Returns a new identifier: a lowercase UUID version 7 without braces, which sorts by time.
QString newId();

/// Tells whether @p id is a lowercase UUID without braces, of any version.
bool isValidId(const QString &id);

/// Formats a time as RFC 3339 in UTC with milliseconds (`2026-10-23T10:15:00.000Z`).
QString formatTime(const QDateTime &time);

/// Parses an RFC 3339 time; returns no value for text that is not one.
std::optional<QDateTime> parseTime(const QString &text);

/// Formats a time for people: the full date and the time without seconds or a time zone, in the
/// local time zone and the interface language (`23 October 2026, 13:15`).
QString displayTime(const QDateTime &time);

/// Returns the current time in UTC, truncated to milliseconds as the library stores it.
QDateTime now();

/// Returns inode, size and nanosecond modification/change times, or empty for a missing file.
QString fileRevision(const QString &path);

/// Resolves the symbolic links in a folder the person chose or the desktop named, such as a
/// documents folder on another disk, so that library file operations, which never follow links,
/// can start from it. Components that do not exist yet are kept as they are.
///
/// @param path An absolute or relative path.
/// @return The absolute path without symbolic links in its existing part.
QString resolvedPath(const QString &path);

/// Reads a whole file.
///
/// @param path The file to read.
/// @return The contents, or NotFound / Io.
Result<QByteArray> readFile(const QString &path);

/// Writes a file atomically: readers see the old or the new contents, never a mixture, and a
/// crash leaves the old file intact. Missing parent directories are created.
///
/// @param path The file to write.
/// @param data The new contents.
/// @param permissions The permissions of the file, such as owner-only for secrets; without
///        them a new file gets the default permissions and an existing one keeps its read and
///        write permissions, without the executable bits.
/// @return Nothing, or Io.
Result<void> writeFileAtomic(const QString &path, const QByteArray &data,
                             std::optional<QFileDevice::Permissions> permissions = std::nullopt);

/// Streams a regular source file into QSaveFile; failures retain the previous destination.
/// Source and destination may name the same file without truncating its contents.
Result<void> copyFileAtomic(const QString &source, const QString &target);

/// Writes a file only its owner may read, for secrets and trusted keys: atomically, with mode
/// 0600, in a folder that gets mode 0700.
///
/// @param path The file to write.
/// @param data The new contents.
/// @return Nothing, or Io.
Result<void> writePrivateFile(const QString &path, const QByteArray &data);

/// Copies the fields of @p object whose keys are not in @p known, so that a file written by a
/// newer Jotter keeps fields this build does not understand when it is rewritten.
QJsonObject unknownFields(const QJsonObject &object,
                          std::initializer_list<QLatin1StringView> known);

/// Returns a string as JSON, or null when there is no value.
QJsonValue optionalString(const std::optional<QString> &value);

/// Serialises JSON for library files: two-space indentation and a final newline, so that
/// changes read well in a diff. Object keys come out in sorted order.
QByteArray toJsonText(const QJsonObject &object);

/// Parses a JSON file's contents that must hold an object.
///
/// @param data The file's contents.
/// @param path The file, for the error.
/// @return The object, or Malformed.
Result<QJsonObject> parseJsonObject(const QByteArray &data, const QString &path);

} // namespace core
