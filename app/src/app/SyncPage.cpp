#include "app/SyncPage.h"

#include "app/SyncConnectionForm.h"
#include "app/SyncStatus.h"
#include "core/LibrarySession.h"
#include "core/SyncEngine.h"
#include "core/SyncScheduler.h"
#include "ui/Icons.h"
#include "ui/Popups.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace {

/// The intervals Settings offers, in minutes; 0 is Off.
constexpr std::array<int, 5> Intervals{0, 5, 15, 30, 60};

/// The status line is refreshed this often, so "how long ago" stays true.
constexpr int RefreshMs = 30 * 1000;

} // namespace

SyncPage::SyncPage(QWidget *parent)
    : QWidget(parent)
    , m_cardTitle(new QLabel)
    , m_statusIcon(new QLabel)
    , m_status(new QLabel)
    , m_intro(new QLabel)
    , m_folder(new QLabel)
    , m_notice(new ui::Notice)
    , m_actions(new QWidget)
    , m_syncNow(new QPushButton)
    , m_change(new QPushButton)
    , m_disconnect(new QPushButton)
    , m_form(new SyncConnectionForm)
    , m_formActions(new QWidget)
    , m_save(new QPushButton)
    , m_cancel(new QPushButton)
    , m_optionsTitle(new QLabel)
    , m_afterChanges(new QCheckBox)
    , m_intervalTitle(new QLabel)
    , m_interval(new QComboBox)
    , m_optionsHint(new QLabel)
{
    const ui::Theme &theme = *ui::Theme::current();
    ui::setTextRole(m_intro, ui::TextRole::Muted);
    ui::setTextRole(m_optionsHint, ui::TextRole::Hint);
    m_intro->setWordWrap(true);
    m_optionsHint->setWordWrap(true);
    m_folder->setWordWrap(true);
    m_folder->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_notice->hide();
    m_save->setProperty("role", QStringLiteral("primary"));
    m_syncNow->setIcon(ui::icon(QStringLiteral("Refresh")));
    for (const int minutes : Intervals) {
        m_interval->addItem({}, minutes);
    }

    auto *statusRow = new QWidget;
    auto *statusLayout = new QHBoxLayout(statusRow);
    statusLayout->setContentsMargins(0, 0, 0, 0);
    statusLayout->addWidget(m_statusIcon);
    statusLayout->addWidget(m_status, 1);

    auto *actions = new QHBoxLayout(m_actions);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->addWidget(m_syncNow);
    actions->addWidget(m_change);
    actions->addStretch();
    actions->addWidget(m_disconnect);

    auto *formActions = new QHBoxLayout(m_formActions);
    formActions->setContentsMargins(0, 0, 0, 0);
    formActions->addStretch();
    formActions->addWidget(m_cancel);
    formActions->addWidget(m_save);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme.px(1.0));
    layout->addWidget(ui::settingsSection(
        m_cardTitle, {statusRow, m_intro, m_folder, m_notice, m_actions, m_form, m_formActions}));
    layout->addWidget(ui::settingsSection(
        m_optionsTitle,
        {m_afterChanges, ui::settingsRow({m_intervalTitle, m_interval}), m_optionsHint}));

    connect(m_syncNow, &QPushButton::clicked, this, [this] {
        if (m_scheduler) {
            m_scheduler->syncNow();
        }
    });
    connect(m_change, &QPushButton::clicked, this, &SyncPage::editConnection);
    connect(m_disconnect, &QPushButton::clicked, this, &SyncPage::disconnectLibrary);
    connect(m_save, &QPushButton::clicked, this, &SyncPage::checkAndSave);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        m_form->hide();
        update();
    });
    connect(m_notice, &ui::Notice::actionTriggered, this, &SyncPage::noticeAction);
    connect(m_form, &SyncConnectionForm::checked, this,
            [this](const std::shared_ptr<core::SftpStorage> & /*storage*/) {
                Q_EMIT connectRequested(m_form->connection(), m_form->credentials());
                m_form->hide();
            });
    connect(m_form, &SyncConnectionForm::checkingChanged, this, [this](bool checking) {
        m_save->setEnabled(!checking);
        m_cancel->setEnabled(!checking);
        m_save->setText(checking ? tr("Checking…") : tr("Check and save"));
    });
    connect(m_afterChanges, &QCheckBox::toggled, this, &SyncPage::afterChangesChosen);
    connect(m_interval, &QComboBox::activated, this,
            [this](int index) { Q_EMIT intervalChosen(m_interval->itemData(index).toInt()); });
    auto *refresh = new QTimer(this);
    refresh->start(RefreshMs);
    connect(refresh, &QTimer::timeout, this, [this] {
        if (isVisible()) {
            update();
        }
    });
    retranslate();
}

void SyncPage::setLibrary(core::LibrarySession *session, core::SyncScheduler *scheduler,
                          const std::optional<core::SftpConnection> &connection)
{
    if (m_scheduler) {
        m_scheduler->disconnect(this);
    }
    m_session = session;
    m_scheduler = scheduler;
    m_connection = connection;
    if (scheduler) {
        connect(scheduler, &core::SyncScheduler::changed, this, &SyncPage::update);
    }
    // The fields are for a library without a connection, or after Change.
    m_form->setVisible(!connection);
    if (!connection) {
        m_form->setConnection({});
    }
    update();
}

void SyncPage::setPreferences(bool afterChanges, int intervalMinutes)
{
    const QSignalBlocker blocker(m_afterChanges);
    m_afterChanges->setChecked(afterChanges);
    m_interval->setCurrentIndex(std::max(0, m_interval->findData(intervalMinutes)));
}

void SyncPage::update()
{
    const bool open = m_session && m_scheduler;
    setEnabled(open);
    const bool connected = open && m_connection;
    const bool editing = !m_form->isHidden();
    m_folder->setVisible(connected);
    m_actions->setVisible(connected && !editing);
    m_formActions->setVisible(editing);
    m_cancel->setVisible(connected);
    m_notice->hide();
    const int iconSize = ui::Theme::current()->px(1.0);
    if (!connected) {
        m_status->setText(tr("Not connected"));
        m_statusIcon->setPixmap(ui::icon(QStringLiteral("Cloud")).pixmap(iconSize, iconSize));
        return;
    }
    const SyncStatus status = syncStatus(*m_scheduler);
    m_status->setText(status.text);
    m_statusIcon->setPixmap(ui::icon(status.icon.isEmpty() ? QStringLiteral("Cloud") : status.icon,
                                     status.failed ? ui::IconTone::Error : ui::IconTone::Primary)
                                .pixmap(iconSize, iconSize));
    const core::SftpConnection &connection = *m_connection;
    m_folder->setText(tr("Folder: %1")
                          .arg((connection.user.isEmpty() ? QString() : connection.user + u'@') +
                               connection.host + u':' + connection.folder));
    const bool running = m_scheduler->phase() == core::SyncScheduler::Phase::Running;
    for (QPushButton *button : {m_syncNow, m_change, m_disconnect}) {
        button->setEnabled(!running);
    }
    const auto &error = m_scheduler->error();
    if (!error || editing) {
        return;
    }
    if (m_scheduler->phase() == core::SyncScheduler::Phase::Waiting) {
        m_notice->showNotice(ui::Notice::Kind::Info, core::describe(*error));
    } else if (const auto &key = m_scheduler->hostKey();
               error->code == core::ErrorCode::HostKey && key) {
        if (key->state == core::HostKey::State::Changed) {
            m_notice->showNotice(ui::Notice::Kind::Warning,
                                 tr("The server's key has changed since Jotter recorded it, so "
                                    "synchronisation stopped.\nRecorded key: %1\nNew %2 key: %3")
                                     .arg(key->knownFingerprint, key->type, key->fingerprint),
                                 tr("Trust new key…"));
        } else {
            m_notice->showNotice(ui::Notice::Kind::Warning,
                                 tr("Jotter does not know the server's %1 key %2, so "
                                    "synchronisation stopped.")
                                     .arg(key->type, key->fingerprint),
                                 tr("Trust key…"));
        }
    } else if (error->code == core::ErrorCode::SignIn) {
        m_notice->showNotice(ui::Notice::Kind::Error, core::describe(*error), tr("Change…"));
    } else {
        m_notice->showNotice(ui::Notice::Kind::Error, core::describe(*error), tr("Retry"));
    }
    m_notice->setToolTip(error->detail);
}

void SyncPage::noticeAction()
{
    if (!m_scheduler || !m_scheduler->error()) {
        return;
    }
    const auto &key = m_scheduler->hostKey();
    if (m_scheduler->error()->code == core::ErrorCode::HostKey && key) {
        if (confirmHostKey(window(), m_scheduler->error()->path, *key)) {
            m_scheduler->trustHostKey(key->fingerprint);
        }
    } else if (m_scheduler->error()->code == core::ErrorCode::SignIn) {
        editConnection();
    } else {
        m_scheduler->syncNow();
    }
}

void SyncPage::editConnection()
{
    if (m_connection) {
        m_form->setConnection(*m_connection);
    }
    m_form->show();
    update();
}

void SyncPage::checkAndSave()
{
    if (!m_session) {
        return;
    }
    m_form->check([id = m_session->library().info().id](core::SftpStorage &storage) {
        return core::checkStorageFor(storage, id);
    });
}

void SyncPage::disconnectLibrary()
{
    if (ui::confirmDestructive(window(), tr("Disconnect from the server?"),
                               tr("This device stops synchronising the library and forgets the "
                                  "password. The notes stay here and on the server."),
                               tr("Disconnect"))) {
        Q_EMIT disconnectRequested();
    }
}

void SyncPage::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QWidget::changeEvent(event);
}

void SyncPage::retranslate()
{
    m_cardTitle->setText(tr("SFTP storage"));
    m_intro->setText(tr("Keeps a copy of the library in a folder on your own server. Every "
                        "device connected to that folder synchronises with it, and the files "
                        "there stay readable."));
    m_syncNow->setText(tr("Sync now"));
    m_syncNow->setToolTip(tr("Sync now (F5)"));
    m_change->setText(tr("Change…"));
    m_disconnect->setText(tr("Disconnect"));
    m_save->setText(tr("Check and save"));
    m_cancel->setText(tr("Cancel"));
    m_optionsTitle->setText(tr("When to synchronise"));
    m_afterChanges->setText(tr("30 seconds after changes"));
    m_intervalTitle->setText(tr("Also every"));
    for (int i = 0; i < m_interval->count(); ++i) {
        const int minutes = m_interval->itemData(i).toInt();
        m_interval->setItemText(i, minutes == 0 ? tr("Off") : tr("%n minute(s)", nullptr, minutes));
    }
    m_optionsHint->setText(tr("Jotter also synchronises when it starts and on Sync now (F5)."));
    update();
}
