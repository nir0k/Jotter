#pragma once

#include "core/Index.h"
#include "core/SftpStorage.h"
#include "ui/Palette.h"
#include "ui/Theme.h"

#include <QByteArray>
#include <QDateTime>
#include <QSettings>
#include <QString>
#include <QStringList>

/// Device preferences (`$XDG_CONFIG_HOME/jotter/settings.ini`) and window state
/// (`$XDG_STATE_HOME/jotter/state.ini`).
///
/// Library settings such as trash retention live in the library, not here.
class Settings
{
public:
    Settings();

    /// The open library's folder, empty before the first start.
    QString libraryPath() const;
    void setLibraryPath(const QString &path);

    ui::PaletteId palette() const;
    void setPalette(ui::PaletteId palette);
    ui::ThemeMode themeMode() const;
    void setThemeMode(ui::ThemeMode mode);

    /// The root size of the interface in pixels (14, 16 or 18; 16 by default), the `rem` every
    /// interface size is measured in.
    int interfaceSize() const;
    void setInterfaceSize(int pixels);

    /// Whether Jotter shows its icon in the system tray, where a closed main window goes; on
    /// by default.
    bool trayIcon() const;
    void setTrayIcon(bool on);

    /// Whether the tray icon is the one-colour drawing that follows the system's light or dark
    /// scheme, instead of the coloured application icon; off by default.
    bool monochromeTrayIcon() const;
    void setMonochromeTrayIcon(bool on);

    /// Whether new notes get the device's position as a geotag; off by
    /// default.
    bool addLocation() const;
    void setAddLocation(bool on);

    /// Whether checked checklist items are drawn struck through and muted; on
    /// by default.
    bool strikeCheckedItems() const;
    void setStrikeCheckedItems(bool on);

    /// The interface language (`en`, `ru`), or empty to follow the system locale.
    QString language() const;
    void setLanguage(const QString &language);

    /// The SFTP connection of a library, or no value when the library is
    /// not synchronised on this device.
    std::optional<core::SftpConnection> syncConnection(const QString &libraryId) const;
    void setSyncConnection(const QString &libraryId,
                           const std::optional<core::SftpConnection> &connection);

    /// Whether a sync runs 30 seconds after the last change; on by default.
    bool syncAfterChanges() const;
    void setSyncAfterChanges(bool on);

    /// Minutes between sync runs, 0 for none; 15 by default.
    int syncInterval() const;
    void setSyncInterval(int minutes);

    /// When a library was last synchronised on this device, or an invalid time.
    QDateTime lastSynced(const QString &libraryId) const;
    void setLastSynced(const QString &libraryId, const QDateTime &time);

    core::NoteSort noteSort() const;
    void setNoteSort(const core::NoteSort &sort);

    QByteArray windowGeometry() const;
    void setWindowGeometry(const QByteArray &geometry);
    QByteArray splitterState() const;
    void setSplitterState(const QByteArray &state);

    /// Whether the sidebar is shown in a wide window.
    bool sidebarVisible() const;
    void setSidebarVisible(bool visible);

    /// The last opened sidebar entry as `kind:id`, and the last opened note.
    QString lastView() const;
    void setLastView(const QString &view);
    QString lastNote() const;
    void setLastNote(const QString &id);

    /// Collections whose view lists their own notes only, without subcollections.
    QStringList ownNotesOnly() const;
    void setOwnNotesOnly(const QStringList &collections);

    /// Identifiers of expanded collections and tag paths of expanded tags.
    QStringList expandedItems() const;
    void setExpandedItems(const QStringList &items);

private:
    QSettings m_config;
    QSettings m_state;
};
