#pragma once

#include "core/Library.h"
#include "editor/Export.h"

#include <QDialog>
#include <QFutureWatcher>

class QLabel;
class QLocale;
class QProgressBar;
class QPushButton;

namespace ui {
class Notice;
class SegmentedControl;
} // namespace ui

/// Exports notes: chooses the format and, for PDF, the page size, runs the
/// export in the background with its progress and Cancel, then asks where to save the result.
///
/// Cancel stops a running export and keeps the dialog open; a failure is shown inside it with
/// the choices kept; a saved export closes it.
class ExportDialog : public QDialog
{
    Q_OBJECT

public:
    /// @param library The library holding the notes.
    /// @param noteIds The notes to export, in their list order.
    /// @param parent The window the dialog belongs to.
    ExportDialog(core::Library library, QStringList noteIds, QWidget *parent = nullptr);
    ~ExportDialog() override;

    /// Returns the name of the saved file, for the confirmation; empty until it is saved.
    const QString &savedName() const { return m_savedName; }

    /// Cancels a running export instead of closing; closes otherwise.
    void reject() override;

private:
    /// Starts the export with the chosen format.
    void start();

    /// Asks where to save a finished export and saves it there in the background.
    void save(const editor::ExportResult &result);

    /// Shows an error inside the dialog and makes it ready to try again.
    void fail(const QString &message);

    /// Switches between choosing and running: the choices and Export are disabled and the
    /// progress shows while it runs.
    void setRunning(bool running);

    /// Shows the page size only for PDF.
    void updatePageSize();

    core::Library m_library;
    QStringList m_noteIds;
    ui::SegmentedControl *m_format;
    QLabel *m_pageSizeLabel;
    ui::SegmentedControl *m_pageSize;
    QProgressBar *m_progress;
    ui::Notice *m_error;
    QPushButton *m_export;
    QFutureWatcher<core::Result<editor::ExportResult>> m_running;
    QFutureWatcher<core::Result<void>> m_saving;
    QString m_savedName;
};

/// Returns the page size a PDF starts with: Letter where the system's region uses it (the United
/// States, Canada, Mexico, Central America, Puerto Rico, the Dominican Republic, Chile, Colombia,
/// Venezuela and the Philippines), A4 elsewhere.
///
/// @param locale The system locale, whose region decides.
/// @return The page size.
QPageSize::PageSizeId defaultPageSize(const QLocale &locale);
