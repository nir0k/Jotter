#pragma once

#include "core/Library.h"
#include "core/Note.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

/// The parts of an export that need no drawing: file names and the HTML
/// format. They run on worker threads.
namespace core {

/// Returns the base name of a note's exported file: its title with the characters that file
/// systems or archives refuse replaced by `_`, without leading or trailing spaces and dots, and
/// at most 100 characters long.
///
/// @param title The note's title.
/// @param untitled The name of a note without a usable title, in the interface language.
/// @return The name without an extension.
QString exportBaseName(const QString &title, const QString &untitled);

/// Hands out the base names of the notes of one export, so that no two files share a name: a
/// repeated name becomes `Name (2)`, `Name (3)`… Names are compared without regard to case, as
/// some file systems do.
class ExportNames
{
public:
    /// Returns @p base, or its first numbered variant not handed out yet.
    QString take(const QString &base);

private:
    QSet<QString> m_taken;
};

/// A file of an export: its path inside the export and its contents, either in memory or as a
/// file of the library to copy.
struct ExportFile
{
    QString path;
    QByteArray data;
    /// The library file to copy instead of @p data; empty when the contents are in memory.
    QString source;
};

/// Exports a note as HTML: a self-contained document with the styles inline and the images
/// embedded, and the files of its attachment chips in a `<base>_files/` folder beside it. Links
/// to notes of the same export point to their files, other note links become plain text, and
/// the document holds no scripts.
///
/// @param library The library holding the note's attachments.
/// @param note The note; its title is the one to show.
/// @param base The document's base name, from ExportNames.
/// @param exported The base names of the notes in the export, by note identifier.
/// @param subtitle The line under the title, such as the updated date.
/// @return The document, `<base>.html`, followed by the attachment files it links to.
QList<ExportFile> exportHtml(const Library &library, const Note &note, const QString &base,
                             const QHash<QString, QString> &exported, const QString &subtitle);

} // namespace core
