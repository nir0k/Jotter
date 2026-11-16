#pragma once

#include <QString>

namespace core {
class SyncScheduler;
}

/// The state of synchronisation in words, as the sidebar's footer and Settings → Sync show it.
struct SyncStatus
{
    /// A short line, such as "Synced 5 min ago", "Syncing 3 of 10" or "Sync failed".
    QString text;
    /// The glyph shown with it.
    QString icon;
    /// Whether the last run failed; the error itself is explained where there is room.
    bool failed = false;
};

/// Describes the state of a scheduler in the interface language.
///
/// @param scheduler The scheduler of the open library.
/// @return The status; its text is empty when the library is not connected.
SyncStatus syncStatus(const core::SyncScheduler &scheduler);
