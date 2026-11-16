#include "app/SyncConnectionForm.h"

#include "app/RemoteFolderDialog.h"
#include "ui/Popups.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>

namespace {

/// The largest key file imported; private SSH keys are a few kilobytes at most.
constexpr qint64 MaxKeyBytes = qint64{64} * 1024;

} // namespace

bool confirmHostKey(QWidget *parent, const QString &location, const core::HostKey &key)
{
    if (key.state == core::HostKey::State::Changed) {
        return ui::confirmDestructive(
            parent, SyncConnectionForm::tr("The server's identity has changed"),
            SyncConnectionForm::tr(
                "The key of %1 is not the one Jotter recorded. This happens when the server was "
                "reinstalled, but also when someone intercepts the connection. Trust the new key "
                "only when you know why it changed.\n\nRecorded key: %2\nNew %3 key: %4")
                .arg(location, key.knownFingerprint, key.type, key.fingerprint),
            SyncConnectionForm::tr("Trust new key"));
    }
    QMessageBox question(
        QMessageBox::Question, SyncConnectionForm::tr("Trust this server?"),
        SyncConnectionForm::tr("Jotter has not connected to %1 before. Compare this fingerprint "
                               "with the one of the server before you trust it.\n\n%2 key: %3")
            .arg(location, key.type, key.fingerprint),
        QMessageBox::NoButton, parent);
    QPushButton *trust =
        question.addButton(SyncConnectionForm::tr("Trust"), QMessageBox::AcceptRole);
    trust->setProperty("role", QStringLiteral("primary"));
    question.addButton(SyncConnectionForm::tr("Cancel"), QMessageBox::RejectRole);
    question.setDefaultButton(trust);
    question.exec();
    return question.clickedButton() == trust;
}

SyncConnectionForm::SyncConnectionForm(QWidget *parent)
    : QWidget(parent)
    , m_hostLabel(new QLabel(this))
    , m_host(new QLineEdit(this))
    , m_portLabel(new QLabel(this))
    , m_port(new QLineEdit(this))
    , m_userLabel(new QLabel(this))
    , m_user(new QLineEdit(this))
    , m_folderLabel(new QLabel(this))
    , m_folderRow(new QWidget(this))
    , m_folder(new QLineEdit(this))
    , m_browseFolder(new QPushButton(this))
    , m_methodLabel(new QLabel(this))
    , m_method(new ui::SegmentedControl(this))
    , m_keyLabel(new QLabel(this))
    , m_keyRow(new QWidget(this))
    , m_keyState(new QLabel(this))
    , m_chooseKey(new QPushButton(this))
    , m_secretLabel(new QLabel(this))
    , m_secret(new QLineEdit(this))
    , m_hint(new QLabel(this))
    , m_notice(new ui::Notice(this))
{
    const ui::Theme &theme = *ui::Theme::current();
    for (QLabel *label : {m_hostLabel, m_portLabel, m_userLabel, m_folderLabel, m_methodLabel,
                          m_keyLabel, m_secretLabel}) {
        ui::setTextRole(label, ui::TextRole::Label);
    }
    ui::setTextRole(m_hint, ui::TextRole::Hint);
    m_hint->setWordWrap(true);
    m_port->setValidator(new QIntValidator(1, 65535, m_port));
    m_port->setMaximumWidth(theme.px(6));
    m_secret->setEchoMode(QLineEdit::Password);
    m_method->addOption({}, static_cast<int>(core::SftpConnection::SignIn::Key));
    m_method->addOption({}, static_cast<int>(core::SftpConnection::SignIn::Password));
    m_method->setValue(static_cast<int>(core::SftpConnection::SignIn::Key));
    m_notice->hide();

    auto *folderLayout = new QHBoxLayout(m_folderRow);
    folderLayout->setContentsMargins(0, 0, 0, 0);
    folderLayout->addWidget(m_folder, 1);
    folderLayout->addWidget(m_browseFolder);

    auto *keyLayout = new QHBoxLayout(m_keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->addWidget(m_keyState, 1);
    keyLayout->addWidget(m_chooseKey);

    auto *server = new QHBoxLayout;
    auto *hostColumn = new QVBoxLayout;
    hostColumn->addWidget(m_hostLabel);
    hostColumn->addWidget(m_host);
    auto *portColumn = new QVBoxLayout;
    portColumn->addWidget(m_portLabel);
    portColumn->addWidget(m_port);
    server->addLayout(hostColumn, 1);
    server->addLayout(portColumn);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme.px(0.4));
    layout->addLayout(server);
    for (QWidget *widget :
         {static_cast<QWidget *>(m_userLabel), static_cast<QWidget *>(m_user),
          static_cast<QWidget *>(m_folderLabel), m_folderRow, static_cast<QWidget *>(m_methodLabel),
          ui::settingsRow({m_method}), static_cast<QWidget *>(m_keyLabel), m_keyRow,
          static_cast<QWidget *>(m_secretLabel), static_cast<QWidget *>(m_secret),
          static_cast<QWidget *>(m_hint), static_cast<QWidget *>(m_notice)}) {
        layout->addWidget(widget);
    }

    connect(m_method, &ui::SegmentedControl::changed, this, &SyncConnectionForm::updateMethod);
    connect(m_chooseKey, &QPushButton::clicked, this, &SyncConnectionForm::chooseKey);
    connect(m_browseFolder, &QPushButton::clicked, this, &SyncConnectionForm::browse);
    connect(&m_connect, &QFutureWatcherBase::finished, this, &SyncConnectionForm::connected);
    connect(&m_finish, &QFutureWatcherBase::finished, this, &SyncConnectionForm::finished);
    retranslate();
    updateMethod();
}

SyncConnectionForm::~SyncConnectionForm()
{
    // The workers hold their own reference to the storage and none to the form; waiting keeps
    // a connection from outliving the window.
    m_connect.waitForFinished();
    m_finish.waitForFinished();
}

core::SftpConnection SyncConnectionForm::connection() const
{
    const auto method = static_cast<core::SftpConnection::SignIn>(m_method->value().toInt());
    return {.host = m_host->text().trimmed(),
            .port = m_port->text().toInt(),
            .user = m_user->text().trimmed(),
            .folder = m_folder->text().trimmed(),
            .signIn = method};
}

void SyncConnectionForm::setConnection(const core::SftpConnection &connection)
{
    m_host->setText(connection.host);
    m_port->setText(connection.port > 0 ? QString::number(connection.port) : QString());
    m_user->setText(connection.user);
    m_folder->setText(connection.folder);
    m_method->setValue(static_cast<int>(connection.signIn));
    m_key.clear();
    m_keyName.clear();
    m_secret->clear();
    m_notice->hide();
    updateMethod();
}

core::Credentials SyncConnectionForm::credentials() const
{
    return {.secret = m_secret->text(),
            .key = connection().signIn == core::SftpConnection::SignIn::Key ? m_key : QString()};
}

void SyncConnectionForm::chooseKey()
{
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Choose a private key"), QDir::homePath());
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        showError({.code = core::ErrorCode::Io, .path = path, .detail = file.errorString()});
        return;
    }
    const QByteArray text = file.read(MaxKeyBytes + 1);
    // Both OpenSSH and PEM private keys are armoured text with these markers.
    if (text.size() > MaxKeyBytes || !text.contains("-----BEGIN ") ||
        !text.contains("PRIVATE KEY-----")) {
        m_notice->showNotice(
            ui::Notice::Kind::Error,
            tr("%1 is not a private SSH key.").arg(QDir::toNativeSeparators(path)));
        m_notice->setToolTip({});
        return;
    }
    m_key = QString::fromUtf8(text);
    m_keyName = QFileInfo(path).fileName();
    m_notice->hide();
    updateMethod();
}

void SyncConnectionForm::check(Check check)
{
    if (connection().host.isEmpty() || connection().folder.isEmpty()) {
        m_notice->showNotice(ui::Notice::Kind::Error, tr("Enter the server and the folder on it."));
        return;
    }
    start(false, std::move(check));
}

void SyncConnectionForm::browse()
{
    if (connection().host.isEmpty()) {
        m_notice->showNotice(ui::Notice::Kind::Error, tr("Enter the server first."));
        return;
    }
    start(true, {});
}

void SyncConnectionForm::start(bool browse, Check check)
{
    core::SftpConnection target = connection();
    if (target.signIn == core::SftpConnection::SignIn::Key && m_key.isEmpty()) {
        m_notice->showNotice(ui::Notice::Kind::Error, tr("Choose the private key first."));
        m_notice->setToolTip({});
        return;
    }
    if (browse) {
        // The folder dialog moves anywhere on the server, so its storage starts at the root.
        target.folder = QStringLiteral("/");
    }
    m_browsing = browse;
    m_check = std::move(check);
    m_notice->hide();
    setChecking(true);
    m_connect.setFuture(
        QtConcurrent::run([target]() -> core::Result<std::shared_ptr<core::SftpStorage>> {
            auto storage = core::SftpStorage::connect(target);
            if (!storage) {
                return std::unexpected(storage.error());
            }
            return std::shared_ptr<core::SftpStorage>(std::move(*storage));
        }));
}

void SyncConnectionForm::connected()
{
    auto storage = m_connect.result();
    if (!storage) {
        showError(storage.error());
        setChecking(false);
        return;
    }
    m_storage = *storage;
    const core::HostKey key = m_storage->hostKey();
    const bool trust = key.state != core::HostKey::State::Known;
    if (trust && !confirmHostKey(window(), m_storage->location(), key)) {
        m_storage.reset();
        m_notice->showNotice(ui::Notice::Kind::Warning,
                             tr("The server's key was not trusted, so nothing was saved."));
        setChecking(false);
        return;
    }
    m_finish.setFuture(QtConcurrent::run([storage = m_storage, trust, credentials = credentials(),
                                          check = m_check, browsing = m_browsing,
                                          folder = connection().folder]() -> core::Result<QString> {
        if (trust) {
            if (auto trusted = storage->trustHostKey(); !trusted) {
                return std::unexpected(trusted.error());
            }
        }
        if (auto signedIn = storage->signIn(credentials); !signedIn) {
            return std::unexpected(signedIn.error());
        }
        if (browsing) {
            // The folder typed in is shown first; one that does not exist yet falls back to
            // the home folder.
            auto shown = storage->resolveFolder(folder);
            return shown || folder.isEmpty() ? shown : storage->resolveFolder({});
        }
        if (auto probed = storage->probe(); !probed) {
            return std::unexpected(probed.error());
        }
        if (check) {
            if (auto checkedByCaller = check(*storage); !checkedByCaller) {
                return std::unexpected(checkedByCaller.error());
            }
        }
        return QString();
    }));
}

void SyncConnectionForm::finished()
{
    const core::Result<QString> result = m_finish.result();
    const std::shared_ptr<core::SftpStorage> storage = std::exchange(m_storage, nullptr);
    setChecking(false);
    if (!result) {
        showError(result.error());
        return;
    }
    if (m_browsing) {
        // The key and the secret stay: Check and save still needs them.
        RemoteFolderDialog dialog(storage, *result, window());
        if (dialog.exec() == QDialog::Accepted) {
            m_folder->setText(dialog.folder());
        }
        return;
    }
    Q_EMIT checked(storage);
    // A key or a secret is never kept in the form once it was used.
    m_secret->clear();
    m_key.clear();
    m_keyName.clear();
    updateMethod();
}

void SyncConnectionForm::showError(const core::Error &error)
{
    m_notice->showNotice(ui::Notice::Kind::Error, core::describe(error));
    // The detail is English diagnostic text from the server or libssh, so only a tooltip.
    m_notice->setToolTip(error.detail);
}

void SyncConnectionForm::setFieldsEnabled(bool enabled)
{
    for (QWidget *field :
         {static_cast<QWidget *>(m_host), static_cast<QWidget *>(m_port),
          static_cast<QWidget *>(m_user), m_folderRow, static_cast<QWidget *>(m_method), m_keyRow,
          static_cast<QWidget *>(m_secret)}) {
        field->setEnabled(enabled);
    }
}

void SyncConnectionForm::setChecking(bool checking)
{
    setFieldsEnabled(!checking);
    Q_EMIT checkingChanged(checking);
}

void SyncConnectionForm::updateMethod()
{
    const bool key = connection().signIn == core::SftpConnection::SignIn::Key;
    m_keyLabel->setVisible(key);
    m_keyRow->setVisible(key);
    m_keyState->setText(m_keyName.isEmpty() ? tr("No key chosen") : m_keyName);
    m_secretLabel->setText(key ? tr("Key passphrase") : tr("Password"));
    m_hint->setText(key ? tr("Jotter keeps a copy of the key and its passphrase unencrypted in a "
                             "file only you can read. A key without a passphrase needs none.")
                        : tr("The password is kept unencrypted in a file only you can read."));
}

void SyncConnectionForm::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QWidget::changeEvent(event);
}

void SyncConnectionForm::retranslate()
{
    m_hostLabel->setText(tr("Server"));
    m_host->setPlaceholderText(tr("Host name or address"));
    m_portLabel->setText(tr("Port"));
    m_port->setPlaceholderText(QStringLiteral("22"));
    m_userLabel->setText(tr("User"));
    m_user->setPlaceholderText(tr("Your user name on this computer if empty"));
    m_folderLabel->setText(tr("Folder on the server"));
    m_folder->setPlaceholderText(tr("Relative to your home folder there, or absolute"));
    m_browseFolder->setText(tr("Browse…"));
    m_methodLabel->setText(tr("Sign in with"));
    m_method->setOptionText(static_cast<int>(core::SftpConnection::SignIn::Key), tr("Key"));
    m_method->setOptionText(static_cast<int>(core::SftpConnection::SignIn::Password),
                            tr("Password"));
    m_keyLabel->setText(tr("Private key"));
    m_chooseKey->setText(tr("Choose…"));
    updateMethod();
}
