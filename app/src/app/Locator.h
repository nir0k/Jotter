#pragma once

#include "core/Note.h"

#include <QObject>

/// Asks the system for the current position for geotags, through Qt
/// Positioning, which reaches GeoClue2 on Linux. Desktop positions are approximate (Wi-Fi or IP
/// based); a position not available within 10 seconds is given up.
class Locator : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    /// Tells whether Qt Positioning has a position service to ask.
    static bool isAvailable();

    /// Asks for the current position on behalf of a note; located() or failed() follows.
    void locate(const QString &noteId);

Q_SIGNALS:
    /// Emitted with the position, rounded to five decimals, for the note it was asked for.
    void located(const QString &noteId, const core::GeoLocation &location);
    /// Emitted when no position came: no service, no permission or no answer in time.
    void failed(const QString &noteId);
};
