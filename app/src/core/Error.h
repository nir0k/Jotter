#pragma once

#include <QString>

#include <expected>

namespace core {

/// The kinds of expected failures of library operations.
enum class ErrorCode {
    /// Reading or writing a file or directory failed.
    Io,
    /// A file or directory that must exist is missing.
    NotFound,
    /// A file does not follow the library format.
    Malformed,
    /// A file was written by a newer version of Jotter.
    NewerFormat,
    /// A new library was requested in a folder that already holds files.
    NotEmpty,
    /// A change was requested in a library opened read-only.
    ReadOnly,
    /// A file is larger than a note accepts.
    TooLarge,
    /// A file a note does not accept: not a regular file, or not an image where one is required.
    Refused,
    /// Another device holds the lock of the sync storage.
    Locked,
    /// The sync storage holds another library.
    OtherLibrary,
    /// The sync storage could not be reached or the connection was lost; worth retrying.
    Network,
    /// The sync storage refused the sign-in.
    SignIn,
    /// The sync storage's host key is unknown or changed and must be confirmed in Settings.
    HostKey,
    /// A concurrent local change requires a bounded retry instead of overwriting newer data.
    Deferred,
    /// A sync run was stopped, as the library was disconnected or closed.
    Canceled,
};

/// An expected failure: what went wrong, the file it concerns and an English detail for logs.
struct Error
{
    ErrorCode code = ErrorCode::Io;
    QString path;
    QString detail;
};

/// The result of an operation that may fail with an Error.
template <typename T> using Result = std::expected<T, Error>;

/// Creates the error value of a failed Result.
inline std::unexpected<Error> fail(ErrorCode code, QString path, QString detail = {})
{
    return std::unexpected(
        Error{.code = code, .path = std::move(path), .detail = std::move(detail)});
}

/// Returns a localized, human-readable description of an error for the interface.
QString describe(const Error &error);

} // namespace core
