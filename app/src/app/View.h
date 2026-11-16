#pragma once

#include "core/Index.h"
#include "core/Library.h"

#include <QString>

/// An entry of the sidebar that the note list shows: a fixed view, a collection, a tag, or a
/// favourite note.
struct View
{
    enum class Kind { All, Unfiled, FavoriteNote, Collection, Tag, Archive, Trash };

    Kind kind = Kind::All;
    /// The collection or note identifier, or the tag path.
    QString id;

    bool operator==(const View &) const = default;

    /// Returns a stable key (`collection:<id>`) for saving the last view.
    QString key() const;

    /// Parses a saved key; an unknown one gives All notes.
    static View fromKey(const QString &key);
};

/// Builds the index filter of a view: a collection includes its descendants unless
/// @p subcollections is off, and a favourite note shows the list of favourite notes.
core::NoteFilter filterFor(const View &view, const core::LibraryInfo &info,
                           const QList<core::Collection> &collections, bool subcollections = true);

/// Returns the title of a view for the note list header.
QString titleFor(const View &view, const QList<core::Collection> &collections);
