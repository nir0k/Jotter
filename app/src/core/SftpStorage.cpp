#include "core/SftpStorage.h"

#include "core/Files.h"

#include <QFile>
#include <QScopeGuard>
#include <QStandardPaths>

#include <fcntl.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>

namespace core {

namespace {

/// Seconds a connection attempt and every request wait for the server.
constexpr long TimeoutSeconds = 20;

/// Reads and writes kept in flight at once, so large files are not slowed down by a round
/// trip per request.
constexpr qsizetype RequestsInFlight = 16;

/// Rounds of keyboard-interactive questions answered with the password before giving up.
constexpr int MaxQuestionRounds = 5;

/// Returns the SHA256 fingerprint of a key as OpenSSH shows it, `SHA256:…`.
QString fingerprintOf(ssh_key key)
{
    unsigned char *hash = nullptr;
    size_t length = 0;
    if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &length) != 0) {
        return {};
    }
    char *text = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, length);
    QString fingerprint = QString::fromLatin1(text);
    ssh_string_free_char(text);
    ssh_clean_pubkey_hash(&hash);
    return fingerprint;
}

/// Returns the libssh message of the last error of a session.
QString sessionError(ssh_session session)
{
    return QString::fromUtf8(ssh_get_error(session));
}

/// Answers keyboard-interactive questions with the password, for servers that ask for it
/// that way instead of through password authentication.
int keyboardInteractive(ssh_session session, const QByteArray &password)
{
    int result = ssh_userauth_kbdint(session, nullptr, nullptr);
    for (int round = 0; result == SSH_AUTH_INFO && round < MaxQuestionRounds; ++round) {
        const int prompts = ssh_userauth_kbdint_getnprompts(session);
        for (int i = 0; i < prompts; ++i) {
            char echo = 0;
            ssh_userauth_kbdint_getprompt(session, static_cast<unsigned int>(i), &echo);
            // Only hidden answers are secrets; anything shown is left empty.
            ssh_userauth_kbdint_setanswer(session, static_cast<unsigned int>(i),
                                          echo ? "" : password.constData());
        }
        result = ssh_userauth_kbdint(session, nullptr, nullptr);
    }
    return result;
}

/// Finds the key recorded for the session's host in Jotter's known hosts file, hashed host
/// names included, to show it beside a changed key.
///
/// @param session A connected session.
/// @return The fingerprint of the first key recorded for the host, or an empty string.
QString recordedFingerprint(ssh_session session)
{
    char *name = nullptr;
    unsigned int port = 22;
    ssh_options_get(session, SSH_OPTIONS_HOST, &name);
    ssh_options_get_port(session, &port);
    const QByteArray host =
        port == 22 ? QByteArray(name) : '[' + QByteArray(name) + "]:" + QByteArray::number(port);
    ssh_string_free_char(name);
    auto data = readFile(SftpStorage::knownHostsPath());
    if (!data) {
        return {};
    }
    for (const QByteArray &line : data->split('\n')) {
        ssh_knownhosts_entry *entry = nullptr;
        if (ssh_known_hosts_parse_line(host.constData(), line.constData(), &entry) == SSH_OK &&
            entry) {
            QString fingerprint = fingerprintOf(entry->publickey);
            ssh_knownhosts_entry_free(entry);
            return fingerprint;
        }
        if (entry) {
            ssh_knownhosts_entry_free(entry);
        }
    }
    return {};
}

/// Converts the attributes the server sent for a file into an entry.
RemoteEntry entryOf(const QString &name, sftp_attributes attributes)
{
    const quint64 seconds = std::max<quint64>(attributes->mtime, attributes->mtime64);
    return {.name = name,
            .folder = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY,
            .size = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY
                        ? 0
                        : static_cast<qint64>(attributes->size),
            .modified = static_cast<qint64>(seconds) * 1000};
}

} // namespace

SftpStorage::SftpStorage(SftpConnection connection, ssh_session_struct *session)
    : m_connection(std::move(connection))
    , m_session(session)
{}

SftpStorage::~SftpStorage()
{
    if (m_sftp) {
        sftp_free(m_sftp);
    }
    if (ssh_is_connected(m_session)) {
        ssh_disconnect(m_session);
    }
    ssh_free(m_session);
}

QString SftpStorage::knownHostsPath()
{
    return resolvedPath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)) +
           QStringLiteral("/jotter/known_hosts");
}

Result<std::unique_ptr<SftpStorage>> SftpStorage::connect(const SftpConnection &connection)
{
    ssh_session session = ssh_new();
    if (!session) {
        return fail(ErrorCode::Io, connection.host, QStringLiteral("cannot create a session"));
    }
    std::unique_ptr<SftpStorage> storage(new SftpStorage(connection, session));
    const QByteArray host = connection.host.toUtf8();
    ssh_options_set(session, SSH_OPTIONS_HOST, host.constData());
    // libssh reads ~/.ssh/config on connecting unless told not to; the connection alone
    // decides.
    const bool processConfig = false;
    ssh_options_set(session, SSH_OPTIONS_PROCESS_CONFIG, &processConfig);
    if (connection.port > 0) {
        const auto port = static_cast<unsigned int>(connection.port);
        ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    }
    if (!connection.user.isEmpty()) {
        const QByteArray user = connection.user.toUtf8();
        ssh_options_set(session, SSH_OPTIONS_USER, user.constData());
    }
    // Host keys are checked against Jotter's file only, also in place of the system-wide one.
    const QByteArray known = QFile::encodeName(knownHostsPath());
    ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, known.constData());
    ssh_options_set(session, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, known.constData());
    const long timeout = TimeoutSeconds;
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);

    char *user = nullptr;
    char *hostName = nullptr;
    ssh_options_get(session, SSH_OPTIONS_USER, &user);
    ssh_options_get(session, SSH_OPTIONS_HOST, &hostName);
    storage->m_location =
        QStringLiteral("%1@%2:%3")
            .arg(QString::fromUtf8(user), QString::fromUtf8(hostName), connection.folder);
    ssh_string_free_char(user);
    ssh_string_free_char(hostName);

    if (ssh_connect(session) != SSH_OK) {
        return fail(ErrorCode::Network, storage->m_location, sessionError(session));
    }
    ssh_key key = nullptr;
    if (ssh_get_server_publickey(session, &key) != SSH_OK) {
        return fail(ErrorCode::Network, storage->m_location, sessionError(session));
    }
    HostKey &hostKey = storage->m_hostKey;
    hostKey.type = QString::fromLatin1(ssh_key_type_to_char(ssh_key_type(key)));
    hostKey.fingerprint = fingerprintOf(key);
    ssh_key_free(key);
    switch (ssh_session_is_known_server(session)) {
    case SSH_KNOWN_HOSTS_OK:
        hostKey.state = HostKey::State::Known;
        break;
    case SSH_KNOWN_HOSTS_CHANGED:
    case SSH_KNOWN_HOSTS_OTHER:
        hostKey.state = HostKey::State::Changed;
        hostKey.knownFingerprint = recordedFingerprint(session);
        break;
    case SSH_KNOWN_HOSTS_NOT_FOUND:
    case SSH_KNOWN_HOSTS_UNKNOWN:
        hostKey.state = HostKey::State::Unknown;
        break;
    case SSH_KNOWN_HOSTS_ERROR:
        return fail(ErrorCode::Io, knownHostsPath(), sessionError(session));
    }
    return storage;
}

Result<std::unique_ptr<SftpStorage>> SftpStorage::open(const SftpConnection &connection,
                                                       const Credentials &credentials)
{
    auto storage = connect(connection);
    if (!storage) {
        return storage;
    }
    if (auto signedIn = (*storage)->signIn(credentials); !signedIn) {
        return std::unexpected(signedIn.error());
    }
    return storage;
}

int SftpStorage::timePrecisionMs() const
{
    // SFTP version 3, which OpenSSH speaks, carries times in whole seconds.
    return 1000;
}

Result<void> SftpStorage::trustHostKey()
{
    char *line = nullptr;
    if (ssh_session_export_known_hosts_entry(m_session, &line) != SSH_OK) {
        return fail(ErrorCode::Io, knownHostsPath(), sessionError(m_session));
    }
    const QByteArray entry = QByteArray(line).trimmed();
    ssh_string_free_char(line);
    const QByteArray host = entry.left(entry.indexOf(' '));
    // Lines of other hosts stay; an earlier key of this host is replaced.
    QByteArray contents;
    if (auto current = readFile(knownHostsPath())) {
        for (const QByteArray &existing : current->split('\n')) {
            if (!existing.trimmed().isEmpty() && existing.left(existing.indexOf(' ')) != host) {
                contents += existing + '\n';
            }
        }
    }
    contents += entry + '\n';
    if (auto written = writePrivateFile(knownHostsPath(), contents); !written) {
        return written;
    }
    m_hostKey.state = HostKey::State::Known;
    m_hostKey.knownFingerprint.clear();
    return {};
}

Result<void> SftpStorage::signIn(const Credentials &credentials)
{
    if (m_hostKey.state != HostKey::State::Known) {
        return fail(ErrorCode::HostKey, m_location, m_hostKey.fingerprint);
    }
    const QByteArray secretBytes = credentials.secret.toUtf8();
    int result = SSH_AUTH_DENIED;
    switch (m_connection.signIn) {
    case SftpConnection::SignIn::Key: {
        if (credentials.key.isEmpty()) {
            return fail(ErrorCode::SignIn, m_location, QStringLiteral("no key was imported"));
        }
        const QByteArray text = credentials.key.toUtf8();
        ssh_key key = nullptr;
        if (ssh_pki_import_privkey_base64(text.constData(),
                                          secretBytes.isEmpty() ? nullptr : secretBytes.constData(),
                                          nullptr, nullptr, &key) != SSH_OK) {
            return fail(ErrorCode::SignIn, m_location,
                        QStringLiteral("cannot read the key or its passphrase"));
        }
        result = ssh_userauth_publickey(m_session, nullptr, key);
        ssh_key_free(key);
        break;
    }
    case SftpConnection::SignIn::Password:
        result = ssh_userauth_password(m_session, nullptr, secretBytes.constData());
        if (result == SSH_AUTH_DENIED) {
            result = keyboardInteractive(m_session, secretBytes);
        }
        break;
    }
    if (result == SSH_AUTH_ERROR) {
        return fail(ErrorCode::Network, m_location, sessionError(m_session));
    }
    if (result != SSH_AUTH_SUCCESS) {
        return fail(ErrorCode::SignIn, m_location, sessionError(m_session));
    }
    m_sftp = sftp_new(m_session);
    if (!m_sftp || sftp_init(m_sftp) != SSH_OK) {
        return fail(ErrorCode::Network, m_location, sessionError(m_session));
    }
    sftp_limits_t limits = sftp_limits(m_sftp);
    if (!limits) {
        return fail(ErrorCode::Network, m_location, sessionError(m_session));
    }
    m_readChunk = static_cast<qsizetype>(limits->max_read_length);
    m_writeChunk = static_cast<qsizetype>(limits->max_write_length);
    sftp_limits_free(limits);
    return {};
}

Result<void> SftpStorage::probe()
{
    if (auto entries = list(QString()); !entries) {
        return std::unexpected(entries.error());
    }
    const QString name = QStringLiteral(".jotter-probe-") + newId();
    if (auto written = write(name, "Jotter connection check\n"); !written) {
        return std::unexpected(written.error());
    }
    return remove(name);
}

QByteArray SftpStorage::remotePath(const QString &path) const
{
    const QString folder =
        m_connection.folder.isEmpty() ? QStringLiteral(".") : m_connection.folder;
    if (path.isEmpty()) {
        return folder.toUtf8();
    }
    // The root folder `/` already ends with the separator.
    return (folder.endsWith(u'/') ? folder + path : folder + u'/' + path).toUtf8();
}

Result<QString> SftpStorage::resolveFolder(const QString &folder)
{
    const QByteArray path = (folder.isEmpty() ? QStringLiteral(".") : folder).toUtf8();
    char *resolved = sftp_canonicalize_path(m_sftp, path.constData());
    if (!resolved) {
        return std::unexpected(failure(folder));
    }
    QString absolute = QString::fromUtf8(resolved);
    ssh_string_free_char(resolved);
    return absolute;
}

Error SftpStorage::failure(const QString &path) const
{
    const QString where = m_location + u'/' + path;
    if (!ssh_is_connected(m_session)) {
        return {.code = ErrorCode::Network, .path = m_location, .detail = sessionError(m_session)};
    }
    switch (sftp_get_error(m_sftp)) {
    case SSH_FX_NO_SUCH_FILE:
    case SSH_FX_NO_SUCH_PATH:
        return {.code = ErrorCode::NotFound, .path = where, .detail = sessionError(m_session)};
    default:
        return {.code = ErrorCode::Io, .path = where, .detail = sessionError(m_session)};
    }
}

Result<RemoteEntry> SftpStorage::stat(const QString &path)
{
    sftp_attributes attributes = sftp_stat(m_sftp, remotePath(path).constData());
    if (!attributes) {
        return std::unexpected(failure(path));
    }
    RemoteEntry entry = entryOf(path.section(u'/', -1), attributes);
    sftp_attributes_free(attributes);
    return entry;
}

Result<QList<RemoteEntry>> SftpStorage::list(const QString &path)
{
    sftp_dir dir = sftp_opendir(m_sftp, remotePath(path).constData());
    if (!dir) {
        return std::unexpected(failure(path));
    }
    const auto close = qScopeGuard([&] { sftp_closedir(dir); });
    QList<RemoteEntry> entries;
    while (sftp_attributes attributes = sftp_readdir(m_sftp, dir)) {
        const QString name = QString::fromUtf8(attributes->name);
        // Symbolic links and other special files are left out, as the interface promises.
        if (name != u"." && name != u".." &&
            (attributes->type == SSH_FILEXFER_TYPE_REGULAR ||
             attributes->type == SSH_FILEXFER_TYPE_DIRECTORY)) {
            entries.append(entryOf(name, attributes));
        }
        sftp_attributes_free(attributes);
        if (auto checked = checkpoint(); !checked) {
            return std::unexpected(checked.error());
        }
    }
    if (!sftp_dir_eof(dir)) {
        return std::unexpected(failure(path));
    }
    return entries;
}

Result<void> SftpStorage::readTo(const QString &path, QIODevice &target)
{
    sftp_file file = sftp_open(m_sftp, remotePath(path).constData(), O_RDONLY, 0);
    if (!file) {
        return std::unexpected(failure(path));
    }
    const auto close = qScopeGuard([&] { sftp_close(file); });
    sftp_attributes attributes = sftp_fstat(file);
    if (!attributes) {
        return std::unexpected(failure(path));
    }
    // The size only says how many requests to send; nothing is allocated by it, so a wrong
    // size from the server costs no memory.
    const auto size = static_cast<qint64>(attributes->size);
    sftp_attributes_free(attributes);
    // Requests run ahead of the answers, which arrive in their order; each answer is written
    // to the target before the next one is awaited.
    struct Request
    {
        sftp_aio aio;
        qint64 length;
    };
    QList<Request> pending;
    const auto discard = qScopeGuard([&] {
        for (const Request &request : pending) {
            sftp_aio_free(request.aio);
        }
    });
    QByteArray buffer;
    qint64 requested = 0;
    qint64 received = 0;
    while (received < size) {
        if (pending.isEmpty()) {
            if (auto checked = checkpoint(); !checked) {
                return std::unexpected(checked.error());
            }
        }
        const bool fillBatch = pending.isEmpty();
        while (fillBatch && pending.size() < RequestsInFlight && requested < size) {
            sftp_aio aio = nullptr;
            const auto length =
                static_cast<size_t>(std::min<qint64>(m_readChunk, size - requested));
            const ssize_t accepted = sftp_aio_begin_read(file, length, &aio);
            if (accepted <= 0) {
                return std::unexpected(failure(path));
            }
            pending.append({.aio = aio, .length = accepted});
            requested += accepted;
        }
        Request request = pending.takeFirst();
        buffer.resize(request.length);
        const ssize_t got =
            sftp_aio_wait_read(&request.aio, buffer.data(), static_cast<size_t>(request.length));
        if (got < 0) {
            return std::unexpected(failure(path));
        }
        if (target.write(buffer.constData(), got) != got) {
            return fail(ErrorCode::Io, path, target.errorString());
        }
        received += got;
        if (got < request.length) {
            // The file ended early: it is never changed in place, so it was shorter than its
            // size said.
            break;
        }
    }
    return {};
}

Result<RemoteEntry> SftpStorage::writeFrom(const QString &path, QIODevice &source)
{
    const qsizetype slash = path.lastIndexOf(u'/');
    const QString temp =
        (slash < 0 ? QString() : path.first(slash + 1)) + QStringLiteral(".jotter-tmp-") + newId();
    sftp_file file =
        sftp_open(m_sftp, remotePath(temp).constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!file) {
        return std::unexpected(failure(temp));
    }
    // A failure on any path closes and removes the temporary file; a successful rename has
    // already moved it into place, so nothing is left to remove then.
    auto cleanup = qScopeGuard([&] {
        if (file != nullptr) {
            sftp_close(file);
        }
        sftp_unlink(m_sftp, remotePath(temp).constData());
    });
    bool written = true;
    bool sourceFailed = false;
    {
        // Each request keeps its part of the data until the server acknowledged it.
        struct Request
        {
            sftp_aio aio;
            QByteArray data;
        };
        QList<Request> pending;
        const auto discard = qScopeGuard([&] {
            for (const Request &request : pending) {
                sftp_aio_free(request.aio);
            }
        });
        // Data read from the source but not yet accepted by a request.
        QByteArray carry;
        while (written && (!source.atEnd() || !carry.isEmpty() || !pending.isEmpty())) {
            const bool fillBatch = pending.isEmpty();
            if (fillBatch) {
                if (auto checked = checkpoint(); !checked) {
                    return std::unexpected(checked.error());
                }
            }
            while (fillBatch && pending.size() < RequestsInFlight &&
                   (!carry.isEmpty() || !source.atEnd())) {
                if (carry.isEmpty()) {
                    carry = source.read(m_writeChunk);
                    if (carry.isEmpty()) {
                        written = false;
                        sourceFailed = true;
                        break;
                    }
                }
                sftp_aio aio = nullptr;
                const ssize_t accepted = sftp_aio_begin_write(
                    file, carry.constData(), static_cast<size_t>(carry.size()), &aio);
                if (accepted <= 0) {
                    written = false;
                    break;
                }
                // The request keeps the buffer it reads from; a part it did not take goes in
                // the next one.
                pending.append({.aio = aio, .data = carry});
                carry = carry.sliced(accepted);
            }
            if (!pending.isEmpty()) {
                Request request = pending.takeFirst();
                written = written && sftp_aio_wait_write(&request.aio) >= 0;
                if (request.aio) {
                    sftp_aio_free(request.aio);
                }
            }
        }
    }
    const Error error =
        sourceFailed ? Error{.code = ErrorCode::Io, .path = path, .detail = source.errorString()}
                     : failure(temp);
    if (sftp_close(file) != SSH_OK) {
        written = false;
    }
    file = nullptr;
    // The rename uses posix-rename@openssh.com when the server offers it, which replaces the
    // target in one step.
    if (written &&
        sftp_rename(m_sftp, remotePath(temp).constData(), remotePath(path).constData()) == SSH_OK) {
        cleanup.dismiss();
        return stat(path);
    }
    return std::unexpected(written ? failure(path) : error);
}

Result<bool> SftpStorage::create(const QString &path, const QByteArray &data)
{
    sftp_file file =
        sftp_open(m_sftp, remotePath(path).constData(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (!file) {
        const Error error = failure(path);
        // SFTP version 3 reports an existing file only as a failure.
        if (error.code == ErrorCode::Network) {
            return std::unexpected(error);
        }
        if (auto existing = stat(path)) {
            return false;
        }
        return std::unexpected(error);
    }
    const bool written = sftp_write(file, data.constData(), static_cast<size_t>(data.size())) ==
                         static_cast<ssize_t>(data.size());
    const Error error = failure(path);
    if (sftp_close(file) != SSH_OK || !written) {
        return std::unexpected(error);
    }
    return true;
}

Result<void> SftpStorage::rename(const QString &from, const QString &to)
{
    if (sftp_rename(m_sftp, remotePath(from).constData(), remotePath(to).constData()) != SSH_OK) {
        return std::unexpected(failure(from));
    }
    return {};
}

Result<void> SftpStorage::remove(const QString &path)
{
    if (sftp_unlink(m_sftp, remotePath(path).constData()) != SSH_OK) {
        const Error error = failure(path);
        if (error.code != ErrorCode::NotFound) {
            return std::unexpected(error);
        }
    }
    return {};
}

Result<void> SftpStorage::removeFolder(const QString &path)
{
    auto entries = list(path);
    if (!entries) {
        return entries.error().code == ErrorCode::NotFound ? Result<void>()
                                                           : std::unexpected(entries.error());
    }
    for (const RemoteEntry &entry : *entries) {
        const QString child = path.isEmpty() ? entry.name : path + u'/' + entry.name;
        if (auto removed = entry.folder ? removeFolder(child) : remove(child); !removed) {
            return removed;
        }
    }
    if (sftp_rmdir(m_sftp, remotePath(path).constData()) != SSH_OK) {
        return std::unexpected(failure(path));
    }
    return {};
}

Result<void> SftpStorage::makeFolder(const QString &path)
{
    // The remote folder itself is created too, as the empty path names it.
    QString current;
    const QStringList parts = path.split(u'/', Qt::SkipEmptyParts);
    for (qsizetype i = -1; i < parts.size(); ++i) {
        if (i >= 0) {
            current = current.isEmpty() ? parts.at(i) : current + u'/' + parts.at(i);
        }
        if (auto existing = stat(current); existing && existing->folder) {
            continue;
        }
        if (sftp_mkdir(m_sftp, remotePath(current).constData(), 0755) != SSH_OK) {
            const Error error = failure(current);
            // Another device may have created it meanwhile.
            if (auto existing = stat(current); !existing || !existing->folder) {
                return std::unexpected(error);
            }
        }
    }
    return {};
}

} // namespace core
