#include "app/RemoteFolderDialog.h"

#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QCollator>
#include <QEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>

namespace {

/// Returns the path of an absolute folder inside a storage whose folder is the root `/`.
QString inRoot(const QString &absolute)
{
    return absolute == u"/" ? QString() : absolute.sliced(1);
}

/// Returns the absolute path of a folder inside another.
QString childOf(const QString &parent, const QString &name)
{
    return parent.endsWith(u'/') ? parent + name : parent + u'/' + name;
}

/// Returns the folder that holds an absolute folder; the root has none above it.
QString parentOf(const QString &path)
{
    const qsizetype slash = path.lastIndexOf(u'/');
    return slash <= 0 ? QStringLiteral("/") : path.first(slash);
}

} // namespace

RemoteFolderDialog::RemoteFolderDialog(std::shared_ptr<core::SftpStorage> storage,
                                       const QString &start, QWidget *parent)
    : QDialog(parent)
    , m_storage(std::move(storage))
    , m_pathLabel(new QLabel(this))
    , m_up(new QPushButton(this))
    , m_list(new QListWidget(this))
    , m_content(new QLabel(this))
    , m_notice(new ui::Notice(this))
    , m_newFolder(new QPushButton(this))
    , m_cancel(new QPushButton(this))
    , m_choose(new QPushButton(this))
{
    const ui::Theme &theme = *ui::Theme::current();
    ui::setTextRole(m_pathLabel, ui::TextRole::Code);
    ui::setTextRole(m_content, ui::TextRole::Hint);
    m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_pathLabel->setWordWrap(true);
    m_content->setWordWrap(true);
    m_up->setIcon(ui::icon(QStringLiteral("ChevronUp")));
    m_newFolder->setIcon(ui::icon(QStringLiteral("Add")));
    m_list->setIconSize(QSize(theme.px(1.0), theme.px(1.0)));
    m_choose->setProperty("role", QStringLiteral("primary"));
    m_choose->setDefault(true);
    m_notice->hide();

    auto *pathRow = new QHBoxLayout;
    pathRow->addWidget(m_up);
    pathRow->addWidget(m_pathLabel, 1);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_newFolder);
    buttons->addStretch();
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_choose);

    auto *layout = new QVBoxLayout(this);
    const int pad = theme.px(1.5);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(theme.px(0.5));
    layout->addLayout(pathRow);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_content);
    layout->addWidget(m_notice);
    layout->addLayout(buttons);
    resize(theme.px(30), theme.px(28));

    connect(m_up, &QPushButton::clicked, this, [this] { showFolder(parentOf(m_path)); });
    connect(m_list, &QListWidget::itemActivated, this,
            [this](const QListWidgetItem *item) { showFolder(childOf(m_path, item->text())); });
    connect(m_newFolder, &QPushButton::clicked, this, &RemoteFolderDialog::createFolder);
    connect(m_cancel, &QPushButton::clicked, this, &RemoteFolderDialog::reject);
    connect(m_choose, &QPushButton::clicked, this, &RemoteFolderDialog::accept);
    connect(&m_listing, &QFutureWatcherBase::finished, this, &RemoteFolderDialog::listed);
    retranslate();
    m_path = start;
    showFolder(start);
}

RemoteFolderDialog::~RemoteFolderDialog()
{
    m_listing.waitForFinished();
}

void RemoteFolderDialog::showFolder(const QString &path)
{
    m_pending = path;
    setBusy(true);
    m_listing.setFuture(QtConcurrent::run(
        [storage = m_storage, path = inRoot(path)] { return storage->list(path); }));
}

void RemoteFolderDialog::createFolder()
{
    const QString name =
        QInputDialog::getText(this, tr("New folder"), tr("Name of the new folder")).trimmed();
    if (name.isEmpty()) {
        return;
    }
    if (name.contains(u'/') || name == u"." || name == u"..") {
        m_notice->showNotice(ui::Notice::Kind::Error,
                             tr("A folder name cannot contain “/” or be “.” or “..”."));
        return;
    }
    m_pending = childOf(m_path, name);
    setBusy(true);
    m_listing.setFuture(
        QtConcurrent::run([storage = m_storage,
                           path = inRoot(m_pending)]() -> core::Result<QList<core::RemoteEntry>> {
            if (auto made = storage->makeFolder(path); !made) {
                return std::unexpected(made.error());
            }
            return storage->list(path);
        }));
}

void RemoteFolderDialog::listed()
{
    const auto entries = m_listing.result();
    setBusy(false);
    if (!entries) {
        // The folder shown before stays, with the reason the other could not be opened.
        m_notice->showNotice(ui::Notice::Kind::Error, core::describe(entries.error()));
        m_notice->setToolTip(entries.error().detail);
        updateContent(m_entries);
        return;
    }
    m_entries = *entries;
    m_notice->hide();
    m_path = m_pending;
    m_pathLabel->setText(m_path);
    m_up->setEnabled(m_path != u"/");
    QStringList folders;
    for (const core::RemoteEntry &entry : *entries) {
        // Hidden folders hold configuration, not libraries.
        if (entry.folder && !entry.name.startsWith(u'.')) {
            folders.append(entry.name);
        }
    }
    QCollator collator;
    collator.setNumericMode(true);
    std::ranges::sort(folders, collator);
    m_list->clear();
    for (const QString &folder : std::as_const(folders)) {
        m_list->addItem(new QListWidgetItem(ui::icon(QStringLiteral("Folder")), folder));
    }
    updateContent(*entries);
}

void RemoteFolderDialog::updateContent(const QList<core::RemoteEntry> &entries)
{
    const bool library = std::ranges::any_of(entries, [](const core::RemoteEntry &entry) {
        return !entry.folder && entry.name == u"library.json";
    });
    // Jotter's own names start with `.jotter-`: a folder holding only those counts as empty,
    // as connecting to it does.
    const bool empty = std::ranges::all_of(entries, [](const core::RemoteEntry &entry) {
        return entry.name.startsWith(QLatin1StringView(".jotter-"));
    });
    if (library) {
        m_content->setText(tr("This folder holds a Jotter library."));
    } else if (empty) {
        m_content->setText(tr("This folder is empty."));
    } else {
        m_content->setText(tr("This folder holds other files and no Jotter library."));
    }
}

void RemoteFolderDialog::setBusy(bool busy)
{
    for (QWidget *widget : {static_cast<QWidget *>(m_list), static_cast<QWidget *>(m_newFolder),
                            static_cast<QWidget *>(m_choose)}) {
        widget->setEnabled(!busy);
    }
    m_up->setEnabled(!busy && m_path != u"/");
    if (busy) {
        m_content->setText(tr("Loading…"));
    }
}

void RemoteFolderDialog::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QDialog::changeEvent(event);
}

void RemoteFolderDialog::retranslate()
{
    setWindowTitle(tr("Choose a folder on the server"));
    m_up->setToolTip(tr("Up"));
    m_newFolder->setText(tr("New folder…"));
    m_cancel->setText(tr("Cancel"));
    m_choose->setText(tr("Choose"));
}
