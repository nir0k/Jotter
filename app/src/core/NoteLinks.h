#pragma once

#include <QString>
#include <QStringView>

#include <optional>

/// Links between notes, `<a class="note-link" href="<note-id>.html">`, whose
/// relative address also works when the note file is opened in a browser, and web links.
namespace core {

/// Returns the address of a link to a note: `<note-id>.html`.
QString noteLinkAddress(const QString &noteId);

/// Parses a link address written by noteLinkAddress().
///
/// @return The target note's identifier, or no value for any other address.
std::optional<QString> noteLinkTarget(QStringView address);

/// Tells whether an address is a web or mail link a note keeps: an absolute `http`, `https` or
/// `mailto` URL. Other schemes, such as `file` or `javascript`, are refused
/// wherever a link is read or opened.
bool isWebLink(const QString &address);

/// Normalises a typed or pasted address: adds `https://` to a bare domain and `mailto:` to a bare
/// mail address, and refuses what isWebLink() refuses.
///
/// @return The address to store, or an empty string when it is not acceptable.
QString normaliseLink(const QString &address);

/// Renames the links to a note whose text still shows its old title; links
/// with text of their own keep it.
///
/// @param body The body HTML of a note that links to @p noteId.
/// @param noteId The renamed note.
/// @param oldTitle Its title before the change.
/// @param newTitle Its new title.
/// @return The body with the links renamed, or no value when no link showed the old title.
std::optional<QString> renameNoteLinks(const QString &body, const QString &noteId,
                                       const QString &oldTitle, const QString &newTitle);

} // namespace core
