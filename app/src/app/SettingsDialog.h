#pragma once

#include "ui/Palette.h"
#include "ui/Theme.h"

#include <QDialog>

namespace core {
class LibrarySession;
struct Rule;
} // namespace core
class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QRadioButton;
class QStackedWidget;
class SyncPage;
namespace ui {
class Notice;
class SegmentedControl;
class ThemePicker;
} // namespace ui

/// Settings (preferences template): General, Appearance, Language, Notes,
/// Library, Sync, Rules, Keyboard and About. Every value here is a choice and applies at once;
/// there is no Save, except in the rule editor, a form dialog, and the Sync connection, which is
/// checked before it is saved.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    /// The sections, in the order of the navigation list.
    enum class Section {
        General,
        Appearance,
        Language,
        Notes,
        Library,
        Sync,
        Rules,
        Keyboard,
        About
    };

    SettingsDialog(ui::PaletteId palette, ui::ThemeMode mode, const QString &language,
                   QWidget *parent = nullptr);

    /// Switches to @p section.
    void showSection(Section section);

    /// Marks the current interface size (14, 16 or 18 pixels).
    void setInterfaceSize(int pixels);

    /// Marks whether Jotter shows its icon in the system tray, and whether it is monochrome.
    void setTrayIcon(bool on, bool monochrome);

    /// Marks whether new notes get a geotag.
    void setAddLocation(bool on);

    /// Marks whether checked checklist items are struck through.
    void setStrikeCheckedItems(bool on);

    /// Shows the library of @p session in the Library section.
    void setSession(core::LibrarySession *session);

    /// Returns the Sync section, which the application connects to the open library's
    /// synchronisation.
    SyncPage *syncPage() const { return m_syncPage; }

Q_SIGNALS:
    void paletteChosen(ui::PaletteId palette);
    void modeChosen(ui::ThemeMode mode);
    void interfaceSizeChosen(int pixels);
    void languageChosen(const QString &language);
    /// Emitted when the person switches the system tray icon on or off.
    void trayIconChosen(bool on);
    /// Emitted when the person chooses the coloured or the monochrome tray icon.
    void monochromeTrayIconChosen(bool on);
    /// Emitted when the person switches geotags of new notes on or off.
    void addLocationChosen(bool on);
    /// Emitted when the person switches striking through checked checklist items on or off.
    void strikeCheckedItemsChosen(bool on);
    /// Emitted with the folder of a library to switch to, created or validated here.
    void libraryChosen(const QString &path);

protected:
    void changeEvent(QEvent *event) override;

private:
    QWidget *buildGeneral();
    QWidget *buildAppearance();
    QWidget *buildLanguage();
    QWidget *buildNotes();
    QWidget *buildLibrary();
    QWidget *buildRules();
    QWidget *buildKeyboard();
    QWidget *buildAbout();
    void retranslate();
    void updateModeControl(ui::PaletteId palette);
    void updateLibrary();
    /// Shows the library's trash retention, which only a writable library can change.
    void updateTrash();
    void updateRules();
    /// Saves the rules after a change in the list, reporting a failure.
    void saveRules(const QList<core::Rule> &rules);
    /// Counts the notes a rule would change and, after a confirmation, changes them.
    void applyRule(const core::Rule &rule);
    /// Fills the list of shortcuts in the interface language.
    void updateKeyboard();
    void openAnotherLibrary();
    void createLibrary();

    core::LibrarySession *m_session = nullptr;
    QListWidget *m_sections;
    QStackedWidget *m_pages;
    SyncPage *m_syncPage = nullptr;

    QLabel *m_trayTitle = nullptr;
    QCheckBox *m_trayIcon = nullptr;
    QLabel *m_trayStyleTitle = nullptr;
    /// The tray icon's look: coloured (false) or monochrome (true).
    ui::SegmentedControl *m_trayStyle = nullptr;
    QLabel *m_trayHint = nullptr;
    QLabel *m_trayStartHint = nullptr;
    QPushButton *m_copyTrayStart = nullptr;

    QLabel *m_themeTitle = nullptr;
    ui::ThemePicker *m_themePicker = nullptr;
    QLabel *m_modeTitle = nullptr;
    ui::SegmentedControl *m_mode = nullptr;
    QLabel *m_modeHint = nullptr;
    QLabel *m_sizeTitle = nullptr;
    ui::SegmentedControl *m_size = nullptr;
    QLabel *m_sizeHint = nullptr;

    QLabel *m_languageTitle = nullptr;
    QRadioButton *m_english = nullptr;
    QRadioButton *m_russian = nullptr;

    QLabel *m_geotagTitle = nullptr;
    QCheckBox *m_addLocation = nullptr;
    QLabel *m_geotagHint = nullptr;
    QLabel *m_checklistTitle = nullptr;
    QCheckBox *m_strikeChecked = nullptr;
    QLabel *m_trashTitle = nullptr;
    ui::SegmentedControl *m_retention = nullptr;
    QLabel *m_trashHint = nullptr;

    QLabel *m_locationTitle = nullptr;
    QLabel *m_location = nullptr;
    QPushButton *m_openFolder = nullptr;
    QPushButton *m_openAnother = nullptr;
    QPushButton *m_createNew = nullptr;
    QLabel *m_indexTitle = nullptr;
    QLabel *m_indexHint = nullptr;
    QPushButton *m_rebuild = nullptr;
    QLabel *m_problemsTitle = nullptr;
    QLabel *m_problemsEmpty = nullptr;
    /// One row per unreadable file: its name and, under it, the reason.
    QWidget *m_problems = nullptr;
    ui::Notice *m_libraryNotice = nullptr;

    QLabel *m_rulesTitle = nullptr;
    QLabel *m_rulesHint = nullptr;
    QLabel *m_rulesEmpty = nullptr;
    /// One row per rule: Enabled, its name and summary, Edit and a menu of the other actions.
    QWidget *m_rules = nullptr;
    QPushButton *m_newRule = nullptr;

    QLabel *m_shortcutsTitle = nullptr;
    /// Two columns: the action and its keys.
    QWidget *m_shortcuts = nullptr;
    QLabel *m_captureTitle = nullptr;
    QLabel *m_captureHint = nullptr;
    QPushButton *m_copyBinding = nullptr;

    QLabel *m_aboutTitle = nullptr;
    QLabel *m_aboutText = nullptr;
    QLabel *m_licencesTitle = nullptr;
    QLabel *m_licences = nullptr;
    QPushButton *m_aboutQt = nullptr;
};
