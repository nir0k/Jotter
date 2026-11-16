#include "app/DownloadDialog.h"

#include "app/SyncConnectionForm.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QtConcurrentRun>

DownloadDialog::DownloadDialog(QWidget *parent)
    : QDialog(parent)
    , m_title(new QLabel(this))
    , m_text(new QLabel(this))
    , m_form(new SyncConnectionForm(this))
    , m_locationLabel(new QLabel(this))
    , m_location(new QLineEdit(this))
    , m_browse(new QPushButton(this))
    , m_progress(new QLabel(this))
    , m_notice(new ui::Notice(this))
    , m_cancel(new QPushButton(this))
    , m_download(new QPushButton(this))
{
    const ui::Theme &theme = *ui::Theme::current();
    ui::setTextRole(m_title, ui::TextRole::PageTitle);
    ui::setTextRole(m_text, ui::TextRole::Muted);
    ui::setTextRole(m_locationLabel, ui::TextRole::Label);
    ui::setTextRole(m_progress, ui::TextRole::Muted);
    m_text->setWordWrap(true);
    m_location->setText(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                        QStringLiteral("/Jotter"));
    m_download->setProperty("role", QStringLiteral("primary"));
    m_download->setDefault(true);
    m_notice->hide();
    m_progress->hide();

    auto *locationRow = new QHBoxLayout;
    locationRow->addWidget(m_location, 1);
    locationRow->addWidget(m_browse);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_download);

    auto *layout = new QVBoxLayout(this);
    const int pad = theme.px(2.0);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(theme.px(0.5));
    layout->addWidget(m_title);
    layout->addWidget(m_text);
    layout->addSpacing(theme.px(0.5));
    layout->addWidget(m_form);
    layout->addSpacing(theme.px(0.5));
    layout->addWidget(m_locationLabel);
    layout->addLayout(locationRow);
    layout->addWidget(m_progress);
    layout->addWidget(m_notice);
    layout->addSpacing(theme.px(1.0));
    layout->addLayout(buttons);
    setMinimumWidth(theme.px(32));

    connect(m_browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getExistingDirectory(
            this, tr("Where to keep the library"), QFileInfo(m_location->text()).absolutePath());
        if (!path.isEmpty()) {
            m_location->setText(QDir::toNativeSeparators(path));
        }
    });
    connect(m_cancel, &QPushButton::clicked, this, &DownloadDialog::reject);
    connect(m_download, &QPushButton::clicked, this, &DownloadDialog::startDownload);
    connect(m_form, &SyncConnectionForm::checkingChanged, this, &DownloadDialog::setBusy);
    connect(m_form, &SyncConnectionForm::checked, this,
            [this](const std::shared_ptr<core::SftpStorage> &storage) {
                m_credentials = m_form->credentials();
                download(storage);
            });
    connect(&m_run, &QFutureWatcherBase::finished, this, &DownloadDialog::downloaded);
    retranslate();
}

DownloadDialog::~DownloadDialog()
{
    m_run.waitForFinished();
}

void DownloadDialog::startDownload()
{
    const QDir folder(QDir::cleanPath(m_location->text().trimmed()));
    if (folder.exists() &&
        !folder.isEmpty(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)) {
        m_notice->showNotice(
            ui::Notice::Kind::Error,
            core::describe(
                {.code = core::ErrorCode::NotEmpty, .path = folder.path(), .detail = {}}));
        return;
    }
    m_notice->hide();
    // The storage must hold a library before anything is written here.
    m_form->check([](core::SftpStorage &storage) -> core::Result<void> {
        if (auto id = core::remoteLibraryId(storage); !id) {
            return std::unexpected(id.error());
        }
        return {};
    });
}

void DownloadDialog::download(const std::shared_ptr<core::SftpStorage> &storage)
{
    setBusy(true);
    m_form->setFieldsEnabled(false);
    m_progress->setText(tr("Downloading…"));
    m_progress->show();
    const QString root = QDir::cleanPath(m_location->text().trimmed());
    m_run.setFuture(QtConcurrent::run([this, storage, root]() -> core::Result<QString> {
        core::SyncOptions options;
        options.progress = [this](int done, int total) {
            QMetaObject::invokeMethod(
                this,
                [this, done, total] {
                    m_progress->setText(tr("Downloading %1 of %2 files").arg(done).arg(total));
                },
                Qt::QueuedConnection);
        };
        return core::downloadLibrary(root, *storage, options);
    }));
}

void DownloadDialog::downloaded()
{
    const core::Result<QString> id = m_run.result();
    setBusy(false);
    m_form->setFieldsEnabled(true);
    m_progress->hide();
    if (!id) {
        m_notice->showNotice(ui::Notice::Kind::Error, core::describe(id.error()));
        m_notice->setToolTip(id.error().detail);
        return;
    }
    m_result = DownloadedLibrary{.root = QDir::cleanPath(m_location->text().trimmed()),
                                 .libraryId = *id,
                                 .connection = m_form->connection(),
                                 .credentials = std::exchange(m_credentials, {})};
    accept();
}

void DownloadDialog::setBusy(bool busy)
{
    m_busy = busy || m_run.isRunning();
    m_download->setEnabled(!m_busy);
    m_cancel->setEnabled(!m_busy);
    m_location->setEnabled(!m_busy);
    m_browse->setEnabled(!m_busy);
}

void DownloadDialog::reject()
{
    // Escape does not cancel while the connection is checked or the library downloads.
    if (!m_busy) {
        QDialog::reject();
    }
}

void DownloadDialog::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QDialog::changeEvent(event);
}

void DownloadDialog::retranslate()
{
    setWindowTitle(tr("Download from SFTP"));
    m_title->setText(tr("Download from SFTP"));
    m_text->setText(tr("Download a library that another device keeps on your server. It opens "
                       "here already connected and synchronises from then on."));
    m_locationLabel->setText(tr("Where to keep the library"));
    m_browse->setText(tr("Choose…"));
    m_cancel->setText(tr("Cancel"));
    m_download->setText(tr("Download"));
}
