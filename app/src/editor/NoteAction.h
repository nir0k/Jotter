#pragma once

namespace editor {

/// Actions on notes offered by the editor's ⋮ menu and by the note list; the
/// main window carries them out, so both places behave the same.
enum class NoteAction {
    /// Choose the collections of the notes.
    Collections,
    /// Choose the tags of the notes.
    Tags,
    Favorite,
    Unfavorite,
    Archive,
    Unarchive,
    Trash,
    Restore,
    DeletePermanently,
    /// Export the notes to PDF, HTML or JPG.
    Export,
};

} // namespace editor
