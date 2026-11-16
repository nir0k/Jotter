#pragma once

#include "core/Library.h"
#include "core/Note.h"
#include "core/Rules.h"

#include <QList>
#include <QStringList>

#include <optional>

namespace core {

/// The result of merging a note changed on two devices.
struct NoteMerge
{
    /// The note both sides keep.
    Note note;
    /// Whether both sides changed the same part of the body. The note then has the remote body
    /// and the local note must be kept as a conflict copy.
    bool conflict = false;
};

/// Merges a note changed locally and remotely against the version both started from.
///
/// Metadata merges field by field: a field changed on one side takes that side, a field
/// changed on both sides keeps the side with the later `updated`, and collections and tags
/// merge as sets. The body merges by top-level blocks; changes to different blocks combine.
/// Without a base (the first connection) notes with different bodies are a
/// conflict, and notes with equal bodies keep the later one.
///
/// @param base The last synchronised version, or no value when there is none.
/// @param local The version in the library.
/// @param remote The version in the sync storage.
/// @return The merged note and whether it is a conflict.
NoteMerge mergeNote(const std::optional<Note> &base, const Note &local, const Note &remote);

/// Merges `library.json`: favourites by entry, tag colours by tag, settings and unknown fields
/// by key, and the tombstones of both sides, dropping expired ones and favourites of deleted
/// notes. Without a base, or when both sides changed the same value, the remote value wins, as
/// these values carry no time of change.
///
/// @param base The last synchronised version, or no value when there is none.
/// @param local The version in the library.
/// @param remote The version in the sync storage.
/// @param now The current time, for expiring tombstones.
/// @return The merged library information, with the remote identifier.
LibraryInfo mergeLibraryInfo(const std::optional<LibraryInfo> &base, const LibraryInfo &local,
                             const LibraryInfo &remote, const QDateTime &now);

/// Merges `collections.json` by collection identifier; a collection changed on both sides keeps
/// the later `updated`, and one changed on one side and deleted on the other is kept. The order
/// is the remote one unless only the local side reordered; collections missing from it follow
/// their predecessor on their own side.
///
/// @param base The last synchronised version; empty when there is none.
/// @param local The version in the library.
/// @param remote The version in the sync storage.
/// @return The merged collections in their order.
QList<Collection> mergeCollections(const QList<Collection> &base, const QList<Collection> &local,
                                   const QList<Collection> &remote);

/// Merges `rules.json` by rule identifier, with the rules of mergeCollections().
///
/// @param base The last synchronised version; empty when there is none.
/// @param local The version in the library.
/// @param remote The version in the sync storage.
/// @return The merged rules in the order they run in.
QList<Rule> mergeRules(const QList<Rule> &base, const QList<Rule> &local,
                       const QList<Rule> &remote);

/// Splits a note body into its top-level blocks (paragraphs, lists, tables, code blocks and the
/// like), without the line breaks between them, the units of the three-way body merge.
///
/// @param body The body HTML.
/// @return The blocks in document order; text outside any element forms blocks of its own.
QStringList bodyBlocks(QStringView body);

} // namespace core
