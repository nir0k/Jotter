#pragma once

#include "core/Error.h"

#include <QFile>
#include <QStringList>

#include <memory>
#include <optional>

namespace core {

/// An open directory descriptor. Children are always resolved without following symbolic
/// links; retaining the descriptor also anchors operations if a parent is renamed concurrently.
class Directory
{
public:
    /// Opens an absolute or relative directory, optionally creating missing components.
    /// @param path The directory path; every component must be a real directory.
    /// @param create Whether missing components may be created.
    /// @return An anchored handle, or NotFound / Io / Refused.
    static Result<Directory> open(const QString &path, bool create = false);
    /// Opens one child directory without following links.
    /// @param name A single entry name, never a path.
    /// @param create Whether a missing child may be created.
    /// @return Its anchored handle, or NotFound / Io / Refused.
    Result<Directory> child(const QString &name, bool create = false) const;
    /// Lists every entry, reporting enumeration failures rather than returning an empty list.
    Result<QStringList> entries() const;
    /// Reads one regular file, rejecting links, devices and FIFOs.
    /// @param name A single entry name.
    /// @return All bytes, or NotFound / Io / Refused.
    Result<QByteArray> read(const QString &name) const;
    /// Atomically replaces a child file through QSaveFile and an anchored final rename.
    /// @param name A single destination entry name.
    /// @param data The complete replacement contents.
    /// @param permissions Optional permissions applied before any bytes become visible; without
    ///        them an existing file's read and write permissions are kept.
    /// @return Nothing, or Io / Refused; a failure retains the previous destination.
    Result<void> write(const QString &name, const QByteArray &data,
                       std::optional<QFileDevice::Permissions> permissions = std::nullopt) const;
    /// Atomically replaces a child file with the rest of an open device, read a part at a time.
    /// @param name A single destination entry name.
    /// @param source The open device the contents are read from.
    /// @param permissions As for the other write().
    /// @return Nothing, or Io / Refused; a failure retains the previous destination.
    Result<void> write(const QString &name, QIODevice &source,
                       std::optional<QFileDevice::Permissions> permissions = std::nullopt) const;
    /// Renames a child over another one without following links; a failure removes the source,
    /// which is a temporary file of the caller.
    /// @param from The entry to rename.
    /// @param to The new name, replacing an entry of that name.
    /// @return Nothing, or Io / Refused.
    Result<void> rename(const QString &from, const QString &to) const;
    /// Removes a child without following links, optionally descending into real directories.
    /// @param name A single entry name; a missing entry is already removed.
    /// @param recursive Whether real child directories may be emptied.
    /// @return Nothing, or Io / Refused.
    Result<void> remove(const QString &name, bool recursive = false) const;
    /// Returns an anchored path while this descriptor remains alive, for Qt device adapters.
    QString path(const QString &name = {}) const;

private:
    /// Takes ownership of an already opened directory descriptor.
    /// @param descriptor The descriptor.
    /// @param location The path it was opened at, for errors.
    Directory(int descriptor, QString location);
    /// Returns the path of a child as errors name it.
    QString where(const QString &name) const;
    struct Handle;
    std::shared_ptr<Handle> m_descriptor;
    /// The path this directory was opened at; errors name it, never the descriptor's path.
    QString m_location;
};

/// Tells whether @p name is a single directory entry: not empty, not `.` or `..`, without a
/// slash or a NUL character.
bool isEntryName(const QString &name);

/// A path split into its folder and its last entry.
struct PathParts
{
    QString folder;
    QString name;
};

/// Splits an absolute or relative path into its absolute folder and last entry, keeping `.` and
/// `..` as they are: QFileInfo would clean them away, so a `..` that a remote name or a note
/// brought in could leave the library before the anchored operation ever sees it.
///
/// @param path The path; a relative one is taken from the current folder.
/// @return The folder and the last entry, which is empty when the path ends with a slash.
PathParts splitPath(const QString &path);

/// Opens one regular file without following links in its path; NewOnly is required for writes.
Result<std::unique_ptr<QFile>> openFile(const QString &path, QIODevice::OpenMode mode);

/// Publishes an owned staging directory into a missing or empty target without replacing data.
Result<void> publishDirectory(const QString &staging, const QString &target);

/// Creates a directory tree without traversing a symbolic link.
Result<void> makeDirectory(const QString &path);
/// Enumerates a directory safely; failures are never interpreted as absence of entries.
Result<QStringList> listDirectory(const QString &path);
/// Removes a file or directory tree without following symbolic links in any component.
Result<void> removePath(const QString &path, bool recursive = false);

} // namespace core
