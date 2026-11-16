#pragma once

#include "core/SftpStorage.h"
#include "core/SyncEngine.h"

#include <QDateTime>
#include <QFutureWatcher>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <memory>
#include <optional>

namespace core {

class LibrarySession;

/// Synchronises the open library in the background: at start, 30
/// seconds after the last change, at an interval and on request. Network failures are retried
/// after growing pauses; sign-in and host key errors wait for the person.
///
/// Runs happen on a worker thread with their own connection and sync state, one at a time; a
/// request during a run starts one more run when it ends.
class SyncScheduler : public QObject
{
    Q_OBJECT

public:
    /// What the scheduler is doing, for the sidebar's footer and Settings → Sync.
    enum class Phase {
        /// Not connected, or between runs after a successful one.
        Idle,
        Running,
        /// Another device holds the lock; the run is tried again shortly.
        Waiting,
        /// The last run failed; error() says why.
        Failed,
    };

    /// @param session The open library; it must outlive the scheduler.
    /// @param parent The QObject parent.
    explicit SyncScheduler(LibrarySession &session, QObject *parent = nullptr);

    /// Waits for a running run, which uses the library.
    ~SyncScheduler() override;

    /// Sets the library's connection and its credentials, and starts a run; no value stops
    /// synchronisation.
    ///
    /// @param connection The SFTP connection, or no value when the library is not connected.
    /// @param credentials The password, or the key and its passphrase.
    void setConnection(const std::optional<SftpConnection> &connection,
                       const Credentials &credentials);

    /// Tells whether the library has a connection.
    bool isConnected() const { return m_connection.has_value(); }

    /// Switches runs 30 seconds after changes on or off.
    void setAfterChanges(bool on);

    /// Sets the minutes between runs; 0 for none.
    void setInterval(int minutes);

    /// Sets when the library was last synchronised, as remembered from an earlier start.
    void setLastSynced(const QDateTime &time);

    /// Starts a run now that lists every remote folder (Sync now, Retry), also after an error
    /// that waits for the person.
    void syncNow();

    /// Trusts a changed or unknown host key of the server when it still has the fingerprint
    /// the person saw, then starts a run (Settings → Sync).
    ///
    /// @param fingerprint The fingerprint shown to the person.
    void trustHostKey(const QString &fingerprint);

    Phase phase() const { return m_phase; }
    QDateTime lastSynced() const { return m_lastSynced; }

    /// The files done and to do in the running run.
    int done() const { return m_done; }
    int total() const { return m_total; }

    /// The error of the last run, when it failed or waits.
    const std::optional<Error> &error() const { return m_error; }

    /// The server's host key, after a run failed because it is unknown or changed.
    const std::optional<HostKey> &hostKey() const { return m_hostKey; }

Q_SIGNALS:
    /// Emitted when the phase, the progress, the error or the time of the last sync changed.
    void changed();

    /// Emitted after a successful run, with what it did.
    void synced(const core::SyncReport &report);

private:
    /// What a run on the worker thread ends with.
    struct Outcome
    {
        Result<SyncReport> report;
        /// The server's host key, when the run stopped because it is not known.
        std::optional<HostKey> hostKey;
        /// The connection generation the run started with.
        int generation = 0;
    };

    /// Starts a run, or marks one as pending while another runs.
    void start(bool fullListing);
    void finish();
    /// Starts the run that follows changes, unless the person must act first.
    void requestAfterChange();
    void setPhase(Phase phase);

    LibrarySession &m_session;
    std::optional<SftpConnection> m_connection;
    Credentials m_credentials;
    bool m_afterChanges = true;
    int m_intervalMinutes = 15;
    QTimer m_changeTimer;
    QTimer m_intervalTimer;
    QTimer m_retryTimer;
    QFutureWatcher<Outcome> m_run;
    /// Counts connection changes; a run's outcome applies only to the connection it used.
    int m_generation = 0;
    /// Asks the current run to stop.
    std::shared_ptr<std::atomic_bool> m_cancel = std::make_shared<std::atomic_bool>(false);
    bool m_pending = false;
    bool m_pendingFull = false;
    /// The fingerprint of a host key the person accepted, for the next run to trust.
    QString m_trust;
    /// Network failures in a row, for the growing pause before the next try.
    int m_failures = 0;
    /// Short retries for concurrent local edits are bounded independently of network backoff.
    int m_deferredRetries = 0;
    /// Whether the last error needs the person (sign-in, host key) before runs go on.
    bool m_waitsForPerson = false;
    Phase m_phase = Phase::Idle;
    QDateTime m_lastSynced;
    int m_done = 0;
    int m_total = 0;
    std::optional<Error> m_error;
    std::optional<HostKey> m_hostKey;
};

} // namespace core
