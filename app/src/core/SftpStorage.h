#pragma once

#include "core/Credentials.h"
#include "core/RemoteStorage.h"

#include <memory>

struct ssh_session_struct;
struct sftp_session_struct;

namespace core {

/// An SFTP connection of a library, kept in the device settings. Files in
/// `~/.ssh` and ssh-agent are never used: the key and the password live in Jotter's
/// credentials.
struct SftpConnection
{
    /// The sign-in method: an imported key with its passphrase, or a password.
    enum class SignIn { Key, Password };

    /// The host name or address.
    QString host;
    /// The port, or 0 for 22.
    int port = 0;
    /// The user name; empty for the local user.
    QString user;
    /// The remote folder: absolute, or relative to the user's home folder on the server.
    QString folder;
    SignIn signIn = SignIn::Key;

    bool operator==(const SftpConnection &) const = default;
};

/// What is known about a server's host key.
struct HostKey
{
    enum class State {
        /// Recorded in Jotter's known hosts file.
        Known,
        /// Not recorded anywhere; the person must accept it.
        Unknown,
        /// Different from the key recorded for the host; never accepted by a sync run.
        Changed,
    };

    State state = State::Unknown;
    /// The key type, such as `ssh-ed25519`.
    QString type;
    /// The SHA256 fingerprint as OpenSSH shows it, `SHA256:…`.
    QString fingerprint;
    /// For a changed key, the fingerprint of the key recorded before.
    QString knownFingerprint;
};

/// A sync storage in a folder on an SFTP server, through libssh.
///
/// Opening it takes the steps of the connection check one by one:
/// connect(), then hostKey() and, once the person accepted the key, trustHostKey(), then
/// signIn(), and probe() to check the folder. A sync run uses open(), which does the same and
/// refuses a key that is not known.
class SftpStorage final : public RemoteStorage
{
public:
    /// Connects to the server and reads its host key, without signing in.
    ///
    /// @param connection The connection.
    /// @return The connection, or Network.
    static Result<std::unique_ptr<SftpStorage>> connect(const SftpConnection &connection);

    /// Connects, checks the host key and signs in, as a sync run needs.
    ///
    /// @param connection The connection.
    /// @param credentials The password, or the key and its passphrase.
    /// @return The storage, or Network / HostKey / SignIn.
    static Result<std::unique_ptr<SftpStorage>> open(const SftpConnection &connection,
                                                     const Credentials &credentials);

    /// Returns Jotter's own known hosts file, `$XDG_DATA_HOME/jotter/known_hosts`.
    static QString knownHostsPath();

    SftpStorage(const SftpStorage &) = delete;
    SftpStorage &operator=(const SftpStorage &) = delete;
    ~SftpStorage() override;

    /// Returns the server's host key and whether it is known.
    const HostKey &hostKey() const { return m_hostKey; }

    /// Records the server's host key in Jotter's known hosts file, replacing a key recorded
    /// there for the host before.
    ///
    /// @return Nothing, or Io.
    Result<void> trustHostKey();

    /// Signs in with the connection's method and starts SFTP; refuses a host key that is not
    /// known.
    ///
    /// @param credentials The password, or the key and its passphrase.
    /// @return Nothing, or HostKey / SignIn / Network.
    Result<void> signIn(const Credentials &credentials);

    /// Writes a probe file into the remote folder through a temporary file and a rename, as
    /// uploads do, and removes it again.
    ///
    /// @return Nothing, or NotFound for a missing folder, or the error of writing.
    Result<void> probe();

    /// Resolves a folder on the server to its absolute path, as the folder of a connection is
    /// written: absolute, or relative to the user's home folder.
    ///
    /// @param folder The folder; empty for the home folder.
    /// @return The absolute path, or NotFound / Network / Io.
    Result<QString> resolveFolder(const QString &folder);

    QString location() const override { return m_location; }
    int timePrecisionMs() const override;
    Result<QList<RemoteEntry>> list(const QString &path) override;
    Result<void> readTo(const QString &path, QIODevice &target) override;
    Result<RemoteEntry> writeFrom(const QString &path, QIODevice &source) override;
    Result<bool> create(const QString &path, const QByteArray &data) override;
    Result<void> rename(const QString &from, const QString &to) override;
    Result<void> remove(const QString &path) override;
    Result<void> removeFolder(const QString &path) override;
    Result<void> makeFolder(const QString &path) override;

private:
    SftpStorage(SftpConnection connection, ssh_session_struct *session);

    /// Returns the server's path of a path relative to the remote folder.
    QByteArray remotePath(const QString &path) const;

    /// Describes the failure of the last SFTP request: Network for a lost connection,
    /// NotFound for a missing file, Io otherwise.
    Error failure(const QString &path) const;

    /// Returns the entry of a file as a listing shows it.
    Result<RemoteEntry> stat(const QString &path);

    SftpConnection m_connection;
    ssh_session_struct *m_session = nullptr;
    sftp_session_struct *m_sftp = nullptr;
    HostKey m_hostKey;
    QString m_location;
    /// The largest reads and writes the server accepts in one request.
    qsizetype m_readChunk = 0;
    qsizetype m_writeChunk = 0;
};

} // namespace core
