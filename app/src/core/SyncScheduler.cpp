#include "core/SyncScheduler.h"

#include "core/LibrarySession.h"
#include "core/SyncState.h"

#include <QtConcurrentRun>

#include <algorithm>

namespace core {

namespace {

/// A run starts this long after the last change.
constexpr int ChangeDelayMs = 30 * 1000;

/// A run that found the lock taken tries again after this long.
constexpr int LockedRetryMs = 30 * 1000;

/// Network failures are retried after 1, 2, 4 and more minutes, up to the interval, or up to
/// this many minutes without one.
constexpr int MaxRetryMinutes = 15;

constexpr int MinuteMs = 60 * 1000;

} // namespace

SyncScheduler::SyncScheduler(LibrarySession &session, QObject *parent)
    : QObject(parent)
    , m_session(session)
{
    m_changeTimer.setSingleShot(true);
    m_changeTimer.setInterval(ChangeDelayMs);
    m_retryTimer.setSingleShot(true);
    for (const QTimer *timer : {&m_changeTimer, &m_retryTimer, &m_intervalTimer}) {
        connect(timer, &QTimer::timeout, this, [this] {
            if (!m_waitsForPerson) {
                start(false);
            }
        });
    }
    connect(&m_run, &QFutureWatcher<Outcome>::finished, this, &SyncScheduler::finish);
    for (const auto signal : {&LibrarySession::notesChanged, &LibrarySession::collectionsChanged,
                              &LibrarySession::infoChanged, &LibrarySession::rulesChanged}) {
        connect(&session, signal, this, &SyncScheduler::requestAfterChange);
    }
    connect(&session, &LibrarySession::noteSaved, this, &SyncScheduler::requestAfterChange);
    setInterval(m_intervalMinutes);
}

SyncScheduler::~SyncScheduler()
{
    // A run stops at its next request, so closing the library does not wait for the transfer.
    m_cancel->store(true);
    m_run.waitForFinished();
}

void SyncScheduler::setConnection(const std::optional<SftpConnection> &connection,
                                  const Credentials &credentials)
{
    // A run still going belongs to the previous connection: it stops at its next request.
    m_cancel->store(true);
    if (!connection) {
        // The state of a disconnected library is removed next; no run may still use it.
        m_run.waitForFinished();
    }
    // Its outcome is ignored as well.
    ++m_generation;
    m_connection = connection;
    m_credentials = credentials;
    m_waitsForPerson = false;
    m_failures = 0;
    m_error.reset();
    m_hostKey.reset();
    m_retryTimer.stop();
    m_changeTimer.stop();
    m_phase = Phase::Idle;
    Q_EMIT changed();
    if (connection) {
        start(false);
    }
}

void SyncScheduler::setAfterChanges(bool on)
{
    m_afterChanges = on;
    if (!on) {
        m_changeTimer.stop();
    }
}

void SyncScheduler::setInterval(int minutes)
{
    m_intervalMinutes = minutes;
    if (minutes > 0) {
        m_intervalTimer.start(minutes * MinuteMs);
    } else {
        m_intervalTimer.stop();
    }
}

void SyncScheduler::setLastSynced(const QDateTime &time)
{
    m_lastSynced = time;
    Q_EMIT changed();
}

void SyncScheduler::syncNow()
{
    m_waitsForPerson = false;
    start(true);
}

void SyncScheduler::trustHostKey(const QString &fingerprint)
{
    m_trust = fingerprint;
    syncNow();
}

void SyncScheduler::requestAfterChange()
{
    if (m_afterChanges && m_connection && !m_waitsForPerson) {
        m_changeTimer.start();
    }
}

void SyncScheduler::setPhase(Phase phase)
{
    m_phase = phase;
    Q_EMIT changed();
}

void SyncScheduler::start(bool fullListing)
{
    if (!m_connection || !m_session.isAvailable()) {
        return;
    }
    if (m_run.isRunning()) {
        m_pending = true;
        m_pendingFull = m_pendingFull || fullListing;
        return;
    }
    m_changeTimer.stop();
    m_retryTimer.stop();
    m_done = 0;
    m_total = 0;
    setPhase(Phase::Running);
    const QString root = m_session.library().root();
    const QString libraryId = m_session.library().info().id;
    const SftpConnection connection = m_connection.value_or(SftpConnection());
    const int generation = m_generation;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    m_run.setFuture(
        QtConcurrent::run([this, root, libraryId, connection, generation, cancel = m_cancel,
                           credentials = m_credentials, trust = std::exchange(m_trust, {}),
                           fullListing]() -> Outcome {
            const auto failed = [generation](const Error &error) {
                return Outcome{
                    .report = std::unexpected(error), .hostKey = {}, .generation = generation};
            };
            auto storage = SftpStorage::connect(connection);
            if (!storage) {
                return failed(storage.error());
            }
            SftpStorage &sftp = **storage;
            // A key the person accepted in Settings is trusted only while the server still has it.
            if (sftp.hostKey().state != HostKey::State::Known && !trust.isEmpty() &&
                sftp.hostKey().fingerprint == trust) {
                if (auto trusted = sftp.trustHostKey(); !trusted) {
                    return failed(trusted.error());
                }
            }
            if (sftp.hostKey().state != HostKey::State::Known) {
                return Outcome{.report = fail(ErrorCode::HostKey, sftp.location()),
                               .hostKey = sftp.hostKey(),
                               .generation = generation};
            }
            if (auto signedIn = sftp.signIn(credentials); !signedIn) {
                return failed(signedIn.error());
            }
            auto state = SyncState::open(SyncState::defaultPath(libraryId));
            if (!state) {
                return failed(state.error());
            }
            SyncOptions options;
            options.fullListing = fullListing;
            options.canceled = [cancel] { return cancel->load(); };
            options.progress = [this](int done, int total) {
                QMetaObject::invokeMethod(
                    this,
                    [this, done, total] {
                        m_done = done;
                        m_total = total;
                        Q_EMIT changed();
                    },
                    Qt::QueuedConnection);
            };
            return Outcome{.report = synchronise(root, sftp, *state, options),
                           .hostKey = {},
                           .generation = generation};
        }));
}

void SyncScheduler::finish()
{
    Outcome outcome = m_run.result();
    if (outcome.generation != m_generation) {
        // The connection changed or was removed during the run: what it reports no longer
        // applies, such as the time of a sync with a disconnected storage.
        if (std::exchange(m_pending, false) && m_connection) {
            start(std::exchange(m_pendingFull, false));
        }
        return;
    }
    m_hostKey = std::move(outcome.hostKey);
    if (outcome.report) {
        m_error.reset();
        m_failures = 0;
        m_waitsForPerson = false;
        if (outcome.report->skipped.isEmpty()) {
            m_deferredRetries = 0;
            m_lastSynced = QDateTime::currentDateTimeUtc();
            setPhase(Phase::Idle);
        } else {
            m_error = Error{.code = ErrorCode::Deferred,
                            .path = m_session.library().root(),
                            .detail = QStringLiteral("Files changed during sync: %1")
                                          .arg(outcome.report->skipped.join(QStringLiteral(", ")))};
            if (++m_deferredRetries <= 3) {
                m_retryTimer.start(5000);
            }
            setPhase(Phase::Failed);
        }
        Q_EMIT synced(*outcome.report);
    } else {
        m_error = outcome.report.error();
        switch (m_error->code) {
        case ErrorCode::Locked:
            m_retryTimer.start(LockedRetryMs);
            setPhase(Phase::Waiting);
            break;
        case ErrorCode::Network: {
            ++m_failures;
            const int cap = m_intervalMinutes > 0 ? m_intervalMinutes : MaxRetryMinutes;
            const int minutes = std::min(1 << std::min(m_failures - 1, 10), cap);
            m_retryTimer.start(minutes * MinuteMs);
            setPhase(Phase::Failed);
            break;
        }
        case ErrorCode::SignIn:
        case ErrorCode::HostKey:
            m_waitsForPerson = true;
            setPhase(Phase::Failed);
            break;
        default:
            setPhase(Phase::Failed);
            break;
        }
    }
    if (m_intervalMinutes > 0) {
        m_intervalTimer.start(m_intervalMinutes * MinuteMs);
    }
    if (std::exchange(m_pending, false) && m_connection) {
        start(std::exchange(m_pendingFull, false));
    }
}

} // namespace core
