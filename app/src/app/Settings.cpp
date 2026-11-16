#include "app/Settings.h"

#include <QStandardPaths>

#include <array>

namespace {

/// The settings group of a library's sync connection.
QString syncGroup(const QString &libraryId)
{
    return QStringLiteral("sync-") + libraryId;
}

/// Sign-in methods by their key in the settings file; an unknown key leaves the default, Key.
constexpr std::array<std::pair<core::SftpConnection::SignIn, const char *>, 2> SignInKeys{{
    {core::SftpConnection::SignIn::Key, "key"},
    {core::SftpConnection::SignIn::Password, "password"},
}};

QString configFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/jotter/settings.ini");
}

QString stateFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) +
           QStringLiteral("/jotter/state.ini");
}

} // namespace

Settings::Settings()
    : m_config(configFile(), QSettings::IniFormat)
    , m_state(stateFile(), QSettings::IniFormat)
{}

QString Settings::libraryPath() const
{
    return m_config.value(QStringLiteral("library/path")).toString();
}

void Settings::setLibraryPath(const QString &path)
{
    m_config.setValue(QStringLiteral("library/path"), path);
}

ui::PaletteId Settings::palette() const
{
    // A palette this build no longer knows falls back to Warm.
    return ui::paletteFromKey(m_config.value(QStringLiteral("appearance/palette")).toString())
        .value_or(ui::PaletteId::Warm);
}

void Settings::setPalette(ui::PaletteId palette)
{
    m_config.setValue(QStringLiteral("appearance/palette"), ui::paletteKey(palette));
}

ui::ThemeMode Settings::themeMode() const
{
    return ui::themeModeFromKey(m_config.value(QStringLiteral("appearance/mode")).toString())
        .value_or(ui::ThemeMode::Auto);
}

void Settings::setThemeMode(ui::ThemeMode mode)
{
    m_config.setValue(QStringLiteral("appearance/mode"), ui::themeModeKey(mode));
}

int Settings::interfaceSize() const
{
    const int pixels = m_config.value(QStringLiteral("appearance/size"), 16).toInt();
    return pixels == 14 || pixels == 18 ? pixels : 16;
}

void Settings::setInterfaceSize(int pixels)
{
    m_config.setValue(QStringLiteral("appearance/size"), pixels);
}

bool Settings::trayIcon() const
{
    return m_config.value(QStringLiteral("general/trayIcon"), true).toBool();
}

void Settings::setTrayIcon(bool on)
{
    m_config.setValue(QStringLiteral("general/trayIcon"), on);
}

bool Settings::monochromeTrayIcon() const
{
    return m_config.value(QStringLiteral("general/monochromeTrayIcon"), false).toBool();
}

void Settings::setMonochromeTrayIcon(bool on)
{
    m_config.setValue(QStringLiteral("general/monochromeTrayIcon"), on);
}

bool Settings::addLocation() const
{
    return m_config.value(QStringLiteral("notes/addLocation"), false).toBool();
}

void Settings::setAddLocation(bool on)
{
    m_config.setValue(QStringLiteral("notes/addLocation"), on);
}

bool Settings::strikeCheckedItems() const
{
    return m_config.value(QStringLiteral("editor/strikeCheckedItems"), true).toBool();
}

void Settings::setStrikeCheckedItems(bool on)
{
    m_config.setValue(QStringLiteral("editor/strikeCheckedItems"), on);
}

QString Settings::language() const
{
    return m_config.value(QStringLiteral("appearance/language")).toString();
}

void Settings::setLanguage(const QString &language)
{
    m_config.setValue(QStringLiteral("appearance/language"), language);
}

std::optional<core::SftpConnection> Settings::syncConnection(const QString &libraryId) const
{
    const QString group = syncGroup(libraryId) + u'/';
    const QString host = m_config.value(group + u"host").toString();
    if (host.isEmpty()) {
        return std::nullopt;
    }
    core::SftpConnection connection;
    connection.host = host;
    connection.port = m_config.value(group + u"port", 0).toInt();
    connection.user = m_config.value(group + u"user").toString();
    connection.folder = m_config.value(group + u"folder").toString();
    const QString signIn = m_config.value(group + u"signIn").toString();
    for (const auto &[method, key] : SignInKeys) {
        if (signIn == QLatin1StringView(key)) {
            connection.signIn = method;
        }
    }
    return connection;
}

void Settings::setSyncConnection(const QString &libraryId,
                                 const std::optional<core::SftpConnection> &connection)
{
    m_config.remove(syncGroup(libraryId));
    if (!connection) {
        return;
    }
    const QString group = syncGroup(libraryId) + u'/';
    m_config.setValue(group + u"host", connection->host);
    m_config.setValue(group + u"port", connection->port);
    m_config.setValue(group + u"user", connection->user);
    m_config.setValue(group + u"folder", connection->folder);
    for (const auto &[method, key] : SignInKeys) {
        if (connection->signIn == method) {
            m_config.setValue(group + u"signIn", QLatin1StringView(key));
        }
    }
}

bool Settings::syncAfterChanges() const
{
    return m_config.value(QStringLiteral("sync/afterChanges"), true).toBool();
}

void Settings::setSyncAfterChanges(bool on)
{
    m_config.setValue(QStringLiteral("sync/afterChanges"), on);
}

int Settings::syncInterval() const
{
    return m_config.value(QStringLiteral("sync/interval"), 15).toInt();
}

void Settings::setSyncInterval(int minutes)
{
    m_config.setValue(QStringLiteral("sync/interval"), minutes);
}

QDateTime Settings::lastSynced(const QString &libraryId) const
{
    return m_state.value(syncGroup(libraryId) + u"/lastSynced").toDateTime();
}

void Settings::setLastSynced(const QString &libraryId, const QDateTime &time)
{
    m_state.setValue(syncGroup(libraryId) + u"/lastSynced", time);
}

core::NoteSort Settings::noteSort() const
{
    const QString key = m_config.value(QStringLiteral("notes/sort")).toString();
    core::NoteSort sort;
    if (key == QLatin1StringView("created")) {
        sort.key = core::NoteSort::Key::Created;
    } else if (key == QLatin1StringView("title")) {
        sort.key = core::NoteSort::Key::Title;
    }
    sort.descending = m_config.value(QStringLiteral("notes/descending"), true).toBool();
    return sort;
}

void Settings::setNoteSort(const core::NoteSort &sort)
{
    QString key = QStringLiteral("updated");
    if (sort.key == core::NoteSort::Key::Created) {
        key = QStringLiteral("created");
    } else if (sort.key == core::NoteSort::Key::Title) {
        key = QStringLiteral("title");
    }
    m_config.setValue(QStringLiteral("notes/sort"), key);
    m_config.setValue(QStringLiteral("notes/descending"), sort.descending);
}

QByteArray Settings::windowGeometry() const
{
    return m_state.value(QStringLiteral("window/geometry")).toByteArray();
}

void Settings::setWindowGeometry(const QByteArray &geometry)
{
    m_state.setValue(QStringLiteral("window/geometry"), geometry);
}

QByteArray Settings::splitterState() const
{
    return m_state.value(QStringLiteral("window/splitter")).toByteArray();
}

void Settings::setSplitterState(const QByteArray &state)
{
    m_state.setValue(QStringLiteral("window/splitter"), state);
}

bool Settings::sidebarVisible() const
{
    return m_state.value(QStringLiteral("window/sidebar"), true).toBool();
}

void Settings::setSidebarVisible(bool visible)
{
    m_state.setValue(QStringLiteral("window/sidebar"), visible);
}

QString Settings::lastView() const
{
    return m_state.value(QStringLiteral("view/last")).toString();
}

void Settings::setLastView(const QString &view)
{
    m_state.setValue(QStringLiteral("view/last"), view);
}

QString Settings::lastNote() const
{
    return m_state.value(QStringLiteral("view/note")).toString();
}

void Settings::setLastNote(const QString &id)
{
    m_state.setValue(QStringLiteral("view/note"), id);
}

QStringList Settings::ownNotesOnly() const
{
    return m_state.value(QStringLiteral("view/ownNotesOnly")).toStringList();
}

void Settings::setOwnNotesOnly(const QStringList &collections)
{
    m_state.setValue(QStringLiteral("view/ownNotesOnly"), collections);
}

QStringList Settings::expandedItems() const
{
    return m_state.value(QStringLiteral("view/expanded")).toStringList();
}

void Settings::setExpandedItems(const QStringList &items)
{
    m_state.setValue(QStringLiteral("view/expanded"), items);
}
