#include "editor/NoteEditor.h"

#include "core/Attachments.h"
#include "core/FileGuard.h"
#include "core/LibrarySession.h"
#include "core/NoteLinks.h"
#include "core/Numbering.h"
#include "editor/Commands.h"
#include "editor/FindBar.h"
#include "editor/HtmlCodec.h"
#include "editor/InfoPanel.h"
#include "editor/NotePicker.h"
#include "editor/Presenter.h"
#include "editor/TagStrip.h"
#include "editor/TextEdit.h"
#include "ui/Icons.h"
#include "ui/Popups.h"
#include "ui/TagColor.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QAction>
#include <QActionGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextList>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <tuple>
#include <utility>

namespace editor {

namespace {

/// Creates an empty note document with the editor's margin, body font and indent.
std::unique_ptr<QTextDocument> newNoteDocument()
{
    auto document = std::make_unique<QTextDocument>();
    const ui::Theme &theme = *ui::Theme::current();
    document->setDocumentMargin(0);
    document->setDefaultFont(theme.font(ui::TextRole::Body));
    document->setIndentWidth(theme.px(2.0));
    return document;
}

} // namespace

/// One live note shared by all of its views; the weak registry retains no closed documents.
/// Loading a note replaces the document and its presenter (see NoteEditor::load()).
struct NoteBuffer
{
    QPointer<core::LibrarySession> session;
    std::optional<core::Note> note;
    QString title;
    std::unique_ptr<QTextDocument> document = newNoteDocument();
    /// The presenter of the document, owned by it.
    Presenter *presenter = new Presenter(document.get());
    QTimer autosave;
    QSet<NoteEditor *> views;
    bool saving = false;
    bool newNote = false;
    bool recreate = false;

    /// Creates one autosave timer for this document, independent of the active view.
    NoteBuffer()
    {
        autosave.setSingleShot(true);
        autosave.setInterval(1000);
        // The timer is the context, as the document is replaced whenever the note is loaded.
        QObject::connect(&autosave, &QTimer::timeout, &autosave, [this] {
            if (!views.isEmpty()) {
                (*views.cbegin())->saveNow();
            }
        });
    }
};

namespace {

/// Finds only a live buffer of this session; unrelated libraries never share documents.
std::shared_ptr<NoteBuffer> noteBuffer(core::LibrarySession *session, const QString &id)
{
    static QList<std::weak_ptr<NoteBuffer>> buffers;
    buffers.removeIf([](const auto &buffer) { return buffer.expired(); });
    for (const auto &weak : std::as_const(buffers)) {
        if (auto buffer = weak.lock();
            buffer && buffer->session == session && buffer->note && buffer->note->meta.id == id) {
            return buffer;
        }
    }
    auto buffer = std::make_shared<NoteBuffer>();
    buffer->session = session;
    buffers.append(buffer);
    return buffer;
}

/// Asks for a link's text and address.
///
/// @return The address and text, an empty address to remove the link, or no value when the
///         person cancelled.
std::optional<std::pair<QString, QString>> askLink(QWidget *parent, const QString &text,
                                                   const QString &address, bool existing,
                                                   const core::LibrarySession *session,
                                                   const QString &openNote)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(existing ? NoteEditor::tr("Edit link") : NoteEditor::tr("Insert link"));
    auto *textField = new QLineEdit(text, &dialog);
    // A link goes to a web or mail address, or to a note chosen by its title.
    QString noteId = core::noteLinkTarget(address).value_or(QString());
    auto *addressField = new QLineEdit(noteId.isEmpty() ? address : QString(), &dialog);
    addressField->setPlaceholderText(QStringLiteral("https://"));
    auto *noteButton = new QPushButton(&dialog);
    const auto showNote = [&noteId, noteButton, session](const QString &title) {
        if (noteId.isEmpty()) {
            noteButton->setText(NoteEditor::tr("Choose a note…"));
        } else if (!title.isEmpty()) {
            noteButton->setText(title);
        } else {
            // A link being edited shows its note's title, or that the note is gone.
            const auto known = session != nullptr
                                   ? session->index().note(noteId).value_or(std::nullopt)
                                   : std::nullopt;
            QString label = NoteEditor::tr("Note not found");
            if (known) {
                label = known->title.isEmpty() ? NoteEditor::tr("Untitled") : known->title;
            }
            noteButton->setText(label);
        }
    };
    showNote({});
    auto *picker = new NotePicker(&dialog);
    picker->setSession(session);
    picker->setExcluded(openNote);
    QObject::connect(noteButton, &QPushButton::clicked, &dialog, [&] {
        picker->openAt(noteButton->mapToGlobal(QPoint(0, noteButton->height())), noteId);
    });
    QObject::connect(picker, &NotePicker::chosen, &dialog,
                     [&](const QString &id, const QString &title) {
                         noteId = id;
                         addressField->clear();
                         if (textField->text().isEmpty()) {
                             textField->setText(title);
                         }
                         showNote(title);
                     });
    QObject::connect(addressField, &QLineEdit::textEdited, &dialog, [&] {
        noteId.clear();
        showNote({});
    });
    auto *error = new QLabel(&dialog);
    error->setProperty("inlineError", true);
    error->setWordWrap(true);
    error->hide();
    auto *form = new QFormLayout;
    form->addRow(NoteEditor::tr("Text"), textField);
    form->addRow(NoteEditor::tr("Address"), addressField);
    form->addRow(NoteEditor::tr("Note"), noteButton);
    form->addRow(QString(), error);
    auto *buttons = new QDialogButtonBox(&dialog);
    QPushButton *save = buttons->addButton(NoteEditor::tr("Save"), QDialogButtonBox::AcceptRole);
    save->setProperty("role", QStringLiteral("primary"));
    save->setDefault(true);
    buttons->addButton(NoteEditor::tr("Cancel"), QDialogButtonBox::RejectRole);
    bool removeRequested = false;
    if (existing) {
        QPushButton *remove =
            buttons->addButton(NoteEditor::tr("Remove link"), QDialogButtonBox::DestructiveRole);
        remove->setProperty("role", QStringLiteral("danger"));
        QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
            removeRequested = true;
            dialog.accept();
        });
    }
    auto *layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(buttons);
    QString normalised;
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(save, &QPushButton::clicked, &dialog, [&] {
        normalised = noteId.isEmpty() ? core::normaliseLink(addressField->text())
                                      : core::noteLinkAddress(noteId);
        if (normalised.isEmpty()) {
            error->setText(NoteEditor::tr(
                "Enter a web or mail address (http, https or mailto), or choose a note."));
            error->show();
            addressField->setFocus();
            return;
        }
        dialog.accept();
    });
    (address.isEmpty() && !text.isEmpty() ? addressField : textField)->setFocus();
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    if (removeRequested) {
        return std::pair<QString, QString>{QString(), QString()};
    }
    QString linkText = textField->text();
    if (linkText.isEmpty()) {
        linkText = noteId.isEmpty() ? normalised : noteButton->text();
    }
    return std::pair<QString, QString>{normalised, linkText};
}

} // namespace

NoteEditor::NoteEditor(QWidget *parent)
    : QWidget(parent)
    , m_back(new QToolButton(this))
    , m_stack(new QStackedWidget(this))
    , m_empty(new ui::EmptyState(this))
    , m_page(new QWidget(this))
    , m_header(new QWidget(m_page))
    , m_iconButton(new QToolButton(m_header))
    , m_title(new QLineEdit(m_header))
    , m_focus(new QToolButton(m_header))
    , m_more(new QToolButton(m_header))
    , m_tags(new TagStrip(m_header))
    , m_picker(new ui::IconPicker(this))
    , m_formatBar(new QWidget(this))
    , m_toolbar(new QToolBar(m_formatBar))
    , m_notice(new ui::Notice(m_header))
    , m_text(new TextEdit(m_page))
    , m_find(new FindBar(m_text, m_page))
    , m_info(new InfoPanel(m_page))
    , m_notePicker(new NotePicker(this))
    , m_tableSize(new ui::TableSizePopup(this))
{
    attachBuffer(std::make_shared<NoteBuffer>());
    setProperty("pane", QStringLiteral("editor"));
    setAttribute(Qt::WA_StyledBackground);
    const ui::Theme &theme = *ui::Theme::current();

    m_back->setIcon(ui::icon(QStringLiteral("Back")));
    m_back->hide();
    m_title->setProperty("titleField", true);
    m_title->setFrame(false);
    m_text->setFrameShape(QFrame::NoFrame);
    m_text->document()->setDocumentMargin(0);
    m_text->document()->setDefaultFont(theme.font(ui::TextRole::Body));
    m_text->document()->setIndentWidth(theme.px(2.0));
    setFocusProxy(m_text);
    buildToolbar();

    const int pad = theme.px(1.5);
    auto *top = new QHBoxLayout;
    top->setContentsMargins(pad, 0, pad, 0);
    top->addWidget(m_back);
    top->addStretch();

    // The title and notices sit above the text with the same side margins.
    auto *headerLayout = new QVBoxLayout(m_header);
    // A line under the title and tags sets them apart from the text, like the toolbar's line.
    m_header->setProperty("noteHeader", true);
    m_header->setAttribute(Qt::WA_StyledBackground);
    headerLayout->setContentsMargins(pad, 0, pad, theme.px(0.75));
    headerLayout->setSpacing(theme.px(0.5));
    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->addWidget(m_iconButton);
    titleRow->addWidget(m_title, 1);
    titleRow->addWidget(m_focus);
    titleRow->addWidget(m_more);
    headerLayout->addLayout(titleRow);
    headerLayout->addWidget(m_tags);
    headerLayout->addWidget(m_notice);
    auto *pageLayout = new QVBoxLayout(m_page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);
    pageLayout->addWidget(m_header);
    pageLayout->addWidget(m_find);
    pageLayout->addSpacing(theme.px(0.75));
    // The info panel sits on the right of the text when open.
    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(m_text, 1);
    body->addWidget(m_info);
    pageLayout->addLayout(body, 1);
    m_info->hide();
    m_infoTimer.setSingleShot(true);
    m_infoTimer.setInterval(300);
    connect(&m_infoTimer, &QTimer::timeout, this, [this] { m_info->setText(*m_text->document()); });
    connect(m_text, &QTextEdit::textChanged, this, [this] {
        if (m_info->isVisible()) {
            m_infoTimer.start();
        }
    });

    m_stack->addWidget(m_empty);
    m_stack->addWidget(m_page);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, theme.px(0.5), 0, 0);
    layout->setSpacing(theme.px(0.75));
    layout->addLayout(top);
    layout->addWidget(m_stack, 1);
    // The formatting toolbar runs along the bottom of the pane, with its
    // controls lined up with the text.
    m_formatBar->setProperty("formatBar", true);
    m_formatBar->setAttribute(Qt::WA_StyledBackground);
    auto *barLayout = new QHBoxLayout(m_formatBar);
    barLayout->setContentsMargins(pad, theme.px(0.25), pad, theme.px(0.25));
    barLayout->addWidget(m_toolbar);
    layout->addWidget(m_formatBar);
    // The text uses the pane's width; the margins sit inside the text widget, so its scroll bar
    // stays at the pane's edge.
    m_text->setSideMargin(pad);
    // The formatting toolbar belongs to an open note.
    connect(m_stack, &QStackedWidget::currentChanged, this,
            [this] { m_formatBar->setVisible(m_stack->currentWidget() == m_page); });
    m_formatBar->hide();

    connect(m_text, &QTextEdit::textChanged, this, &NoteEditor::scheduleSave);
    connect(m_title, &QLineEdit::textEdited, this, [this](const QString &title) {
        m_buffer->title = title;
        refreshViews();
        scheduleSave();
    });
    connect(m_title, &QLineEdit::returnPressed, this, [this] {
        QTextCursor cursor = m_text->textCursor();
        cursor.movePosition(QTextCursor::Start);
        m_text->setTextCursor(cursor);
        m_text->setFocus();
    });
    connect(m_text, &QTextEdit::cursorPositionChanged, this, &NoteEditor::updateToolbar);
    connect(m_text, &TextEdit::message, this, &NoteEditor::message);
    connect(m_text, &QTextEdit::currentCharFormatChanged, this, &NoteEditor::updateToolbar);
    connect(m_back, &QToolButton::clicked, this, &NoteEditor::backRequested);
    m_more->setIcon(ui::icon(QStringLiteral("More")));
    m_focus->setIcon(ui::icon(QStringLiteral("Focus")));
    connect(m_focus, &QToolButton::clicked, this, &NoteEditor::focusModeRequested);
    connect(m_text, &TextEdit::noteLinkRequested, this, [this] {
        m_notePicker->openAt(m_text->viewport()->mapToGlobal(m_text->cursorRect().bottomLeft()));
    });
    connect(m_notePicker, &NotePicker::chosen, this, &NoteEditor::insertNoteLink);
    connect(m_text, &TextEdit::noteLinkActivated, this, &NoteEditor::noteLinkActivated);
    connect(m_info, &InfoPanel::noteActivated, this, &NoteEditor::noteLinkActivated);
    connect(m_info, &InfoPanel::removeLocationRequested, this,
            [this] { changeMeta([](core::NoteMeta &meta) { meta.location.reset(); }); });
    m_iconButton->setIconSize(QSize(theme.px(1.5), theme.px(1.5)));
    connect(m_more, &QToolButton::clicked, this, &NoteEditor::showMoreMenu);
    connect(m_iconButton, &QToolButton::clicked, this, [this] {
        if (m_buffer->note) {
            m_picker->open(m_iconButton, m_buffer->note->meta.icon.value_or(QString()),
                           m_buffer->note->meta.color.value_or(QString()), true);
        }
    });
    connect(m_tags, &TagStrip::tagsChanged, this, [this](const QStringList &tags) {
        changeMeta([&](core::NoteMeta &meta) { meta.tags = tags; });
    });
    m_tags->setSuggestionSource([this] {
        QStringList names;
        if (m_session != nullptr) {
            for (const core::TagCount &tag :
                 m_session->index().tags().value_or(QList<core::TagCount>())) {
                names.append(tag.name);
            }
        }
        return names;
    });
    connect(m_picker, &ui::IconPicker::iconChosen, this, [this](const QString &icon) {
        changeMeta([&](core::NoteMeta &meta) {
            meta.icon = icon.isEmpty() ? std::nullopt : std::optional<QString>(icon);
        });
    });
    connect(m_picker, &ui::IconPicker::colorChosen, this, [this](const QString &color) {
        changeMeta([&](core::NoteMeta &meta) {
            meta.color = color.isEmpty() ? std::nullopt : std::optional<QString>(color);
        });
    });
    connect(ui::Theme::current(), &ui::Theme::changed, this, [this] {
        // The interface size can change while a note is open; the text follows it.
        const ui::Theme &current = *ui::Theme::current();
        m_text->document()->setDefaultFont(current.font(ui::TextRole::Body));
        m_toolbar->setIconSize(QSize(current.px(1.25), current.px(1.25)));
        updateHeader();
    });
    retranslate();
    m_stack->setCurrentWidget(m_empty);
}

NoteEditor::~NoteEditor()
{
    m_buffer->views.remove(this);
    // QTextEdit must release the shared document before its last owner disappears.
    delete m_text;
}

void NoteEditor::attachBuffer(std::shared_ptr<NoteBuffer> buffer)
{
    if (m_buffer) {
        m_buffer->views.remove(this);
    }
    m_text->setNoteDocument(buffer->document.get());
    m_presenter = buffer->presenter;
    m_buffer = std::move(buffer);
    m_buffer->views.insert(this);
}

void NoteEditor::refreshViews()
{
    const auto views = m_buffer->views.values();
    for (NoteEditor *view : views) {
        const QSignalBlocker blocker(view->m_title);
        view->m_title->setText(m_buffer->title);
        view->updateHeader();
        view->updateReadOnly();
        Q_EMIT view->titleChanged(m_buffer->title);
    }
}

void NoteEditor::buildToolbar()
{
    const ui::Theme &theme = *ui::Theme::current();
    m_toolbar->setIconSize(QSize(theme.px(1.25), theme.px(1.25)));
    m_toolbar->setMovable(false);
    m_toolbar->setFloatable(false);
    // The overflow button of a narrow toolbar gets Jotter's own glyph: Fusion's own arrow
    // is barely visible on the editor's surface.
    if (auto *more = m_toolbar->findChild<QToolButton *>(QStringLiteral("qt_toolbar_ext_button"))) {
        more->setIcon(ui::icon(QStringLiteral("ChevronRight")));
        more->installEventFilter(this);
    }

    // Each action also belongs to the editor itself, so its shortcut works wherever the focus
    // is in the pane, even when the action sits in the toolbar's overflow menu.
    const auto action = [this](const char *iconName, const QKeySequence &shortcut, bool checkable,
                               auto slot) {
        auto *created = new QAction(ui::icon(QString::fromLatin1(iconName)), QString(), this);
        created->setShortcut(shortcut);
        created->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        created->setCheckable(checkable);
        connect(created, &QAction::triggered, this, slot);
        addAction(created);
        return created;
    };

    // Paragraph styles are chosen from a menu under a button that shows the current style;
    // each has its shortcut, Ctrl+0 to Ctrl+3. Quote has a button of its
    // own next to it.
    m_blockStyle = new QToolButton(m_toolbar);
    m_blockStyle->setPopupMode(QToolButton::InstantPopup);
    auto *styleMenu = new QMenu(m_blockStyle);
    // The toolbar runs along the bottom, so the menu opens upwards (see eventFilter()).
    styleMenu->installEventFilter(this);
    auto *styleGroup = new QActionGroup(this);
    const std::array<const char *, 4> styleIcons{"Paragraph", "Heading1", "Heading2", "Heading3"};
    for (std::size_t i = 0; i < m_styles.size(); ++i) {
        const auto style = static_cast<BlockStyle>(i);
        m_styles.at(i) = action(styleIcons.at(i), QKeySequence(QStringLiteral("Ctrl+%1").arg(i)),
                                true, [this, style] {
                                    setBlockStyle(m_text->textCursor(), style);
                                    updateToolbar();
                                    m_text->setFocus();
                                });
        styleGroup->addAction(m_styles.at(i));
        styleMenu->addAction(m_styles.at(i));
    }
    m_blockStyle->setMenu(styleMenu);
    m_toolbar->addWidget(m_blockStyle);
    m_quote = action("Quote", QKeySequence(), true, [this](bool on) {
        setBlockStyle(m_text->textCursor(), on ? BlockStyle::Quote : BlockStyle::Paragraph);
        updateToolbar();
        m_text->setFocus();
    });
    m_toolbar->addAction(m_quote);
    m_toolbar->addSeparator();

    const auto toggleFormat = [this](auto change) {
        return [this, change](bool on) {
            QTextCharFormat format;
            change(format, on);
            m_text->mergeCurrentCharFormat(format);
        };
    };
    m_bold = action("Bold", QKeySequence::Bold, true, toggleFormat([](QTextCharFormat &f, bool on) {
                        f.setFontWeight(on ? QFont::Bold : QFont::Normal);
                    }));
    m_italic = action("Italic", QKeySequence::Italic, true,
                      toggleFormat([](QTextCharFormat &f, bool on) { f.setFontItalic(on); }));
    m_underline = action("Underline", QKeySequence::Underline, true,
                         toggleFormat([](QTextCharFormat &f, bool on) { f.setFontUnderline(on); }));
    m_strike = action("Strikethrough", QKeySequence(QStringLiteral("Ctrl+Shift+X")), true,
                      toggleFormat([](QTextCharFormat &f, bool on) { f.setFontStrikeOut(on); }));
    m_superscript = action("Superscript", QKeySequence(QStringLiteral("Ctrl+.")), true,
                           toggleFormat([](QTextCharFormat &f, bool on) {
                               f.setVerticalAlignment(on ? QTextCharFormat::AlignSuperScript
                                                         : QTextCharFormat::AlignNormal);
                           }));
    m_subscript = action("Subscript", QKeySequence(QStringLiteral("Ctrl+,")), true,
                         toggleFormat([](QTextCharFormat &f, bool on) {
                             f.setVerticalAlignment(on ? QTextCharFormat::AlignSubScript
                                                       : QTextCharFormat::AlignNormal);
                         }));
    m_inlineCode = action("InlineCode", QKeySequence(), true, [this](bool on) {
        setCharProperty(*m_text, InlineCode, on ? QVariant(true) : QVariant());
    });
    for (QAction *format :
         {m_bold, m_italic, m_underline, m_strike, m_superscript, m_subscript, m_inlineCode}) {
        m_toolbar->addAction(format);
    }

    // Colours are chosen from a grid of tiles that show the result.
    const auto colorButton = [this](const char *iconName, bool highlight) {
        auto *button = new QToolButton(m_toolbar);
        button->setIcon(ui::icon(QString::fromLatin1(iconName)));
        auto *popup = new ui::ColorPopup(
            highlight ? ui::ColorGrid::Style::Highlight : ui::ColorGrid::Style::Text, this);
        const int property = highlight ? HighlightKey : TextColorKey;
        connect(button, &QToolButton::clicked, this, [this, button, popup, property] {
            popup->open(button, m_text->currentCharFormat().stringProperty(property));
        });
        connect(popup, &ui::ColorPopup::chosen, this, [this, property](const QString &key) {
            setCharProperty(*m_text, property, key);
            m_text->setFocus();
        });
        m_toolbar->addWidget(button);
        return button;
    };
    m_textColor = colorButton("TextColor", false);
    m_highlight = colorButton("Highlight", true);
    m_link = action("Link", QKeySequence(QStringLiteral("Ctrl+K")), false, &NoteEditor::editLink);
    m_toolbar->addAction(m_link);
    m_toolbar->addSeparator();

    const auto listAction = [&](const char *iconName, const char *keys, ListKind kind) {
        return action(iconName, QKeySequence(QString::fromLatin1(keys)), true, [this, kind] {
            toggleList(m_text->textCursor(), kind);
            updateToolbar();
        });
    };
    m_bulleted = listAction("BulletedList", "Ctrl+Shift+8", ListKind::Bulleted);
    m_numbered = listAction("NumberedList", "Ctrl+Shift+7", ListKind::Numbered);
    m_checklist = listAction("Checklist", "Ctrl+Shift+9", ListKind::Checklist);
    // The numbered list button has the style menu on its arrow.
    m_numberedButton = new QToolButton(m_toolbar);
    m_numberedButton->setDefaultAction(m_numbered);
    m_numberedButton->setPopupMode(QToolButton::MenuButtonPopup);
    m_numberingMenu = new QMenu(m_numberedButton);
    m_numberingMenu->installEventFilter(this);
    connect(m_numberingMenu, &QMenu::aboutToShow, this, &NoteEditor::fillNumberingMenu);
    m_numberedButton->setMenu(m_numberingMenu);
    m_outdent = action("Outdent", QKeySequence(), false,
                       [this] { changeListLevel(m_text->textCursor(), -1); });
    m_indent = action("Indent", QKeySequence(), false,
                      [this] { changeListLevel(m_text->textCursor(), 1); });
    m_divider =
        action("Divider", QKeySequence(), false, [this] { insertDivider(m_text->textCursor()); });
    m_codeBlock = action("Code", QKeySequence(), false, [this] {
        QTextCursor cursor = m_text->textCursor();
        if (formatCode(cursor)) {
            m_text->setTextCursor(cursor);
        }
        m_text->setFocus();
    });
    // The table button opens the size grid; its action is for the overflow menu.
    m_table = action("TableView", QKeySequence(), false, [this] {
        QWidget *anchor = m_toolbar->widgetForAction(m_table);
        m_tableSize->open(anchor != nullptr && anchor->isVisible() ? anchor : m_toolbar);
    });
    connect(m_tableSize, &ui::TableSizePopup::chosen, this,
            [this](int rows, int columns, bool header) {
                QTextCursor cursor = m_text->textCursor();
                if (insertTable(cursor, rows, columns, header) != nullptr) {
                    m_text->setTextCursor(cursor);
                }
                m_text->setFocus();
            });
    m_image = action("Image", QKeySequence(), false, [this] { chooseFiles(true); });
    m_attachment = action("Attach", QKeySequence(), false, [this] { chooseFiles(false); });
    m_toolbar->addAction(m_bulleted);
    m_toolbar->addWidget(m_numberedButton);
    for (QAction *list : {m_checklist, m_outdent, m_indent}) {
        m_toolbar->addAction(list);
    }
    m_toolbar->addSeparator();
    for (QAction *insert : {m_codeBlock, m_table, m_image, m_attachment, m_divider}) {
        m_toolbar->addAction(insert);
    }

    // Find and replace and the info panel have shortcuts but no toolbar button.
    m_findAction = action("Search", QKeySequence::Find, false, [this] { m_find->open(false); });
    m_replaceAction = action("Search", QKeySequence(QStringLiteral("Ctrl+H")), false,
                             [this] { m_find->open(true); });
    // With the find bar closed, F3 steps through the words of the note list's search, if any.
    m_findNext = action("ChevronDown", QKeySequence(Qt::Key_F3), false, [this] {
        if (!m_find->isVisible() && !m_find->hasSearchTerms()) {
            m_find->open(false);
        }
        m_find->step(true);
    });
    m_findPrevious = action("ChevronUp", QKeySequence(Qt::SHIFT | Qt::Key_F3), false, [this] {
        if (!m_find->isVisible() && !m_find->hasSearchTerms()) {
            m_find->open(false);
        }
        m_find->step(false);
    });
    m_infoAction = action("Info", QKeySequence(QStringLiteral("Ctrl+Shift+I")), true,
                          [this](bool on) { setInfoVisible(on); });
}

void NoteEditor::fillNumberingMenu()
{
    m_numberingMenu->clear();
    const QTextList *list = m_text->textCursor().currentList();
    const bool numbered = list != nullptr && listKindOf(list->format()) == ListKind::Numbered;
    const QString current = numbered ? list->format().stringProperty(NumberingStyleId) : QString();
    const auto addStyle = [&](const QString &id, const QString &name) {
        QAction *entry = m_numberingMenu->addAction(name);
        entry->setCheckable(true);
        entry->setChecked(numbered && (current == id ||
                                       (current.isEmpty() && id == core::DefaultNumberingStyle)));
        connect(entry, &QAction::triggered, this, [this, id] {
            setNumberingStyle(m_text->textCursor(), id);
            updateToolbar();
            m_text->setFocus();
        });
    };
    // Styles are named by their first marker; Multilevel by words.
    for (const core::NumberingStyle &style : core::numberingStyles()) {
        addStyle(style.id.toString(), style.includeParents ? tr("Multilevel (1. 1.1. 1.1.1.)")
                                                           : core::numberingMarker(style, {1}));
    }
    m_numberingMenu->addSeparator();
    QAction *restart = m_numberingMenu->addAction(tr("Restart numbering"), this, [this] {
        setListStart(m_text->textCursor(), 1);
        m_text->setFocus();
    });
    QAction *startAt = m_numberingMenu->addAction(tr("Start at…"), this, [this] {
        const QTextList *atCursor = m_text->textCursor().currentList();
        bool ok = false;
        const int start = QInputDialog::getInt(this, tr("Start at"), tr("First number"),
                                               atCursor != nullptr ? atCursor->format().start() : 1,
                                               1, 999999999, 1, &ok);
        if (ok) {
            setListStart(m_text->textCursor(), start);
        }
        m_text->setFocus();
    });
    restart->setEnabled(numbered);
    startAt->setEnabled(numbered);
}

void NoteEditor::chooseFiles(bool images)
{
    if (!m_buffer->note || m_readOnly) {
        return;
    }
    const QStringList paths =
        images ? QFileDialog::getOpenFileNames(
                     this, tr("Insert image"),
                     QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
                     tr("Images (*.png *.jpg *.jpeg *.gif *.webp)"))
               : QFileDialog::getOpenFileNames(
                     this, tr("Attach files"),
                     QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    if (!paths.isEmpty()) {
        m_text->insertFiles(paths, images);
    }
    m_text->setFocus();
}

void NoteEditor::setInfoVisible(bool visible)
{
    m_infoAction->setChecked(visible);
    m_info->setVisible(visible && m_buffer->note.has_value());
    if (visible) {
        updateInfo();
    }
}

void NoteEditor::updateInfo()
{
    if (!m_buffer->note || !m_info->isVisible()) {
        return;
    }
    QStringList names;
    if (m_session != nullptr) {
        // In the tree's order, which the person arranged.
        for (const core::Collection &collection : m_session->collections()) {
            if (m_buffer->note->meta.collections.contains(collection.id)) {
                names.append(collection.name);
            }
        }
    }
    m_info->setNote(m_buffer->note->meta, names);
    m_info->setText(*m_text->document());
    m_info->setLocation(m_buffer->note->meta.location,
                        m_locationUnavailable.contains(m_buffer->note->meta.id), !m_readOnly);
    if (m_session != nullptr) {
        m_info->setBacklinks(m_session->index()
                                 .backlinks(m_buffer->note->meta.id)
                                 .value_or(QList<core::NoteSummary>()));
    }
}

void NoteEditor::updateLinkedNotes()
{
    if (!m_buffer->note || m_session == nullptr) {
        return;
    }
    QStringList targets;
    const QTextDocument *document = m_text->document();
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat format = it.fragment().charFormat();
            if (const auto target = core::noteLinkTarget(format.anchorHref());
                format.isAnchor() && target && !targets.contains(*target)) {
                targets.append(*target);
            }
        }
    }
    auto known =
        m_session->index().notesById(targets).value_or(QHash<QString, core::NoteSummary>());
    QSet<QString> missing;
    for (const QString &target : std::as_const(targets)) {
        if (!known.contains(target)) {
            missing.insert(target);
        }
    }
    m_text->setLinkedNotes(std::move(known));
    m_presenter->setMissingNotes(std::move(missing));
}

void NoteEditor::insertNoteLink(const QString &noteId, const QString &title)
{
    QTextCursor cursor = m_text->textCursor();
    // The `[[` typed before the picker opened becomes the link.
    const qsizetype at = cursor.positionInBlock();
    if (at >= 2 && cursor.block().text().sliced(at - 2, 2) == QLatin1StringView("[[")) {
        cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, 2);
    }
    setLink(cursor, core::noteLinkAddress(noteId), title);
    m_text->setFocus();
    updateLinkedNotes();
}

void NoteEditor::setLocationsEnabled(bool enabled)
{
    m_locationsEnabled = enabled;
}

void NoteEditor::setLocationUnavailable(const QString &noteId)
{
    m_locationUnavailable.insert(noteId);
    updateInfo();
}

void NoteEditor::retranslate()
{
    m_back->setToolTip(tr("Back to the list"));
    m_back->setAccessibleName(tr("Back to the list"));
    m_title->setPlaceholderText(tr("Untitled"));
    if (auto *more = m_toolbar->findChild<QToolButton *>(QStringLiteral("qt_toolbar_ext_button"))) {
        more->setToolTip(tr("More formatting"));
        more->setAccessibleName(tr("More formatting"));
    }
    m_more->setToolTip(tr("More actions"));
    m_more->setAccessibleName(tr("More actions"));
    const QString focus =
        m_focusMode ? tr("Leave focus mode (Ctrl+Shift+D)") : tr("Focus mode (Ctrl+Shift+D)");
    m_focus->setToolTip(focus);
    m_focus->setAccessibleName(focus);
    m_iconButton->setToolTip(tr("Icon and colour"));
    m_iconButton->setAccessibleName(tr("Icon and colour"));
    m_empty->setContent(QStringLiteral("Note"), tr("No note selected"),
                        tr("Choose a note in the list, or create one with Ctrl+N."));
    const auto describe = [](QAction *action, const QString &text) {
        action->setText(text);
        action->setToolTip(action->shortcut().isEmpty()
                               ? text
                               : QStringLiteral("%1 (%2)").arg(
                                     text, action->shortcut().toString(QKeySequence::NativeText)));
    };
    const std::array styles{tr("Paragraph"), tr("Heading 1"), tr("Heading 2"), tr("Heading 3")};
    for (std::size_t i = 0; i < styles.size(); ++i) {
        describe(m_styles.at(i), styles.at(i));
    }
    describe(m_quote, tr("Quote"));
    describe(m_bold, tr("Bold"));
    describe(m_italic, tr("Italic"));
    describe(m_underline, tr("Underline"));
    describe(m_strike, tr("Strikethrough"));
    describe(m_superscript, tr("Superscript"));
    describe(m_subscript, tr("Subscript"));
    describe(m_inlineCode, tr("Code"));
    describe(m_link, tr("Link"));
    describe(m_bulleted, tr("Bulleted list"));
    describe(m_numbered, tr("Numbered list"));
    describe(m_checklist, tr("Checklist"));
    describe(m_outdent, tr("Decrease indent (Shift+Tab)"));
    describe(m_indent, tr("Increase indent (Tab)"));
    describe(m_divider, tr("Divider"));
    describe(m_codeBlock, tr("Code block"));
    describe(m_table, tr("Table"));
    describe(m_image, tr("Image"));
    describe(m_attachment, tr("Attachment"));
    describe(m_findAction, tr("Find in note"));
    describe(m_replaceAction, tr("Replace"));
    describe(m_findNext, tr("Next match"));
    describe(m_findPrevious, tr("Previous match"));
    describe(m_infoAction, tr("Note info"));
    m_numberedButton->setToolTip(m_numbered->toolTip());
    m_textColor->setToolTip(tr("Text colour"));
    m_textColor->setAccessibleName(tr("Text colour"));
    m_highlight->setToolTip(tr("Highlight"));
    m_highlight->setAccessibleName(tr("Highlight"));
    m_blockStyle->setToolTip(tr("Paragraph style"));
    m_blockStyle->setAccessibleName(tr("Paragraph style"));
    updateReadOnly();
}

bool NoteEditor::eventFilter(QObject *watched, QEvent *event)
{
    // Qt creates the overflow menu once buttons no longer fit and opens it, like the style
    // menu, below its button when the screen has room; it is moved above the toolbar before it
    // appears.
    if (event->type() == QEvent::ChildPolished) {
        if (auto *menu = qobject_cast<QMenu *>(static_cast<QChildEvent *>(event)->child())) {
            menu->installEventFilter(this);
        }
    } else if (event->type() == QEvent::Show) {
        if (auto *menu = qobject_cast<QMenu *>(watched)) {
            menu->move(menu->x(), m_toolbar->mapToGlobal(QPoint()).y() - menu->height());
        }
    }
    return QWidget::eventFilter(watched, event);
}

void NoteEditor::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QWidget::changeEvent(event);
}

bool NoteEditor::setSession(core::LibrarySession *session)
{
    if (!closeNote()) {
        return false;
    }
    if (m_session != nullptr) {
        m_session->disconnect(this);
    }
    m_session = session;
    m_notePicker->setSession(session);
    if (session != nullptr) {
        connect(session, &core::LibrarySession::filesChanged, this, [this] {
            m_text->document()->markContentsDirty(0, m_text->document()->characterCount());
            m_text->viewport()->update();
            checkOutsideChanges();
        });
        connect(session, &core::LibrarySession::savingRequested, this,
                [this](bool &accepted) { accepted = saveNow() && accepted; });
        connect(session, &core::LibrarySession::mutationChanged, this, &NoteEditor::updateReadOnly);
        connect(session, &core::LibrarySession::notesChanged, this,
                &NoteEditor::checkOutsideChanges);
        // Saving another note, as in another window, can rewrite this one: renaming a note
        // renames the links to it. The notes this editor saves itself are left out.
        connect(session, &core::LibrarySession::noteSaved, this, [this](const QString &id) {
            if (!m_buffer->saving && m_buffer->note && m_buffer->note->meta.id != id) {
                checkOutsideChanges();
            }
        });
        // Links and backlinks follow notes created, renamed, trashed or deleted.
        const auto followLinks = [this] {
            updateLinkedNotes();
            updateInfo();
        };
        connect(session, &core::LibrarySession::notesChanged, this, followLinks);
        connect(session, &core::LibrarySession::noteSaved, this, followLinks);
        connect(session, &core::LibrarySession::availabilityChanged, this,
                &NoteEditor::updateReadOnly);
        connect(session, &core::LibrarySession::infoChanged, this,
                [this] { m_tags->setTagColors(m_session->library().info().tagColors); });
    }
    m_tags->setTagColors(session != nullptr ? session->library().info().tagColors
                                            : QMap<QString, QString>());
    return true;
}

void NoteEditor::setFocusMode(bool on)
{
    m_focusMode = on;
    m_focus->setIcon(ui::icon(on ? QStringLiteral("FocusExit") : QStringLiteral("Focus")));
    retranslate();
}

void NoteEditor::setCompact()
{
    m_iconButton->hide();
    m_focus->hide();
    m_more->hide();
    m_infoAction->setEnabled(false);
    // The common formatting stays; inserted objects and the rarer styles are left to the main
    // window.
    const QList<const QWidget *> hiddenWidgets{m_textColor, m_highlight};
    for (QAction *action : {m_superscript, m_subscript, m_inlineCode, m_codeBlock, m_table, m_image,
                            m_attachment, m_divider}) {
        action->setVisible(false);
    }
    bool groupVisible = false;
    QAction *separator = nullptr;
    for (QAction *action : m_toolbar->actions()) {
        if (hiddenWidgets.contains(m_toolbar->widgetForAction(action))) {
            action->setVisible(false);
        }
        // A separator stays only between visible groups.
        if (action->isSeparator()) {
            if (separator != nullptr) {
                separator->setVisible(groupVisible);
            }
            separator = action;
            groupVisible = false;
        } else {
            groupVisible = groupVisible || action->isVisible();
        }
    }
    if (separator != nullptr) {
        separator->setVisible(groupVisible);
    }
}

void NoteEditor::setSearchTerms(const QList<core::SearchTerm> &terms)
{
    m_find->setSearchTerms(terms);
}

bool NoteEditor::openNote(const QString &id)
{
    if (m_buffer->note && m_buffer->note->meta.id == id) {
        return true;
    }
    if (!closeNote()) {
        return false;
    }
    if (m_session == nullptr || id.isEmpty()) {
        return true;
    }
    auto buffer = noteBuffer(m_session, id);
    const bool existing = buffer->note.has_value();
    attachBuffer(std::move(buffer));
    if (existing) {
        m_text->setAttachmentContext(m_session->library().root(), id);
        m_notePicker->setExcluded(id);
        refreshViews();
        updateLinkedNotes();
        updateToolbar();
        m_stack->setCurrentWidget(m_page);
        return true;
    }
    auto note = m_session->library().readNote(id);
    if (!note) {
        m_stack->setCurrentWidget(m_page);
        m_notice->showNotice(ui::Notice::Kind::Error, core::describe(note.error()));
        m_title->setReadOnly(true);
        m_text->setReadOnly(true);
        m_toolbar->setEnabled(false);
        return true;
    }
    load(*note);
    return true;
}

bool NoteEditor::closeNote()
{
    if (m_buffer->saving) {
        return false;
    }
    if (!saveNow()) {
        return false;
    }
    discardEmptyNew();
    attachBuffer(std::make_shared<NoteBuffer>());
    m_stack->setCurrentWidget(m_empty);
    Q_EMIT titleChanged(QString());
    Q_EMIT noteClosed();
    return true;
}

QString NoteEditor::noteId() const
{
    return m_buffer->note ? m_buffer->note->meta.id : QString();
}

void NoteEditor::focusTitle()
{
    m_title->setFocus();
    m_title->selectAll();
}

void NoteEditor::reload()
{
    if (m_buffer->note && m_session != nullptr && !m_session->isMutating()) {
        checkOutsideChanges();
    }
}

void NoteEditor::markNew()
{
    m_buffer->newNote = m_buffer->note.has_value();
}

void NoteEditor::discardEmptyNew()
{
    if (m_buffer->views.size() > 1 || !std::exchange(m_buffer->newNote, false) || !m_buffer->note ||
        m_session == nullptr) {
        return;
    }
    if (!m_title->text().trimmed().isEmpty() || !m_text->document()->isEmpty() ||
        m_buffer->note->meta.icon) {
        return;
    }
    const QString id = m_buffer->note->meta.id;
    // Forget the note first: deleting it refreshes the list, which may open another note.
    m_buffer->note.reset();
    m_buffer->autosave.stop();
    if (auto deleted = m_session->deleteNotes({id}); !deleted) {
        qWarning("Cannot delete the empty new note %s: %s", qPrintable(id),
                 qPrintable(deleted.error().detail));
    }
}

void NoteEditor::setBackButtonVisible(bool visible)
{
    m_back->setVisible(visible);
}

void NoteEditor::load(const core::Note &note)
{
    // Reloading a shared document preserves each view's own navigation state.
    QList<std::tuple<NoteEditor *, int, int, int>> positions;
    if (m_buffer->note) {
        for (NoteEditor *view : std::as_const(m_buffer->views)) {
            const QTextCursor cursor = view->m_text->textCursor();
            positions.append({view, cursor.position(), cursor.anchor(),
                              view->m_text->verticalScrollBar()->value()});
        }
    }
    m_buffer->autosave.stop();
    m_buffer->note = note;
    m_buffer->title = note.meta.title;
    m_title->setText(note.meta.title);
    // A document a view has laid out is laid out again at every insertion and every change of
    // highlighting, which makes filling it grow with the square of its size. The note is
    // filled and highlighted in a new document that no view shows yet, which then replaces the
    // old one in every view. Filling it is not an edit, so no view schedules a save.
    std::unique_ptr<QTextDocument> document = newNoteDocument();
    auto *presenter = new Presenter(document.get());
    {
        // The presenter highlights the whole document once it is filled.
        const QSignalBlocker blocker(document.get());
        readBody(*document, note.body);
    }
    presenter->rehighlight();
    // The old document lives until no view shows it.
    const std::unique_ptr<QTextDocument> previous =
        std::exchange(m_buffer->document, std::move(document));
    m_buffer->presenter = presenter;
    for (NoteEditor *view : std::as_const(m_buffer->views)) {
        view->m_text->setNoteDocument(m_buffer->document.get());
        view->m_presenter = presenter;
    }
    // The views share the document's image handler, which needs the note's folder.
    m_text->setAttachmentContext(m_session->library().root(), note.meta.id);
    m_notePicker->setExcluded(note.meta.id);
    updateLinkedNotes();
    m_text->document()->setModified(false);
    m_text->document()->clearUndoRedoStacks();
    m_session->markSeen(note.meta.id);
    m_buffer->newNote = false;
    m_notice->hide();
    updateHeader();
    updateReadOnly();
    updateToolbar();
    m_stack->setCurrentWidget(m_page);
    m_info->setVisible(m_infoAction->isChecked());
    updateInfo();
    m_find->refresh();
    for (const auto &[view, position, anchor, scroll] : std::as_const(positions)) {
        QTextCursor cursor(view->m_text->document());
        const int last = view->m_text->document()->characterCount() - 1;
        cursor.setPosition(std::min(anchor, last));
        cursor.setPosition(std::min(position, last), QTextCursor::KeepAnchor);
        view->m_text->setTextCursor(cursor);
        view->m_text->verticalScrollBar()->setValue(scroll);
        view->m_find->refresh();
    }
    refreshViews();
}

void NoteEditor::scheduleSave()
{
    if (m_buffer->note && !m_readOnly && isModified()) {
        m_buffer->autosave.start();
    }
}

bool NoteEditor::isModified() const
{
    return m_buffer->note &&
           (m_text->document()->isModified() || m_title->text() != m_buffer->note->meta.title);
}

bool NoteEditor::saveNow()
{
    m_buffer->autosave.stop();
    if (m_buffer->saving || !m_buffer->note || !isModified()) {
        return true;
    }
    if (m_readOnly || m_session == nullptr) {
        return false;
    }
    return writeNote(*m_buffer->note);
}

bool NoteEditor::writeNote(core::Note note)
{
    const auto baseline = m_buffer->note;
    if (!baseline || m_session == nullptr || m_buffer->saving) {
        return false;
    }
    // A note deleted outside and kept by the person is written again without a version to
    // compare; otherwise the session compares the file with the version this buffer loaded.
    const bool recreating =
        m_buffer->recreate && !QFileInfo::exists(m_session->library().notePath(note.meta.id));
    const QScopedValueRollback<bool> saving(m_buffer->saving, true);
    note.meta.title = m_title->text();
    m_buffer->title = note.meta.title;
    note.body = writeBody(*m_text->document());
    // Saving announces the note at once: lists refresh and may close this note or ask it to
    // save again. The editor takes the saved state before writing, so it does not write the
    // same text in a loop, and returns to the unsaved state if the write fails.
    const std::optional<core::Note> previous = m_buffer->note;
    const bool wasModified = m_text->document()->isModified();
    m_buffer->note = note;
    m_text->document()->setModified(false);
    const auto stillOpen = [&] {
        return m_buffer->note && m_buffer->note->meta.id == note.meta.id;
    };
    if (auto saved = m_session->saveNote(note, recreating ? std::nullopt : previous); !saved) {
        if (stillOpen()) {
            m_buffer->note = previous;
            m_text->document()->setModified(wasModified);
        }
        if (saved.error().code == core::ErrorCode::Deferred && stillOpen()) {
            // The file changed since it was loaded, and a watcher notification may still be
            // queued: the editor takes the change (or keeps the local text as a conflict copy)
            // and saves once more over the version it now holds.
            m_buffer->saving = false;
            checkOutsideChanges();
            if (!m_buffer->note || !isModified()) {
                return m_buffer->note.has_value();
            }
            const auto current = m_session->library().readNote(m_buffer->note->meta.id);
            if (!current || *current != *m_buffer->note) {
                return false;
            }
            return writeNote(*m_buffer->note);
        }
        // Never shown as saved before the write succeeds; the text stays here.
        m_notice->showNotice(
            ui::Notice::Kind::Error,
            tr("The note could not be saved: %1").arg(core::describe(saved.error())), tr("Retry"));
        disconnect(m_notice, nullptr, this, nullptr);
        connect(m_notice, &ui::Notice::actionTriggered, this, [this] {
            if (m_buffer->note) {
                writeNote(*m_buffer->note);
            }
        });
        return false;
    }
    if (stillOpen()) {
        m_buffer->note = note; // With the updated time the session set.
    }
    if (m_notice->property("notice").toString() == QLatin1StringView("error")) {
        m_notice->hide();
    }
    m_buffer->recreate = false;
    refreshViews();
    return true;
}

void NoteEditor::changeMeta(const std::function<void(core::NoteMeta &)> &change)
{
    if (!m_buffer->note || m_readOnly || m_session == nullptr) {
        return;
    }
    core::Note note = *m_buffer->note;
    change(note.meta);
    if (writeNote(note)) {
        updateHeader();
    }
}

void NoteEditor::updateHeader()
{
    if (!m_buffer->note) {
        return;
    }
    // A note without an icon still offers the button, with the Note glyph muted.
    const auto color = ui::colorFromKey(m_buffer->note->meta.color.value_or(QString()));
    const QString glyph = m_buffer->note->meta.icon.value_or(QStringLiteral("Note"));
    m_iconButton->setIcon(color ? ui::icon(glyph, *color) : ui::icon(glyph));
    m_iconButton->setEnabled(!m_readOnly);
    m_tags->setTags(m_buffer->note->meta.tags);
    updateInfo();
    // The title takes its colour from the theme's shade of the tag colour, like the list does.
    m_title->setStyleSheet(
        color ? QStringLiteral("color: %1;")
                    .arg(ui::tagTextColor(*color, ui::Theme::current()->roles()).name())
              : QString());
}

void NoteEditor::showMoreMenu()
{
    if (!m_buffer->note || m_session == nullptr) {
        return;
    }
    const QString id = m_buffer->note->meta.id;
    QMenu menu(this);
    const auto add = [&](const QString &text, NoteAction action, bool danger = false) {
        QAction *entry = menu.addAction(text);
        if (danger) {
            entry->setIcon(ui::icon(QStringLiteral("Delete"), ui::IconTone::Error));
        }
        connect(entry, &QAction::triggered, this,
                [this, action, id] { Q_EMIT actionRequested(action, {id}); });
    };
    const auto &favorites = m_session->library().info().favorites;
    const bool favorite = std::ranges::any_of(favorites, [&](const core::FavoriteRef &ref) {
        return ref.kind == core::FavoriteRef::Kind::Note && ref.id == id;
    });
    if (m_buffer->note->meta.trashed) {
        menu.addAction(m_infoAction);
        menu.addSeparator();
        add(tr("Restore"), NoteAction::Restore);
        menu.addSeparator();
        add(tr("Delete permanently…"), NoteAction::DeletePermanently, true);
    } else {
        add(tr("Collections…"), NoteAction::Collections);
        menu.addAction(m_infoAction);
        // While geotags are off, no location is asked for.
        if (m_locationsEnabled && !m_buffer->note->meta.location && !m_readOnly) {
            connect(menu.addAction(tr("Add location")), &QAction::triggered, this,
                    [this, id] { Q_EMIT locationRequested(id); });
        }
        add(tr("Export…"), NoteAction::Export);
        menu.addSeparator();
        add(favorite ? tr("Remove from favourites") : tr("Add to favourites"),
            favorite ? NoteAction::Unfavorite : NoteAction::Favorite);
        add(m_buffer->note->meta.archived ? tr("Unarchive") : tr("Archive"),
            m_buffer->note->meta.archived ? NoteAction::Unarchive : NoteAction::Archive);
        menu.addSeparator();
        add(tr("Move to trash"), NoteAction::Trash, true);
    }
    menu.exec(m_more->mapToGlobal(QPoint(0, m_more->height())));
}

void NoteEditor::updateReadOnly()
{
    // Disable action shortcuts as well as text input while a worker mutates these notes.
    setEnabled(m_session == nullptr || !m_session->isMutating());
    QString reason;
    if (m_buffer->note && m_session != nullptr) {
        const core::ReadOnlyReason readOnly = m_session->library().readOnlyReason();
        if (readOnly == core::ReadOnlyReason::NewerFormat) {
            reason = tr("This library was made by a newer version of Jotter and is read-only.");
        } else if (readOnly == core::ReadOnlyReason::NoPermission) {
            reason = tr("You may not change the files of this library folder, so it is "
                        "read-only.");
        } else if (m_session->isMutating()) {
            reason = tr("The library is being updated. Editing will resume when it finishes.");
        } else if (!m_session->isAvailable()) {
            reason = tr("The library folder cannot be reached, so the note cannot be changed.");
        } else if (m_buffer->note->meta.trashed) {
            reason = tr("This note is in the trash. Restore it to edit it.");
        } else if (m_buffer->note->meta.archived) {
            reason = tr("This note is archived. Unarchive it to edit it.");
        }
    }
    m_readOnly = !reason.isEmpty();
    m_title->setReadOnly(m_readOnly);
    m_text->setReadOnly(m_readOnly);
    m_toolbar->setEnabled(!m_readOnly);
    m_iconButton->setEnabled(!m_readOnly);
    m_tags->setReadOnly(m_readOnly);
    m_find->setReadOnly(m_readOnly);
    if (m_readOnly && m_buffer->note) {
        // Trashed and archived notes offer the way back right in the notice.
        const core::NoteMeta &meta = m_buffer->note->meta;
        const bool writable = !m_session->library().isReadOnly() && m_session->isAvailable();
        QString action;
        std::optional<NoteAction> back;
        if (meta.trashed && writable) {
            action = tr("Restore");
            back = NoteAction::Restore;
        } else if (meta.archived && writable) {
            action = tr("Unarchive");
            back = NoteAction::Unarchive;
        }
        m_notice->showNotice(ui::Notice::Kind::Info, reason, action);
        disconnect(m_notice, nullptr, this, nullptr);
        if (back) {
            const QString id = meta.id;
            connect(m_notice, &ui::Notice::actionTriggered, this,
                    [this, back, id] { Q_EMIT actionRequested(*back, {id}); });
        }
    } else if (m_notice->property("notice").toString() == QLatin1StringView("info") &&
               m_notice->isVisible()) {
        m_notice->hide();
    }
}

void NoteEditor::updateToolbar()
{
    const QTextCursor cursor = m_text->textCursor();
    const QTextCharFormat format = m_text->currentCharFormat();
    m_bold->setChecked(format.fontWeight() >= QFont::Bold);
    m_italic->setChecked(format.fontItalic());
    m_underline->setChecked(format.fontUnderline());
    m_strike->setChecked(format.fontStrikeOut());
    m_superscript->setChecked(format.verticalAlignment() == QTextCharFormat::AlignSuperScript);
    m_subscript->setChecked(format.verticalAlignment() == QTextCharFormat::AlignSubScript);
    m_inlineCode->setChecked(format.boolProperty(InlineCode));
    const auto kind = listKind(cursor);
    m_bulleted->setChecked(kind == ListKind::Bulleted);
    m_numbered->setChecked(kind == ListKind::Numbered);
    m_checklist->setChecked(kind == ListKind::Checklist);
    m_outdent->setEnabled(kind.has_value());
    m_indent->setEnabled(kind.has_value());
    // A quote is a paragraph with the Quote button on.
    const BlockStyle current = blockStyle(cursor);
    m_quote->setChecked(current == BlockStyle::Quote);
    QAction *style = m_styles.at(
        static_cast<std::size_t>(current == BlockStyle::Quote ? BlockStyle::Paragraph : current));
    style->setChecked(true);
    m_blockStyle->setIcon(style->icon());
    // Code is plain text: formatting, styles, lists and objects do not apply inside a code
    // block. Table cells hold paragraphs, inline formatting and lists only.
    const bool code = codeBlockOf(cursor.block()) != nullptr;
    const bool cell = cursor.currentTable() != nullptr;
    for (QAction *action :
         {m_bold, m_italic, m_underline, m_strike, m_superscript, m_subscript, m_inlineCode, m_link,
          m_bulleted, m_numbered, m_checklist, m_image, m_attachment}) {
        action->setEnabled(!code);
    }
    for (QAction *action : {m_table, m_divider}) {
        action->setEnabled(!code && !cell);
    }
    // A selection that takes a code block or a table does not become a code block.
    m_codeBlock->setEnabled(canInsertCodeBlock(cursor));
    for (QAction *action : m_styles) {
        action->setEnabled(!code && !cell);
    }
    m_quote->setEnabled(!code && !cell);
    m_blockStyle->setEnabled(!code && !cell);
    m_textColor->setEnabled(!code);
    m_highlight->setEnabled(!code);
}

void NoteEditor::editLink()
{
    QTextCursor cursor = m_text->textCursor();
    const bool existing = cursor.charFormat().isAnchor() && selectLink(cursor);
    const QString address = existing ? cursor.charFormat().anchorHref() : QString();
    const auto link = askLink(this, cursor.selectedText(), address, existing, m_session,
                              m_buffer->note ? m_buffer->note->meta.id : QString());
    if (!link) {
        return;
    }
    if (link->first.isEmpty()) {
        removeLink(cursor);
    } else {
        setLink(cursor, link->first, link->second);
    }
    updateLinkedNotes();
    m_text->setFocus();
}

void NoteEditor::checkOutsideChanges()
{
    if (!m_buffer->note || m_session == nullptr) {
        return;
    }
    const QString id = m_buffer->note->meta.id;
    if (!QFile::exists(m_session->library().notePath(id))) {
        // A note deleted outside while open can be kept (written again) or closed.
        m_notice->showNotice(ui::Notice::Kind::Warning, tr("This note was deleted outside Jotter."),
                             tr("Keep"), tr("Close"));
        disconnect(m_notice, nullptr, this, nullptr);
        connect(m_notice, &ui::Notice::actionTriggered, this, [this] {
            m_buffer->recreate = true;
            m_text->document()->setModified(true);
            saveNow();
        });
        connect(m_notice, &ui::Notice::secondaryActionTriggered, this, [this] {
            m_buffer->note.reset();
            m_stack->setCurrentWidget(m_empty);
            Q_EMIT titleChanged(QString());
            Q_EMIT noteClosed();
        });
        return;
    }
    // The file is read once; every decision below works on this version.
    const auto disk = m_session->library().readNote(id);
    if (!disk) {
        m_notice->showNotice(ui::Notice::Kind::Error, core::describe(disk.error()));
        return;
    }
    if (!m_session->changedOutside(id) && !(isModified() && disk->body != m_buffer->note->body)) {
        // Jotter itself may have changed the note's metadata, such as renaming one of its tags
        // from the sidebar, or its text, such as the lists of a deleted numbering style; the
        // editor takes the change, so the next save keeps it. Unsaved typing wins over a
        // changed text.
        if (const core::Note &current = *disk; current != *m_buffer->note) {
            if (current.body != m_buffer->note->body && !isModified()) {
                const int position = m_text->textCursor().position();
                load(current);
                QTextCursor cursor = m_text->textCursor();
                cursor.setPosition(std::min(position, m_text->document()->characterCount() - 1));
                m_text->setTextCursor(cursor);
                return;
            }
            const bool titleEdited = m_buffer->title != m_buffer->note->meta.title;
            if (!titleEdited) {
                m_buffer->title = current.meta.title;
            }
            m_buffer->note->meta = current.meta;
            refreshViews();
        }
        return;
    }
    const core::Note &outside = *disk;
    const bool conflict = isModified();
    QString copyId;
    QString copyTitle;
    if (conflict) {
        // The outside version is kept; the local text becomes a new note beside it.
        core::Note copy = core::makeNote();
        copyTitle =
            tr("%1 (conflict, %2)")
                .arg(m_title->text().isEmpty() ? tr("Untitled") : m_title->text(),
                     QLocale().toString(QDateTime::currentDateTime(), QLocale::ShortFormat));
        copy.meta.title = copyTitle;
        copy.meta.collections = m_buffer->note->meta.collections;
        copy.meta.tags = m_buffer->note->meta.tags;
        copy.body = writeBody(*m_text->document());
        if (auto copied = core::copyNoteAttachments(m_session->library().root(), copy); !copied) {
            m_notice->showNotice(ui::Notice::Kind::Error, core::describe(copied.error()));
            return;
        }
        const QScopedValueRollback<bool> saving(m_buffer->saving, true);
        if (auto saved = m_session->saveNote(copy); saved) {
            copyId = copy.meta.id;
        } else {
            m_notice->showNotice(ui::Notice::Kind::Error, core::describe(saved.error()));
            return;
        }
    }
    const int position = m_text->textCursor().position();
    load(outside);
    QTextCursor cursor = m_text->textCursor();
    cursor.setPosition(std::min(position, m_text->document()->characterCount() - 1));
    m_text->setTextCursor(cursor);
    if (!copyId.isEmpty()) {
        m_notice->showNotice(ui::Notice::Kind::Info,
                             tr("This note was changed outside Jotter. Your version was saved as "
                                "“%1”.")
                                 .arg(copyTitle),
                             tr("Open copy"));
        disconnect(m_notice, nullptr, this, nullptr);
        connect(m_notice, &ui::Notice::actionTriggered, this,
                [this, copyId] { Q_EMIT openNoteRequested(copyId); });
    }
}

} // namespace editor
