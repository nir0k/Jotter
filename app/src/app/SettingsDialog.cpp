#include "app/SettingsDialog.h"

#include "app/Locator.h"
#include "app/RuleEditorDialog.h"
#include "app/SyncPage.h"
#include "core/Attachments.h"
#include "core/Files.h"
#include "core/Library.h"
#include "core/LibrarySession.h"
#include "editor/HtmlCodec.h"
#include "ui/Icons.h"
#include "ui/Popups.h"
#include "ui/Widgets.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QTextDocument>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrentRun>

namespace {

/// Returns the command that launches Jotter with @p option, through flatpak under Flatpak.
QString launchCommand(QLatin1StringView option)
{
    return (qEnvironmentVariableIsSet("FLATPAK_ID")
                ? QStringLiteral("flatpak run io.github.nir0k.Jotter --")
                : QStringLiteral("jotter --")) +
           option;
}

/// Creates a selectable line of configuration in the code role, with a button that copies it.
///
/// @param text The line.
/// @param copy Receives the Copy button, labelled by retranslate().
/// @return The line.
QLabel *makeCommandLine(const QString &text, QPushButton *&copy)
{
    auto *line = new QLabel(text);
    ui::setTextRole(line, ui::TextRole::Code);
    line->setTextInteractionFlags(Qt::TextSelectableByMouse);
    line->setWordWrap(true);
    copy = new QPushButton(ui::icon(QStringLiteral("Copy")), {});
    QObject::connect(copy, &QPushButton::clicked, line,
                     [line] { QApplication::clipboard()->setText(line->text()); });
    return line;
}

} // namespace

SettingsDialog::SettingsDialog(ui::PaletteId palette, ui::ThemeMode mode, const QString &language,
                               QWidget *parent)
    : QDialog(parent)
    , m_sections(new QListWidget(this))
    , m_pages(new QStackedWidget(this))
{
    const ui::Theme &theme = *ui::Theme::current();
    m_sections->setProperty("pane", QStringLiteral("nav"));
    m_sections->setFixedWidth(theme.px(11));
    m_sections->setFrameShape(QFrame::NoFrame);
    m_sections->setIconSize(QSize(theme.px(1.0), theme.px(1.0)));
    for (const char *icon : {"Settings", "Theme", "Language", "Note", "Storage", "Cloud", "Rules",
                             "Keyboard", "Info"}) {
        auto *item = new QListWidgetItem(ui::icon(QString::fromLatin1(icon)), QString());
        m_sections->addItem(item);
    }
    m_pages->addWidget(buildGeneral());
    m_pages->addWidget(buildAppearance());
    m_pages->addWidget(buildLanguage());
    m_pages->addWidget(buildNotes());
    m_pages->addWidget(buildLibrary());
    m_syncPage = new SyncPage;
    m_pages->addWidget(ui::settingsPage({m_syncPage}));
    m_pages->addWidget(buildRules());
    m_pages->addWidget(buildKeyboard());
    m_pages->addWidget(buildAbout());

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *sidebar = new QWidget(this);
    sidebar->setProperty("pane", QStringLiteral("nav"));
    sidebar->setAttribute(Qt::WA_StyledBackground);
    auto *sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(theme.px(0.5), theme.px(0.75), theme.px(0.5), 0);
    sidebarLayout->addWidget(m_sections);
    layout->addWidget(sidebar);
    auto *pages = new QWidget(this);
    pages->setProperty("pane", QStringLiteral("editor"));
    pages->setAttribute(Qt::WA_StyledBackground);
    auto *pagesLayout = new QVBoxLayout(pages);
    pagesLayout->setContentsMargins(0, 0, 0, 0);
    pagesLayout->addWidget(m_pages);
    layout->addWidget(pages, 1);
    resize(theme.px(54), theme.px(42));

    m_themePicker->setPalette(palette);
    m_mode->setValue(static_cast<int>(mode));
    updateModeControl(palette);
    (language == QLatin1StringView("ru") ? m_russian : m_english)->setChecked(true);

    connect(m_sections, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    m_sections->setCurrentRow(0);
    retranslate();
}

QWidget *SettingsDialog::buildGeneral()
{
    m_trayTitle = new QLabel;
    m_trayIcon = new QCheckBox;
    m_trayStyleTitle = new QLabel;
    m_trayStyle = new ui::SegmentedControl;
    m_trayStyle->addOption({}, false);
    m_trayStyle->addOption({}, true);
    connect(m_trayIcon, &QCheckBox::toggled, this, [this](bool on) {
        m_trayStyle->setEnabled(on);
        Q_EMIT trayIconChosen(on);
    });
    connect(m_trayStyle, &ui::SegmentedControl::changed, this,
            [this](const QVariant &value) { Q_EMIT monochromeTrayIconChosen(value.toBool()); });
    m_trayHint = new QLabel;
    m_trayHint->setWordWrap(true);
    ui::setTextRole(m_trayHint, ui::TextRole::Hint);
    m_trayStartHint = new QLabel;
    m_trayStartHint->setWordWrap(true);
    ui::setTextRole(m_trayStartHint, ui::TextRole::Muted);
    QLabel *const trayStart = makeCommandLine(
        QStringLiteral("exec %1").arg(launchCommand(QLatin1StringView("tray"))), m_copyTrayStart);
    return ui::settingsPage({ui::settingsSection(
        m_trayTitle, {m_trayIcon, ui::settingsRow({m_trayStyleTitle, m_trayStyle}), m_trayHint,
                      m_trayStartHint, trayStart, ui::settingsRow({m_copyTrayStart})})});
}

void SettingsDialog::setTrayIcon(bool on, bool monochrome)
{
    const QSignalBlocker blocker(m_trayIcon);
    m_trayIcon->setChecked(on);
    m_trayStyle->setValue(monochrome);
    m_trayStyle->setEnabled(on);
}

QWidget *SettingsDialog::buildAppearance()
{
    m_themeTitle = new QLabel;
    m_themePicker = new ui::ThemePicker;
    m_modeTitle = new QLabel;
    m_mode = new ui::SegmentedControl;
    m_mode->addOption({}, static_cast<int>(ui::ThemeMode::Auto));
    m_mode->addOption({}, static_cast<int>(ui::ThemeMode::Light));
    m_mode->addOption({}, static_cast<int>(ui::ThemeMode::Dark));
    m_modeHint = new QLabel;
    m_modeHint->setWordWrap(true);
    ui::setTextRole(m_modeHint, ui::TextRole::Hint);
    m_sizeTitle = new QLabel;
    m_size = new ui::SegmentedControl;
    for (const int pixels : {14, 16, 18}) {
        m_size->addOption({}, pixels);
    }
    m_sizeHint = new QLabel;
    m_sizeHint->setWordWrap(true);
    ui::setTextRole(m_sizeHint, ui::TextRole::Hint);
    connect(m_size, &ui::SegmentedControl::changed, this,
            [this](const QVariant &value) { Q_EMIT interfaceSizeChosen(value.toInt()); });

    connect(m_themePicker, &ui::ThemePicker::paletteChosen, this, [this](ui::PaletteId palette) {
        updateModeControl(palette);
        Q_EMIT paletteChosen(palette);
    });
    connect(m_mode, &ui::SegmentedControl::changed, this, [this](const QVariant &value) {
        Q_EMIT modeChosen(static_cast<ui::ThemeMode>(value.toInt()));
    });
    return ui::settingsPage(
        {ui::settingsSection(m_modeTitle, {ui::settingsRow({m_mode}), m_modeHint}),
         ui::settingsSection(m_sizeTitle, {ui::settingsRow({m_size}), m_sizeHint}),
         ui::settingsSection(m_themeTitle, {m_themePicker})});
}

QWidget *SettingsDialog::buildLanguage()
{
    m_languageTitle = new QLabel;
    // Each language is named in itself, so it can be found without reading the current one.
    m_english = new QRadioButton(QStringLiteral("English"));
    m_russian = new QRadioButton(QStringLiteral("Русский"));
    auto *group = new QButtonGroup(this);
    group->addButton(m_english);
    group->addButton(m_russian);
    connect(m_english, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) {
            Q_EMIT languageChosen(QStringLiteral("en"));
        }
    });
    connect(m_russian, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) {
            Q_EMIT languageChosen(QStringLiteral("ru"));
        }
    });
    return ui::settingsPage({ui::settingsSection(m_languageTitle, {m_english, m_russian})});
}

QWidget *SettingsDialog::buildNotes()
{
    m_geotagTitle = new QLabel;
    m_addLocation = new QCheckBox;
    connect(m_addLocation, &QCheckBox::toggled, this, &SettingsDialog::addLocationChosen);
    m_geotagHint = new QLabel;
    m_geotagHint->setWordWrap(true);
    ui::setTextRole(m_geotagHint, ui::TextRole::Hint);
    m_checklistTitle = new QLabel;
    m_strikeChecked = new QCheckBox;
    connect(m_strikeChecked, &QCheckBox::toggled, this, &SettingsDialog::strikeCheckedItemsChosen);
    m_trashTitle = new QLabel;
    m_retention = new ui::SegmentedControl;
    // The trash keeps notes 7, 30 or 90 days, or until emptied (0); the library's setting,
    // following it to other devices.
    for (const int days : {7, 30, 90, 0}) {
        m_retention->addOption({}, days);
    }
    connect(m_retention, &ui::SegmentedControl::changed, this, [this](const QVariant &value) {
        if (m_session == nullptr) {
            return;
        }
        const int days = value.toInt();
        if (auto saved = m_session->updateInfo([days](core::LibraryInfo &info) {
                info.trashRetentionDays = days > 0 ? std::optional<int>(days) : std::nullopt;
            });
            !saved) {
            QMessageBox::warning(this, tr("Trash"), core::describe(saved.error()));
            updateTrash();
        }
    });
    m_trashHint = new QLabel;
    m_trashHint->setWordWrap(true);
    ui::setTextRole(m_trashHint, ui::TextRole::Hint);
    return ui::settingsPage({ui::settingsSection(m_geotagTitle, {m_addLocation, m_geotagHint}),
                             ui::settingsSection(m_checklistTitle, {m_strikeChecked}),
                             ui::settingsSection(m_trashTitle, {m_retention, m_trashHint})});
}

void SettingsDialog::setAddLocation(bool on)
{
    const QSignalBlocker blocker(m_addLocation);
    m_addLocation->setChecked(on);
}

void SettingsDialog::setStrikeCheckedItems(bool on)
{
    const QSignalBlocker blocker(m_strikeChecked);
    m_strikeChecked->setChecked(on);
}

void SettingsDialog::updateTrash()
{
    const bool writable = m_session != nullptr && !m_session->library().isReadOnly();
    m_retention->setEnabled(writable);
    if (m_session != nullptr) {
        m_retention->setValue(m_session->library().info().trashRetentionDays.value_or(0));
    }
}

QWidget *SettingsDialog::buildLibrary()
{
    m_locationTitle = new QLabel;
    m_location = new QLabel;
    ui::setTextRole(m_location, ui::TextRole::Code);
    m_location->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_location->setWordWrap(true);
    m_openFolder = new QPushButton(ui::icon(QStringLiteral("OpenExternal")), {});
    m_openAnother = new QPushButton;
    m_createNew = new QPushButton;
    m_libraryNotice = new ui::Notice;
    m_indexTitle = new QLabel;
    m_indexHint = new QLabel;
    m_indexHint->setWordWrap(true);
    ui::setTextRole(m_indexHint, ui::TextRole::Muted);
    m_rebuild = new QPushButton(ui::icon(QStringLiteral("Refresh")), {});
    m_problemsTitle = new QLabel;
    m_problemsEmpty = new QLabel;
    ui::setTextRole(m_problemsEmpty, ui::TextRole::Muted);
    m_problems = new QWidget;
    auto *problemsLayout = new QVBoxLayout(m_problems);
    problemsLayout->setContentsMargins(0, 0, 0, 0);
    problemsLayout->setSpacing(ui::Theme::current()->px(0.5));

    connect(m_openFolder, &QPushButton::clicked, this, [this] {
        if (m_session != nullptr) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_session->library().root()));
        }
    });
    connect(m_openAnother, &QPushButton::clicked, this, &SettingsDialog::openAnotherLibrary);
    connect(m_createNew, &QPushButton::clicked, this, &SettingsDialog::createLibrary);
    connect(m_rebuild, &QPushButton::clicked, this, [this] {
        if (m_session != nullptr) {
            m_session->rebuildIndex();
            m_rebuild->setEnabled(false);
        }
    });
    return ui::settingsPage(
        {ui::settingsSection(m_locationTitle,
                             {m_location, ui::settingsRow({m_openFolder}),
                              ui::settingsRow({m_openAnother, m_createNew}), m_libraryNotice}),
         ui::settingsSection(m_indexTitle, {m_indexHint, ui::settingsRow({m_rebuild})}),
         ui::settingsSection(m_problemsTitle, {m_problemsEmpty, m_problems})});
}

QWidget *SettingsDialog::buildRules()
{
    m_rulesTitle = new QLabel;
    m_rulesHint = new QLabel;
    m_rulesHint->setWordWrap(true);
    ui::setTextRole(m_rulesHint, ui::TextRole::Muted);
    m_rulesEmpty = new QLabel;
    ui::setTextRole(m_rulesEmpty, ui::TextRole::Muted);
    m_rules = new QWidget;
    auto *rows = new QVBoxLayout(m_rules);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(ui::Theme::current()->px(0.5));
    m_newRule = new QPushButton(ui::icon(QStringLiteral("Add")), {});
    connect(m_newRule, &QPushButton::clicked, this, [this] {
        if (m_session != nullptr) {
            core::Rule rule;
            rule.id = core::newId();
            editRule(this, *m_session, rule);
        }
    });
    return ui::settingsPage({ui::settingsSection(
        m_rulesTitle, {m_rulesHint, m_rulesEmpty, m_rules, ui::settingsRow({m_newRule})})});
}

void SettingsDialog::updateRules()
{
    QLayout *rows = m_rules->layout();
    while (const QLayoutItem *row = rows->takeAt(0)) {
        // The list is rebuilt from a row's own checkbox or menu, so rows go later.
        row->widget()->hide();
        row->widget()->deleteLater();
        delete row;
    }
    const bool writable = m_session != nullptr && !m_session->library().isReadOnly();
    const QList<core::Rule> rules = m_session != nullptr ? m_session->rules() : QList<core::Rule>();
    const QList<core::Collection> collections =
        m_session != nullptr ? m_session->collections() : QList<core::Collection>();
    for (qsizetype i = 0; i < rules.size(); ++i) {
        const core::Rule &rule = rules.at(i);
        auto *enabled = new QCheckBox;
        enabled->setChecked(rule.enabled);
        enabled->setEnabled(writable);
        enabled->setToolTip(tr("Enabled"));
        enabled->setAccessibleName(tr("Enable “%1”").arg(rule.name));
        connect(enabled, &QCheckBox::toggled, this, [this, rules, i](bool on) {
            QList<core::Rule> changed = rules;
            changed[i].enabled = on;
            saveRules(changed);
        });

        // The summary: how many conditions, what the rule does and whether it stops. A rule with
        // conditions of earlier versions is never run, so it cannot be edited or applied.
        QStringList names;
        for (const QString &id : rule.action.collections) {
            const auto found = std::ranges::find(collections, id, &core::Collection::id);
            if (found != collections.end()) {
                names.append(found->name);
            }
        }
        const QString targets =
            names.isEmpty() ? tr("a deleted collection") : names.join(QStringLiteral(", "));
        QStringList summary{
            rule.supported
                ? tr("%n condition(s)", nullptr, static_cast<int>(rule.conditions.size()))
                : tr("conditions no longer supported"),
            rule.action.kind == core::RuleAction::Kind::Move ? tr("moves to %1").arg(targets)
                                                             : tr("adds to %1").arg(targets)};
        if (rule.stop) {
            summary.append(tr("stops"));
        }
        auto *name = new QLabel(rule.name);
        auto *details = new QLabel(summary.join(QStringLiteral(" · ")));
        ui::setTextRole(details, ui::TextRole::Muted);

        auto *edit = new QPushButton(tr("Edit…"));
        edit->setEnabled(writable && rule.supported);
        connect(edit, &QPushButton::clicked, this, [this, rule] {
            if (m_session != nullptr) {
                editRule(this, *m_session, rule);
            }
        });
        auto *more = new QToolButton;
        more->setIcon(ui::icon(QStringLiteral("More")));
        more->setToolTip(tr("More actions"));
        more->setAccessibleName(tr("More actions for “%1”").arg(rule.name));
        more->setPopupMode(QToolButton::InstantPopup);
        more->setEnabled(writable);
        auto *menu = new QMenu(more);
        more->setMenu(menu);
        const auto moveBy = [this, rules, i](qsizetype step) {
            QList<core::Rule> changed = rules;
            changed.move(i, i + step);
            saveRules(changed);
        };
        QAction *up = menu->addAction(tr("Move up"));
        up->setEnabled(i > 0);
        connect(up, &QAction::triggered, this, [moveBy] { moveBy(-1); });
        QAction *down = menu->addAction(tr("Move down"));
        down->setEnabled(i + 1 < rules.size());
        connect(down, &QAction::triggered, this, [moveBy] { moveBy(1); });
        QAction *apply = menu->addAction(tr("Apply to existing notes…"));
        apply->setEnabled(rule.supported);
        connect(apply, &QAction::triggered, this, [this, rule] { applyRule(rule); });
        menu->addSeparator();
        connect(
            menu->addAction(ui::icon(QStringLiteral("Delete"), ui::IconTone::Error), tr("Delete…")),
            &QAction::triggered, this, [this, rules, i] {
                if (ui::confirmDestructive(this, tr("Delete the rule “%1”?").arg(rules.at(i).name),
                                           tr("Notes it has filed stay in their collections."),
                                           tr("Delete"))) {
                    QList<core::Rule> changed = rules;
                    changed.removeAt(i);
                    saveRules(changed);
                }
            });

        auto *row = new QWidget;
        if (!rule.supported) {
            row->setToolTip(tr("This rule looks at the title or uses a regular expression, which "
                               "Jotter no longer supports, so it is never run. It can be switched "
                               "off, moved and deleted."));
        }
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        auto *text = new QVBoxLayout;
        text->setSpacing(0);
        text->addWidget(name);
        text->addWidget(details);
        rowLayout->addWidget(enabled);
        rowLayout->addLayout(text, 1);
        rowLayout->addWidget(edit);
        rowLayout->addWidget(more);
        rows->addWidget(row);
    }
    m_rules->setVisible(!rules.isEmpty());
    m_rulesEmpty->setVisible(rules.isEmpty());
    m_newRule->setEnabled(writable);
}

void SettingsDialog::saveRules(const QList<core::Rule> &rules)
{
    if (m_session == nullptr) {
        return;
    }
    if (auto saved = m_session->saveRules(rules); !saved) {
        QMessageBox::warning(this, tr("Rules"), core::describe(saved.error()));
        updateRules();
    }
}

void SettingsDialog::applyRule(const core::Rule &rule)
{
    if (m_session == nullptr) {
        return;
    }
    const auto ids = m_session->notesForRule(rule);
    if (!ids) {
        QMessageBox::warning(this, tr("Rules"), core::describe(ids.error()));
        return;
    }
    if (ids->isEmpty()) {
        QMessageBox::information(this, tr("Rules"),
                                 tr("No note would change: the notes matching “%1” are already "
                                    "where it files them.")
                                     .arg(rule.name));
        return;
    }
    QMessageBox question(QMessageBox::Question, tr("Apply “%1” to existing notes?").arg(rule.name),
                         tr("%n note(s) will change their collections. You can undo it from the "
                            "message that follows.",
                            nullptr, static_cast<int>(ids->size())),
                         QMessageBox::NoButton, this);
    QPushButton *apply = question.addButton(tr("Apply"), QMessageBox::AcceptRole);
    apply->setProperty("role", QStringLiteral("primary"));
    question.addButton(tr("Cancel"), QMessageBox::RejectRole);
    question.setDefaultButton(apply);
    question.exec();
    if (question.clickedButton() != apply) {
        return;
    }
    m_session->mutate(
        [rule, ids = *ids](core::LibrarySession &worker) { return worker.applyRule(rule, ids); });
}

QWidget *SettingsDialog::buildKeyboard()
{
    m_shortcutsTitle = new QLabel;
    m_shortcuts = new QWidget;
    auto *grid = new QGridLayout(m_shortcuts);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(ui::Theme::current()->px(1.5));
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    m_captureTitle = new QLabel;
    m_captureHint = new QLabel;
    m_captureHint->setWordWrap(true);
    ui::setTextRole(m_captureHint, ui::TextRole::Muted);
    QLabel *const binding = makeCommandLine(QStringLiteral("bindsym $mod+Shift+n exec %1")
                                                .arg(launchCommand(QLatin1StringView("new-note"))),
                                            m_copyBinding);
    return ui::settingsPage(
        {ui::settingsSection(m_captureTitle,
                             {m_captureHint, binding, ui::settingsRow({m_copyBinding})}),
         ui::settingsSection(m_shortcutsTitle, {m_shortcuts})});
}

void SettingsDialog::updateKeyboard()
{
    auto *grid = static_cast<QGridLayout *>(m_shortcuts->layout());
    while (const QLayoutItem *item = grid->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    // The shortcuts, fixed in this version.
    const QList<std::pair<QString, QString>> shortcuts{
        {tr("New note"), QStringLiteral("Ctrl+N")},
        {tr("New collection"), QStringLiteral("Ctrl+Shift+N")},
        {tr("Search notes"), QStringLiteral("Ctrl+Shift+F")},
        {tr("Find in note"), QStringLiteral("Ctrl+F")},
        {tr("Replace"), QStringLiteral("Ctrl+H")},
        {tr("Next / previous match"), QStringLiteral("F3, Shift+F3")},
        {tr("Focus mode"), QStringLiteral("Ctrl+Shift+D")},
        {tr("Note info"), QStringLiteral("Ctrl+Shift+I")},
        {tr("Bold, italic, underline"), QStringLiteral("Ctrl+B, Ctrl+I, Ctrl+U")},
        {tr("Strikethrough"), QStringLiteral("Ctrl+Shift+X")},
        {tr("Superscript, subscript"), QStringLiteral("Ctrl+., Ctrl+,")},
        {tr("Paragraph, headings 1–3"), QStringLiteral("Ctrl+0, Ctrl+1, Ctrl+2, Ctrl+3")},
        {tr("Numbered, bulleted list, checklist"),
         QStringLiteral("Ctrl+Shift+7, Ctrl+Shift+8, Ctrl+Shift+9")},
        {tr("Link"), QStringLiteral("Ctrl+K")},
        {tr("Paste as plain text"), QStringLiteral("Ctrl+Shift+V")},
        {tr("Move between panes"), QStringLiteral("F6, Shift+F6")},
        {tr("Rename collection"), QStringLiteral("F2")},
        {tr("Settings"), QStringLiteral("Ctrl+Alt+S")},
        {tr("Close the new note window"), QStringLiteral("Ctrl+Return, Esc")},
    };
    for (int row = 0; row < shortcuts.size(); ++row) {
        const auto &[action, keys] = shortcuts.at(row);
        auto *keysLabel = new QLabel(QKeySequence(keys).toString(QKeySequence::NativeText));
        ui::setTextRole(keysLabel, ui::TextRole::Code);
        // Several keys wrap after their commas instead of widening the page.
        keysLabel->setWordWrap(true);
        auto *actionLabel = new QLabel(action);
        actionLabel->setWordWrap(true);
        grid->addWidget(actionLabel, row, 0);
        grid->addWidget(keysLabel, row, 1);
    }
}

QWidget *SettingsDialog::buildAbout()
{
    m_aboutTitle = new QLabel;
    m_aboutText = new QLabel;
    m_aboutText->setWordWrap(true);
    m_licencesTitle = new QLabel;
    m_licences = new QLabel;
    m_licences->setWordWrap(true);
    m_licences->setTextFormat(Qt::RichText);
    m_aboutQt = new QPushButton;
    connect(m_aboutQt, &QPushButton::clicked, this, [this] { QMessageBox::aboutQt(this); });
    connect(m_licences, &QLabel::linkActivated, this, [this](const QString &link) {
        QFile file(QStringLiteral(":/licenses/%1").arg(link));
        if (file.open(QIODevice::ReadOnly)) {
            QMessageBox::information(this, tr("Licence"), QString::fromUtf8(file.readAll()));
        }
    });
    return ui::settingsPage(
        {ui::settingsSection(m_aboutTitle, {m_aboutText}),
         ui::settingsSection(m_licencesTitle, {m_licences, ui::settingsRow({m_aboutQt})})});
}

void SettingsDialog::setSession(core::LibrarySession *session)
{
    if (m_session != nullptr) {
        m_session->disconnect(this);
    }
    m_session = session;
    if (session != nullptr) {
        connect(session, &core::LibrarySession::notesChanged, this, [this] {
            m_rebuild->setEnabled(true);
            updateLibrary();
        });
        connect(session, &core::LibrarySession::rulesChanged, this, &SettingsDialog::updateRules);
        connect(session, &core::LibrarySession::infoChanged, this, &SettingsDialog::updateTrash);
        connect(session, &core::LibrarySession::collectionsChanged, this,
                &SettingsDialog::updateRules);
    }
    updateLibrary();
    updateTrash();
    updateRules();
}

void SettingsDialog::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QDialog::changeEvent(event);
}

void SettingsDialog::retranslate()
{
    setWindowTitle(tr("Settings"));
    const QStringList sections{tr("General"), tr("Appearance"), tr("Language"),
                               tr("Notes"),   tr("Library"),    tr("Sync"),
                               tr("Rules"),   tr("Keyboard"),   tr("About")};
    for (int i = 0; i < m_sections->count(); ++i) {
        m_sections->item(i)->setText(sections.at(i));
    }
    m_trayTitle->setText(tr("System tray"));
    m_trayIcon->setText(tr("Show the icon in the system tray"));
    m_trayStyleTitle->setText(tr("Icon"));
    m_trayStyle->setOptionText(false, tr("Colour"));
    m_trayStyle->setOptionText(true, tr("Monochrome"));
    m_trayHint->setText(
        QSystemTrayIcon::isSystemTrayAvailable()
            ? tr("The icon creates notes, opens Jotter and quits it. Closing the window "
                 "keeps Jotter running in the tray.")
            : tr("This desktop shows no system tray, so closing the window quits Jotter. On sway "
                 "the tray is shown by the bar, for example the tray module of Waybar."));
    m_trayStartHint->setText(
        tr("To start Jotter in the tray at login, without its window, add the command to your "
           "desktop's autostart; on sway, add this line to its configuration:"));
    m_copyTrayStart->setText(tr("Copy"));
    m_modeTitle->setText(tr("Light or dark"));
    m_mode->setOptionText(static_cast<int>(ui::ThemeMode::Auto), tr("Auto"));
    m_mode->setOptionText(static_cast<int>(ui::ThemeMode::Light), tr("Light"));
    m_mode->setOptionText(static_cast<int>(ui::ThemeMode::Dark), tr("Dark"));
    m_themeTitle->setText(tr("Colour theme"));
    m_sizeTitle->setText(tr("Interface size"));
    m_size->setOptionText(14, tr("Small"));
    m_size->setOptionText(16, tr("Medium"));
    m_size->setOptionText(18, tr("Large"));
    m_sizeHint->setText(
        tr("Text and controls change at once; some spacing follows after Jotter restarts."));
    m_languageTitle->setText(tr("Interface language"));
    m_locationTitle->setText(tr("Library folder"));
    m_openFolder->setText(tr("Open folder"));
    m_openAnother->setText(tr("Open another library…"));
    m_createNew->setText(tr("Create a new library…"));
    m_indexTitle->setText(tr("Search index"));
    m_indexHint->setText(tr("Jotter keeps an index of the notes for lists and search. It holds "
                            "nothing the library does not and can be rebuilt at any time."));
    m_rebuild->setText(tr("Rebuild index"));
    m_problemsTitle->setText(tr("Problems"));
    m_geotagTitle->setText(tr("Location"));
    m_addLocation->setText(tr("Add location to new notes"));
    m_geotagHint->setText(
        Locator::isAvailable()
            ? tr("A new note asks the system for its position, which desktops know only "
                 "approximately (from Wi-Fi or the network). On sway a GeoClue agent must be "
                 "running.")
            : tr("No position service was found. Install GeoClue to add locations to notes."));
    m_checklistTitle->setText(tr("Checklists"));
    m_strikeChecked->setText(tr("Strike through checked items"));
    m_trashTitle->setText(tr("Trash"));
    m_retention->setOptionText(7, tr("7 days"));
    m_retention->setOptionText(30, tr("30 days"));
    m_retention->setOptionText(90, tr("90 days"));
    m_retention->setOptionText(0, tr("Never"));
    m_trashHint->setText(tr("How long deleted notes stay in the trash before they are deleted "
                            "permanently. The setting belongs to the library."));
    m_rulesTitle->setText(tr("Rules"));
    m_rulesHint->setText(tr("Rules file notes into collections by their title and tags. They run "
                            "in this order when a note is created and when its title or tags "
                            "change. They belong to the library and follow it to other devices."));
    m_rulesEmpty->setText(tr("No rules yet."));
    m_newRule->setText(tr("New rule…"));
    m_captureTitle->setText(tr("New note"));
    m_captureHint->setText(
        tr("Jotter has no global shortcuts of its own. Bind a key to the command in your "
           "desktop's settings; on sway, add this line to its configuration:"));
    m_copyBinding->setText(tr("Copy"));
    m_shortcutsTitle->setText(tr("Shortcuts"));
    updateKeyboard();
    m_aboutTitle->setText(QStringLiteral("Jotter %1").arg(QApplication::applicationVersion()));
    m_aboutText->setText(tr("A note manager that keeps notes as ordinary HTML files, readable "
                            "without Jotter."));
    m_licencesTitle->setText(tr("Licences"));
    m_licences->setText(
        tr("Jotter is free software under the Apache License 2.0. It uses Qt 6 under the GNU LGPL "
           "3.0 and <a href=\"%1\">Bootstrap Icons 1.13.2</a> under the MIT licence.")
            .arg(QStringLiteral("Bootstrap-Icons-MIT.txt")));
    m_aboutQt->setText(tr("About Qt"));
    updateModeControl(ui::Theme::current()->palette());
    updateLibrary();
    updateRules();
}

void SettingsDialog::setInterfaceSize(int pixels)
{
    m_size->setValue(pixels);
}

void SettingsDialog::updateModeControl(ui::PaletteId palette)
{
    const bool twoVariants = ui::hasLightVariant(palette);
    m_mode->setEnabled(twoVariants);
    m_modeHint->setVisible(!twoVariants);
    m_modeHint->setText(tr("%1 has only a dark variant.").arg(ui::paletteName(palette)));
}

void SettingsDialog::updateLibrary()
{
    const bool open = m_session != nullptr;
    m_location->setText(open ? QDir::toNativeSeparators(m_session->library().root()) : QString());
    m_openFolder->setEnabled(open);
    m_rebuild->setEnabled(open && !m_session->isScanning());
    QLayout *rows = m_problems->layout();
    while (const QLayoutItem *row = rows->takeAt(0)) {
        delete row->widget();
        delete row;
    }
    QList<core::Problem> problems;
    if (open) {
        problems = m_session->index().problems().value_or(QList<core::Problem>());
    }
    for (const core::Problem &problem : problems) {
        QString reason;
        switch (problem.error.code) {
        case core::ErrorCode::NewerFormat:
            reason = tr("Made by a newer version of Jotter");
            break;
        case core::ErrorCode::Malformed:
            reason = tr("Damaged or not a Jotter note");
            break;
        default:
            reason = tr("Cannot be read");
            break;
        }
        auto *file = new QLabel(problem.path);
        ui::setTextRole(file, ui::TextRole::Code);
        file->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto *why = new QLabel(reason);
        ui::setTextRole(why, ui::TextRole::Muted);
        why->setWordWrap(true);
        // The technical detail is English diagnostic text, so it is only a tooltip.
        why->setToolTip(problem.error.detail);
        auto *row = new QWidget;
        auto *rowLayout = new QVBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(0);
        rowLayout->addWidget(file);
        rowLayout->addWidget(why);
        if (problem.error.detail.startsWith(QLatin1StringView("unsupported body:")) &&
            !m_session->library().isReadOnly()) {
            auto *normalize = new QPushButton(tr("Create normalised copy"), row);
            connect(normalize, &QPushButton::clicked, this, [this, path = problem.path] {
                if (QMessageBox::question(
                        this, tr("Create normalised copy"),
                        tr("Unsupported formatting will be simplified in a new note. The original "
                           "file will be kept unchanged. Continue?")) != QMessageBox::Yes) {
                    return;
                }
                const QString fullPath =
                    QDir(m_session->library().notesDir()).filePath(QFileInfo(path).fileName());
                m_session->mutate([fullPath](core::LibrarySession &worker) -> core::Result<void> {
                    auto contents = core::readFile(fullPath);
                    if (!contents) {
                        return std::unexpected(contents.error());
                    }
                    auto note = core::parseNoteForNormalization(*contents, fullPath);
                    if (!note) {
                        return std::unexpected(note.error());
                    }
                    QTextDocument document;
                    editor::readBody(document, note->body);
                    note->body = editor::writeBody(document);
                    note->meta.id = core::newId();
                    note->meta.created = core::now();
                    if (auto copied = core::copyNoteAttachments(worker.library().root(), *note);
                        !copied) {
                        return std::unexpected(copied.error());
                    }
                    return worker.saveNote(*note);
                });
            });
            rowLayout->addWidget(normalize);
        }
        rows->addWidget(row);
    }
    m_problems->setVisible(!problems.isEmpty());
    m_problemsEmpty->setVisible(problems.isEmpty());
    m_problemsEmpty->setText(tr("Every note file could be read."));
}

void SettingsDialog::openAnotherLibrary()
{
    const QString path = QFileDialog::getExistingDirectory(this, tr("Open another library"));
    if (path.isEmpty()) {
        return;
    }
    if (!core::Library::exists(path)) {
        m_libraryNotice->showNotice(ui::Notice::Kind::Error,
                                    tr("%1 is not a Jotter library: it has no library.json.")
                                        .arg(QDir::toNativeSeparators(path)));
        return;
    }
    m_libraryNotice->hide();
    Q_EMIT libraryChosen(path);
}

void SettingsDialog::createLibrary()
{
    const QString path =
        QFileDialog::getExistingDirectory(this, tr("Choose an empty folder for the new library"));
    if (path.isEmpty()) {
        return;
    }
    auto library = core::Library::create(path);
    if (!library) {
        m_libraryNotice->showNotice(ui::Notice::Kind::Error, core::describe(library.error()));
        return;
    }
    m_libraryNotice->hide();
    Q_EMIT libraryChosen(library->root());
}
