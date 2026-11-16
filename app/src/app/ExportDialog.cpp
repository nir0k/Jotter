#include "app/ExportDialog.h"

#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <array>

ExportDialog::ExportDialog(core::Library library, QStringList noteIds, QWidget *parent)
    : QDialog(parent)
    , m_library(std::move(library))
    , m_noteIds(std::move(noteIds))
    , m_format(new ui::SegmentedControl(this))
    , m_pageSizeLabel(new QLabel(tr("Page size"), this))
    , m_pageSize(new ui::SegmentedControl(this))
    , m_progress(new QProgressBar(this))
    , m_error(new ui::Notice(this))
    , m_export(new QPushButton(tr("Export"), this))
{
    using editor::ExportFormat;
    setWindowTitle(tr("Export %n note(s)", nullptr, static_cast<int>(m_noteIds.size())));
    const ui::Theme &theme = *ui::Theme::current();
    m_format->addOption(tr("PDF"), static_cast<int>(ExportFormat::Pdf));
    m_format->addOption(tr("HTML"), static_cast<int>(ExportFormat::Html));
    m_format->addOption(tr("JPG"), static_cast<int>(ExportFormat::Jpg));
    m_format->setValue(static_cast<int>(ExportFormat::Pdf));
    m_pageSize->addOption(tr("A4"), static_cast<int>(QPageSize::A4));
    m_pageSize->addOption(tr("Letter"), static_cast<int>(QPageSize::Letter));
    m_pageSize->setValue(static_cast<int>(defaultPageSize(QLocale::system())));
    m_progress->setTextVisible(false);
    m_progress->hide();
    m_error->hide();

    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(theme.px(0.6));
    form->addRow(tr("Format"), m_format);
    form->addRow(m_pageSizeLabel, m_pageSize);

    auto *cancel = new QPushButton(tr("Cancel"), this);
    m_export->setProperty("role", QStringLiteral("primary"));
    m_export->setDefault(true);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(m_export);

    auto *layout = new QVBoxLayout(this);
    const int pad = theme.px(1.0);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(theme.px(0.75));
    layout->addLayout(form);
    layout->addWidget(m_progress);
    layout->addWidget(m_error);
    layout->addLayout(buttons);
    setMinimumWidth(theme.px(24));

    connect(m_format, &ui::SegmentedControl::changed, this, &ExportDialog::updatePageSize);
    connect(m_export, &QPushButton::clicked, this, &ExportDialog::start);
    connect(cancel, &QPushButton::clicked, this, &ExportDialog::reject);
    connect(&m_running, &QFutureWatcherBase::progressRangeChanged, m_progress,
            &QProgressBar::setRange);
    connect(&m_running, &QFutureWatcherBase::progressValueChanged, m_progress,
            &QProgressBar::setValue);
    connect(&m_running, &QFutureWatcherBase::finished, this, [this] {
        if (m_running.isCanceled() || m_running.future().resultCount() == 0) {
            setRunning(false);
            return;
        }
        const core::Result<editor::ExportResult> result = m_running.result();
        if (result) {
            save(*result);
        } else {
            fail(core::describe(result.error()));
        }
    });
    connect(&m_saving, &QFutureWatcherBase::finished, this, [this] {
        const core::Result<void> saved = m_saving.result();
        if (saved) {
            accept();
        } else {
            fail(core::describe(saved.error()));
        }
    });
    updatePageSize();
}

ExportDialog::~ExportDialog()
{
    // The export stops between pages; a saved file is only replaced once it is complete.
    m_running.cancel();
    m_running.waitForFinished();
    m_saving.waitForFinished();
}

void ExportDialog::reject()
{
    if (m_running.isRunning()) {
        m_running.cancel();
        return;
    }
    if (!m_saving.isRunning()) {
        QDialog::reject();
    }
}

void ExportDialog::start()
{
    m_error->hide();
    setRunning(true);
    const editor::ExportRequest request{
        .library = m_library,
        .noteIds = m_noteIds,
        .format = static_cast<editor::ExportFormat>(m_format->value().toInt()),
        .pageSize = static_cast<QPageSize::PageSizeId>(m_pageSize->value().toInt())};
    m_running.setFuture(QtConcurrent::run(editor::exportNotes, request));
}

void ExportDialog::save(const editor::ExportResult &result)
{
    const QString suffix = QFileInfo(result.fileName).suffix();
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save export"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + u'/' +
            result.fileName,
        QStringLiteral("%1 (*.%2)").arg(suffix.toUpper(), suffix));
    if (path.isEmpty()) {
        QThreadPool::globalInstance()->start(
            [temporary = result.temporaryPath] { QFile::remove(temporary); });
        setRunning(false);
        return;
    }
    m_savedName = QFileInfo(path).fileName();
    // Saving copies the file; its progress is not counted.
    m_progress->setRange(0, 0);
    m_saving.setFuture(QtConcurrent::run(editor::saveExport, result.temporaryPath, path));
}

void ExportDialog::fail(const QString &message)
{
    setRunning(false);
    m_error->showNotice(ui::Notice::Kind::Error, message);
}

void ExportDialog::setRunning(bool running)
{
    m_format->setEnabled(!running);
    m_pageSize->setEnabled(!running);
    m_export->setEnabled(!running);
    m_progress->setRange(0, static_cast<int>(m_noteIds.size()));
    m_progress->setValue(0);
    m_progress->setVisible(running);
}

void ExportDialog::updatePageSize()
{
    const bool pdf = m_format->value().toInt() == static_cast<int>(editor::ExportFormat::Pdf);
    m_pageSizeLabel->setVisible(pdf);
    m_pageSize->setVisible(pdf);
}

QPageSize::PageSizeId defaultPageSize(const QLocale &locale)
{
    // The regions where Letter is the usual paper, as in the C library's paper settings.
    static constexpr std::array LetterRegions{
        QLocale::UnitedStates, QLocale::Canada,     QLocale::Mexico,     QLocale::Guatemala,
        QLocale::Belize,       QLocale::ElSalvador, QLocale::Honduras,   QLocale::Nicaragua,
        QLocale::CostaRica,    QLocale::Panama,     QLocale::PuertoRico, QLocale::DominicanRepublic,
        QLocale::Chile,        QLocale::Colombia,   QLocale::Venezuela,  QLocale::Philippines};
    return std::ranges::contains(LetterRegions, locale.territory()) ? QPageSize::Letter
                                                                    : QPageSize::A4;
}
