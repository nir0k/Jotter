#pragma once

#include "core/Error.h"

#include <QString>

namespace core {

/// The sync credentials of a library.
struct Credentials
{
    /// The SFTP password, or the passphrase of the key; empty for none.
    QString secret;
    /// The text of the private key imported into Jotter, for sign-in with a key; empty for a
    /// password.
    QString key;

    bool operator==(const Credentials &) const = default;
};

/// Returns the credentials file, `$XDG_DATA_HOME/jotter/credentials.json`.
QString credentialsPath();

/// Reads the sync credentials of a library.
///
/// @param libraryId The library.
/// @return The credentials, empty when none are stored, or Io / Malformed.
Result<Credentials> readCredentials(const QString &libraryId);

/// Stores or removes the sync credentials of a library. The file is written atomically with
/// mode 0600 in a folder with mode 0700; the credentials are protected by these permissions
/// only.
///
/// @param libraryId The library.
/// @param credentials The credentials; empty ones remove the library's entry.
/// @return Nothing, or Io / Malformed.
Result<void> writeCredentials(const QString &libraryId, const Credentials &credentials);

} // namespace core
