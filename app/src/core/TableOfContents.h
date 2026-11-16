#pragma once

#include "core/Index.h"
#include "core/Library.h"

#include <QByteArray>
#include <QList>

namespace core {

/// Builds the generated table of contents, `index.html`, for reading the
/// library without Jotter: the notes grouped by collection, a note under each of its
/// collections and nested collections as nested headings, then the notes without a collection,
/// then the archive. Each entry links to the note file and shows its updated date and tags.
/// Headings and dates follow the interface language.
///
/// @param notes The notes that are not in the trash, in any order.
/// @param collections The collection tree.
/// @return The document.
QByteArray tableOfContents(const QList<NoteSummary> &notes, const QList<Collection> &collections);

} // namespace core
