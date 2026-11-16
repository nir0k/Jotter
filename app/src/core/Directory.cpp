#include "core/Directory.h"

#include "core/FileGuard.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QUuid>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace core {

bool isEntryName(const QString &name)
{
    return !name.isEmpty() && name != QLatin1StringView(".") && name != QLatin1StringView("..") &&
           !name.contains(u'/') && !name.contains(QChar::Null);
}

namespace {

/// Captures errno immediately, retaining absence as distinct from an I/O or unsafe-path error.
std::unexpected<Error> systemError(const QString &path)
{
    const int error = errno;
    return fail(error == ENOENT ? ErrorCode::NotFound : ErrorCode::Io, path,
                QString::fromLocal8Bit(std::strerror(error)));
}

} // namespace

/// Shared ownership keeps descriptors alive while an anchored operation uses them.
struct Directory::Handle
{
    int value;
    /// Takes ownership of a descriptor already checked by its caller.
    explicit Handle(int descriptor)
        : value(descriptor)
    {}
    /// Releases the descriptor when the last directory view disappears.
    ~Handle()
    {
        if (value >= 0) {
            ::close(value);
        }
    }
};

Directory::Directory(int descriptor, QString location)
    : m_descriptor(std::make_shared<Handle>(descriptor))
    , m_location(std::move(location))
{}

QString Directory::where(const QString &name) const
{
    return m_location.endsWith(u'/') ? m_location + name : m_location + u'/' + name;
}

Result<Directory> Directory::open(const QString &path, bool create)
{
    auto current = Directory(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC), QStringLiteral("/"));
    if (current.m_descriptor->value < 0) {
        return systemError(path);
    }
    const QString absolute = QDir::isRelativePath(path) ? QDir::currentPath() + u'/' + path : path;
    // Do not clean '..' away: accepting it would weaken the component-by-component boundary.
    for (const QString &part : absolute.split(u'/', Qt::SkipEmptyParts)) {
        auto next = current.child(part, create);
        if (!next) {
            return std::unexpected(next.error());
        }
        current = std::move(*next);
    }
    return current;
}

Result<Directory> Directory::child(const QString &name, bool create) const
{
    if (!isEntryName(name)) {
        return fail(ErrorCode::Refused, where(name), QStringLiteral("invalid directory entry"));
    }
    const QByteArray encoded = QFile::encodeName(name);
    if (create && ::mkdirat(m_descriptor->value, encoded.constData(), 0777) != 0 &&
        errno != EEXIST) {
        return systemError(where(name));
    }
    const int descriptor = ::openat(m_descriptor->value, encoded.constData(),
                                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        return systemError(where(name));
    }
    return Directory(descriptor, where(name));
}

QString Directory::path(const QString &name) const
{
    return QStringLiteral("/proc/self/fd/%1").arg(m_descriptor->value) +
           (name.isEmpty() ? QString() : u'/' + name);
}

Result<QStringList> Directory::entries() const
{
    // A separate open file description prevents shared readdir offsets between copied handles.
    const int descriptor = ::openat(m_descriptor->value, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        return systemError(m_location);
    }
    DIR *stream = ::fdopendir(descriptor);
    if (stream == nullptr) {
        ::close(descriptor);
        return systemError(m_location);
    }
    QStringList result;
    errno = 0;
    while (const dirent *entry = ::readdir(stream)) {
        const QString name = QFile::decodeName(entry->d_name);
        if (name != QLatin1StringView(".") && name != QLatin1StringView("..")) {
            result.append(name);
        }
        errno = 0;
    }
    const int error = errno;
    ::closedir(stream);
    if (error != 0) {
        errno = error;
        return systemError(m_location);
    }
    result.sort();
    return result;
}

Result<QByteArray> Directory::read(const QString &name) const
{
    if (!isEntryName(name)) {
        return fail(ErrorCode::Refused, where(name));
    }
    const QByteArray encoded = QFile::encodeName(name);
    const int descriptor = ::openat(m_descriptor->value, encoded.constData(),
                                    O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) {
        return systemError(where(name));
    }
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        ::close(descriptor);
        return fail(ErrorCode::Io, where(name), QStringLiteral("not a regular file"));
    }
    QFile file;
    if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        return fail(ErrorCode::Io, where(name), file.errorString());
    }
    QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return fail(ErrorCode::Io, where(name), file.errorString());
    }
    return bytes;
}

Result<void> Directory::write(const QString &name, const QByteArray &data,
                              std::optional<QFileDevice::Permissions> permissions) const
{
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    return write(name, buffer, permissions);
}

Result<void> Directory::write(const QString &name, QIODevice &source,
                              std::optional<QFileDevice::Permissions> permissions) const
{
    if (!isEntryName(name)) {
        return fail(ErrorCode::Refused, where(name));
    }
    // QSaveFile follows an existing destination symlink. Commit to a fresh unpredictable
    // sibling first, then renameat over the destination without ever resolving its target.
    const QString temporary =
        QStringLiteral(".jotter-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QByteArray to = QFile::encodeName(name);
    // An existing file keeps its read and write permissions, such as an owner-only note; the
    // replacement is never executable.
    std::optional<mode_t> mode;
    if (!permissions) {
        struct stat status{};
        if (::fstatat(m_descriptor->value, to.constData(), &status, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(status.st_mode)) {
            mode = status.st_mode & 0666;
        }
    }
    QSaveFile file(path(temporary));
    if (!file.open(QIODevice::WriteOnly) || (permissions && !file.setPermissions(*permissions)) ||
        (mode && ::fchmod(file.handle(), *mode) != 0)) {
        return fail(ErrorCode::Io, where(name), file.errorString());
    }
    // The contents are copied a megabyte at a time, so a large file never sits in memory.
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(qint64{1} << 20);
        if (chunk.isEmpty() || file.write(chunk) != chunk.size()) {
            return fail(ErrorCode::Io, where(name),
                        chunk.isEmpty() ? source.errorString() : file.errorString());
        }
    }
    if (!file.commit()) {
        return fail(ErrorCode::Io, where(name), file.errorString());
    }
    return rename(temporary, name);
}

Result<void> Directory::rename(const QString &from, const QString &to) const
{
    if (!isEntryName(from) || !isEntryName(to)) {
        return fail(ErrorCode::Refused, where(to));
    }
    const QByteArray source = QFile::encodeName(from);
    const QByteArray target = QFile::encodeName(to);
    if (::renameat(m_descriptor->value, source.constData(), m_descriptor->value,
                   target.constData()) != 0) {
        const auto error = systemError(where(to));
        ::unlinkat(m_descriptor->value, source.constData(), 0);
        return error;
    }
    return {};
}

Result<void> Directory::remove(const QString &name, bool recursive) const
{
    if (!isEntryName(name)) {
        return fail(ErrorCode::Refused, where(name));
    }
    const QByteArray encoded = QFile::encodeName(name);
    struct stat status{};
    if (::fstatat(m_descriptor->value, encoded.constData(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return {};
        }
        return systemError(where(name));
    }
    if (S_ISDIR(status.st_mode)) {
        if (recursive) {
            auto directory = child(name);
            if (!directory) {
                return std::unexpected(directory.error());
            }
            auto names = directory->entries();
            if (!names) {
                return std::unexpected(names.error());
            }
            for (const QString &entry : *names) {
                if (auto removed = directory->remove(entry, true); !removed) {
                    return removed;
                }
            }
        }
        if (::unlinkat(m_descriptor->value, encoded.constData(), AT_REMOVEDIR) != 0) {
            return systemError(where(name));
        }
    } else if (::unlinkat(m_descriptor->value, encoded.constData(), 0) != 0) {
        return systemError(where(name));
    }
    return {};
}

PathParts splitPath(const QString &path)
{
    const QString absolute = QDir::isRelativePath(path) ? QDir::currentPath() + u'/' + path : path;
    const qsizetype slash = absolute.lastIndexOf(u'/');
    return {.folder = slash == 0 ? QStringLiteral("/") : absolute.first(slash),
            .name = absolute.sliced(slash + 1)};
}

Result<std::unique_ptr<QFile>> openFile(const QString &path, QIODevice::OpenMode mode)
{
    const PathParts info = splitPath(path);
    auto directory = Directory::open(info.folder);
    if (!directory) {
        return std::unexpected(directory.error());
    }
    if (!isEntryName(info.name)) {
        return fail(ErrorCode::Refused, path);
    }
    const bool writing = mode.testFlag(QIODevice::WriteOnly);
    if (writing && !mode.testFlag(QIODevice::NewOnly)) {
        return fail(ErrorCode::Refused, path);
    }
    const int flags =
        O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | (writing ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY);
    const QByteArray anchored = QFile::encodeName(directory->path(info.name));
    const int descriptor = ::open(anchored.constData(), flags, 0644);
    if (descriptor < 0) {
        return systemError(path);
    }
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        ::close(descriptor);
        return fail(ErrorCode::Io, path, QStringLiteral("not a regular file"));
    }
    auto file = std::make_unique<QFile>(path);
    file->setObjectName(path);
    if (!file->open(descriptor, mode, QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        return fail(ErrorCode::Io, path, file->errorString());
    }
    return file;
}

Result<void> publishDirectory(const QString &staging, const QString &target)
{
    const PathParts source = splitPath(staging);
    const PathParts destination = splitPath(target);
    auto from = Directory::open(source.folder);
    auto to = Directory::open(destination.folder);
    if (!from || !to) {
        return std::unexpected(from ? to.error() : from.error());
    }
    // rmdir succeeds only for a real empty directory; it never removes a symlink or data.
    if (auto existing = to->child(destination.name); existing) {
        auto entries = existing->entries();
        if (!entries || !entries->isEmpty()) {
            return entries ? fail(ErrorCode::NotEmpty, target) : std::unexpected(entries.error());
        }
        const QByteArray emptyTarget = QFile::encodeName(to->path(destination.name));
        if (::rmdir(emptyTarget.constData()) != 0) {
            return systemError(target);
        }
    } else if (existing.error().code != ErrorCode::NotFound) {
        return std::unexpected(existing.error());
    }
    const QByteArray fromPath = QFile::encodeName(from->path(source.name));
    const QByteArray toPath = QFile::encodeName(to->path(destination.name));
    if (::syscall(SYS_renameat2, AT_FDCWD, fromPath.constData(), AT_FDCWD, toPath.constData(),
                  RENAME_NOREPLACE) != 0) {
        return systemError(target);
    }
    return {};
}

Result<void> makeDirectory(const QString &path)
{
    auto directory = Directory::open(path, true);
    return directory ? Result<void>() : std::unexpected(directory.error());
}

Result<QStringList> listDirectory(const QString &path)
{
    auto directory = Directory::open(path);
    return directory ? directory->entries() : std::unexpected(directory.error());
}

Result<void> removePath(const QString &path, bool recursive)
{
    const FileGuard guard(path);
    const PathParts file = splitPath(path);
    auto directory = Directory::open(file.folder);
    if (!directory) {
        return directory.error().code == ErrorCode::NotFound ? Result<void>()
                                                             : std::unexpected(directory.error());
    }
    return directory->remove(file.name, recursive);
}

} // namespace core
