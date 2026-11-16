#pragma once

#include <QLatin1StringView>

/// Drag and drop data types inside Jotter.
namespace mime {

/// Note identifiers, one per line, dragged from the note list.
inline constexpr QLatin1StringView Notes("application/x-jotter-notes");

/// A favourite's view key (`collection:<id>` or `favorite:<id>`), dragged to reorder favourites.
inline constexpr QLatin1StringView Favorite("application/x-jotter-favorite");

/// A collection identifier, dragged within the sidebar.
inline constexpr QLatin1StringView Collection("application/x-jotter-collection");

} // namespace mime
