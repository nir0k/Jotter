#pragma once

#include "core/Error.h"
#include "core/Library.h"

#include <QPageSize>
#include <QPromise>
#include <QString>
#include <QStringList>

/// Export of notes to PDF, HTML and JPG.
namespace editor {

enum class ExportFormat { Pdf, Html, Jpg };

/// What to export and how.
struct ExportRequest
{
    /// The library, copied for the worker thread.
    core::Library library;
    /// The notes, in the order of their files in an archive.
    QStringList noteIds;
    ExportFormat format = ExportFormat::Pdf;
    /// The page size of a PDF.
    QPageSize::PageSizeId pageSize = QPageSize::A4;
};

/// A finished export waiting to be saved.
struct ExportResult
{
    /// The temporary file holding the export; saveExport() moves it into place.
    QString temporaryPath;
    /// The file name to offer: the note's file, or a ZIP archive.
    QString fileName;
};

/// Exports notes on a worker thread. Each note becomes one file named after its title (duplicates
/// numbered); several notes, or one note whose export makes several files, are packed into a ZIP
/// archive. PDF and JPG are drawn as the editor shows the note, in the light Warm palette, with
/// the title and the updated date at the top.
///
/// Progress counts the notes done. When the export is cancelled it stops between notes or pages,
/// removes its temporary file and reports no result.
///
/// @param promise Receives the progress and the result: the temporary file and the name to
///        offer, or the error of a note that cannot be read (NotFound, Malformed…) or of a file
///        that cannot be written (Io).
/// @param request What to export.
void exportNotes(QPromise<core::Result<ExportResult>> &promise, const ExportRequest &request);

/// Saves a finished export where the person chose, replacing the file there only once it is
/// complete. The temporary file is removed either way: a failed save is tried again with a new
/// export. It runs on a worker thread.
///
/// @param temporaryPath The temporary file of exportNotes().
/// @param target The file chosen in the save dialog.
/// @return Nothing, or Io with the target's path.
core::Result<void> saveExport(const QString &temporaryPath, const QString &target);

} // namespace editor
