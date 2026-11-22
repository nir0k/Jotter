#include "editor/TextEdit.h"

#include "core/Attachments.h"
#include "core/NoteLinks.h"
#include "editor/Commands.h"
#include "editor/Formats.h"
#include "editor/HtmlCodec.h"
#include "editor/LanguagePopup.h"
#include "editor/Markdown.h"
#include "editor/Objects.h"
#include "editor/Presenter.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextFrame>
#include <QTextTable>
#include <QToolTip>
#include <QUrl>

#include <memory>
#include <utility>

namespace editor {

TextEdit::TextEdit(QWidget *parent)
    : QTextEdit(parent)
    , m_titleEdit(new QLineEdit(viewport()))
    , m_languages(new LanguagePopup(this))
    , m_objects(new ObjectHandler(document()))
{
    setAcceptRichText(false);
    setTabChangesFocus(false);
    // Long words and addresses wrap inside the text column and table cells instead of widening
    // them.
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    connect(m_objects, &ObjectHandler::imageLoaded, viewport(), qOverload<>(&QWidget::update));
    connect(this, &QTextEdit::textChanged, this, [this] { m_markersValid = false; });
    // Backspace undoes an automatic change only right after it, before the cursor moves on.
    connect(this, &QTextEdit::cursorPositionChanged, this, [this] { m_autoFormat.reset(); });
    // Text typed next to an image or a chip must not take the object's properties along.
    connect(this, &QTextEdit::currentCharFormatChanged, this,
            [this](const QTextCharFormat &format) {
                if (textCursor().hasSelection() ||
                    !(isObject(format) || format.hasProperty(ObjectFile))) {
                    return;
                }
                QTextCharFormat text = format;
                for (const int property :
                     {static_cast<int>(QTextFormat::ObjectType), static_cast<int>(ObjectNote),
                      static_cast<int>(ObjectFile), static_cast<int>(ImageWidth),
                      static_cast<int>(AttachmentSize), static_cast<int>(AttachmentName),
                      static_cast<int>(PendingSource), static_cast<int>(ImageWrapMode),
                      static_cast<int>(QTextFormat::TextVerticalAlignment)}) {
                    text.clearProperty(property);
                }
                setCurrentCharFormat(text);
            });
    viewport()->setMouseTracking(true);
    viewport()->installEventFilter(this);
    m_titleEdit->setFrame(false);
    m_titleEdit->hide();
    m_titleEdit->installEventFilter(this);
    connect(m_titleEdit, &QLineEdit::editingFinished, this, [this] { finishTitle(true); });
    // The title field stays over its header only until the text scrolls.
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { finishTitle(true); });
    // A code block shows its title placeholder while the cursor is in it.
    connect(this, &QTextEdit::cursorPositionChanged, this, [this] {
        if (QTextFrame *code = codeBlockOf(textCursor().block()); code != m_cursorCode) {
            m_cursorCode = code;
            viewport()->update();
        }
    });
    connect(m_languages, &LanguagePopup::chosen, this, [this](const QString &key) {
        if (m_languageFrame != nullptr) {
            setCodeBlockProperty(*m_languageFrame, CodeLanguage, key);
        }
        setFocus();
    });
}

TextEdit::~TextEdit() = default;

void TextEdit::setSideMargin(int margin)
{
    setViewportMargins(margin, 0, margin, 0);
}

void TextEdit::setNoteDocument(QTextDocument *document)
{
    if (m_objects != nullptr) {
        disconnect(m_objects, nullptr, viewport(), nullptr);
    }
    QTextEdit::setDocument(document);
    m_objects = document->findChild<ObjectHandler *>();
    if (m_objects == nullptr) {
        m_objects = new ObjectHandler(document);
    }
    connect(m_objects, &ObjectHandler::imageLoaded, viewport(), qOverload<>(&QWidget::update));
    m_markersValid = false;
    m_cursorCode = nullptr;
    m_languageFrame = nullptr;
}

void TextEdit::setAttachmentContext(const QString &libraryRoot, const QString &noteId)
{
    m_libraryRoot = libraryRoot;
    m_noteId = noteId;
    m_objects->setLibraryRoot(libraryRoot);
}

QString TextEdit::attachmentFolder() const
{
    if (m_libraryRoot.isEmpty() || m_noteId.isEmpty()) {
        return {};
    }
    return core::attachmentsDir(m_libraryRoot, m_noteId);
}

void TextEdit::paintEvent(QPaintEvent *event)
{
    const QList<CodeHeader> headers = codeHeaders();
    const QList<TableGeometry> tables = tableGeometries();
    {
        const ui::Roles &roles = ui::Theme::current()->roles();
        QPainter background(viewport());
        paintCodeBackgrounds(background, headers, roles);
        paintTables(background, tables, roles);
        paintInlineCode(background, cursorForPosition(QPoint(0, 0)).block(), roles,
                        QPointF(-horizontalScrollBar()->value(), -verticalScrollBar()->value()),
                        viewport()->height());
    }
    QTextEdit::paintEvent(event);
    QPainter painter(viewport());
    paintCodeHeaders(painter, headers);
    paintCodeLineNumbers(painter, headers, ui::Theme::current()->roles(),
                         QPointF(-horizontalScrollBar()->value(), -verticalScrollBar()->value()),
                         viewport()->height());
    paintBlockDecorations(painter);
    paintTableGrips(painter, tables);
    paintDragFeedback(painter);
}

void TextEdit::paintBlockDecorations(QPainter &painter)
{
    if (!m_markersValid) {
        m_markers = listMarkers(*document());
        m_markersValid = true;
    }
    editor::paintBlockDecorations(
        painter, cursorForPosition(QPoint(0, 0)).block(), m_markers, ui::Theme::current()->roles(),
        QPointF(-horizontalScrollBar()->value(), -verticalScrollBar()->value()),
        viewport()->height());
}

void TextEdit::keyPressEvent(QKeyEvent *event)
{
    const auto modifiers = event->modifiers() & ~Qt::KeypadModifier;
    QTextCursor cursor = textCursor();
    if (!isReadOnly() && event->text().size() == 1 &&
        wrapSelection(cursor, event->text().front())) {
        setTextCursor(cursor);
        return;
    }
    switch (event->key()) {
    case Qt::Key_Tab:
        // A list item in a table cell changes its level; elsewhere in a table Tab moves on.
        if (modifiers == Qt::NoModifier &&
            (indentCode(cursor, 1) || changeListLevel(cursor, 1) || moveToCell(cursor, 1))) {
            if (cursor.currentTable() != nullptr) {
                setTextCursor(cursor);
            }
            return;
        }
        break;
    case Qt::Key_Backtab:
        if (indentCode(cursor, -1) || changeListLevel(cursor, -1) || moveToCell(cursor, -1)) {
            if (cursor.currentTable() != nullptr) {
                setTextCursor(cursor);
            }
            return;
        }
        break;
    case Qt::Key_C:
        if (modifiers == (Qt::ControlModifier | Qt::ShiftModifier)) {
            copyMarkdown();
            return;
        }
        break;
    case Qt::Key_V:
        if (modifiers == (Qt::ControlModifier | Qt::ShiftModifier)) {
            pastePlainText();
            return;
        }
        break;
    case Qt::Key_Right:
        if (modifiers == Qt::NoModifier && leaveInlineCode(cursor)) {
            setTextCursor(cursor);
            return;
        }
        break;
    case Qt::Key_Backspace:
        if (modifiers == Qt::NoModifier && undoAutoFormat()) {
            return;
        }
        if (modifiers == Qt::NoModifier && removeEmptyCodeBlock(cursor)) {
            setTextCursor(cursor);
            return;
        }
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (modifiers == Qt::ControlModifier) {
            if (leaveCodeBlock(cursor)) {
                setTextCursor(cursor);
            } else {
                toggleChecked(cursor);
            }
            return;
        }
        if (modifiers == Qt::NoModifier) {
            handleReturn(event);
            return;
        }
        break;
    default:
        break;
    }
    QTextEdit::keyPressEvent(event);
    const int typedAt = textCursor().position();
    if (event->key() == Qt::Key_Space && modifiers == Qt::NoModifier &&
        codeBlockOf(textCursor().block()) == nullptr &&
        (applyMarkdownPrefix(textCursor()) || linkTypedAddress())) {
        markAutoFormat(typedAt);
    }
    if (event->text() == QLatin1StringView("`")) {
        if (QTextCursor typed = textCursor(); applyMarkdownCode(typed)) {
            setTextCursor(typed);
            markAutoFormat(typedAt);
        }
    }
    // `[[` opens the note picker; the typed characters stay until a note is chosen.
    if (event->text() == QLatin1StringView("[") && !isReadOnly() &&
        codeBlockOf(textCursor().block()) == nullptr) {
        const QTextCursor typed = textCursor();
        const qsizetype at = typed.positionInBlock();
        if (at >= 2 && typed.block().text().sliced(at - 2, 2) == QLatin1StringView("[[")) {
            Q_EMIT noteLinkRequested();
        }
    }
}

void TextEdit::setLinkedNotes(QHash<QString, core::NoteSummary> notes)
{
    m_linkedNotes = std::move(notes);
}

void TextEdit::handleReturn(QKeyEvent *event)
{
    QTextCursor cursor = textCursor();
    const QTextBlock block = cursor.block();
    if (codeBlockOf(block) != nullptr) {
        // A new line of code starts with the indentation of the line it splits.
        const QString text = block.text().left(cursor.positionInBlock());
        qsizetype indent = 0;
        while (indent < text.size() && (text.at(indent) == u' ' || text.at(indent) == u'\t')) {
            ++indent;
        }
        cursor.beginEditBlock();
        cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
        cursor.insertText(text.left(indent));
        cursor.endEditBlock();
        setTextCursor(cursor);
        return;
    }
    const bool empty = block.text().isEmpty();
    if (empty && block.textList() != nullptr) {
        changeListLevel(cursor, -1);
        return;
    }
    if (empty && blockStyle(cursor) == BlockStyle::Quote) {
        setBlockStyle(cursor, BlockStyle::Paragraph);
        return;
    }
    const int typedAt = cursor.position();
    if (applyMarkdownLine(cursor)) {
        setTextCursor(cursor);
        markAutoFormat(typedAt);
        return;
    }
    const bool heading = block.blockFormat().headingLevel() > 0;
    const bool atEnd = cursor.atBlockEnd();
    const bool leaveCode = atEnd && cursor.charFormat().boolProperty(InlineCode);
    QTextCursor editBlock = cursor;
    if (leaveCode) {
        editBlock.beginEditBlock();
    }
    QTextEdit::keyPressEvent(event);
    cursor = textCursor();
    if (leaveCode) {
        QTextCharFormat plain = cursor.charFormat();
        plain.clearProperty(InlineCode);
        cursor.setCharFormat(plain);
        cursor.setBlockCharFormat(plain);
        setTextCursor(cursor);
    }
    QTextBlockFormat format = cursor.blockFormat();
    if (heading && atEnd) {
        format.setHeadingLevel(0);
        cursor.setBlockFormat(format);
    }
    if (format.marker() == QTextBlockFormat::MarkerType::Checked) {
        format.setMarker(QTextBlockFormat::MarkerType::Unchecked);
        cursor.setBlockFormat(format);
    }
    if (leaveCode) {
        editBlock.endEditBlock();
    }
}

void TextEdit::mousePressEvent(QMouseEvent *event)
{
    const QPoint position = event->position().toPoint();
    if (event->button() == Qt::LeftButton && clickCodeHeader(position)) {
        return;
    }
    if (event->button() == Qt::LeftButton && !isReadOnly()) {
        if (startColumnDrag(position)) {
            return;
        }
        if (const auto image = selectedImage()) {
            const QRectF rect = objectRect(*image);
            const auto handles = imageHandles(rect);
            for (std::size_t i = 0; i < handles.size(); ++i) {
                if (handles.at(i).contains(position)) {
                    m_imageDrag = ImageDrag{.image = *image,
                                            .start = rect,
                                            .fromLeft = i % 2 == 0,
                                            .width = rect.width()};
                    return;
                }
            }
        }
        if (QTextTable *table = tableGripAt(position)) {
            QTextCursor content(document());
            content.setPosition(table->firstPosition() - 1);
            content.setPosition(table->lastPosition() + 1, QTextCursor::KeepAnchor);
            m_press = MoveSource{.content = content, .table = table, .start = position};
            return;
        }
        // A click on an image selects it, which shows its handles. Images and chips move when
        // dragged further.
        if (const auto object = objectAt(position);
            object && event->modifiers() == Qt::NoModifier) {
            if (object->charFormat().objectType() == ImageObject) {
                setTextCursor(*object);
                viewport()->update();
            }
            m_press = MoveSource{.content = *object, .table = nullptr, .start = position};
            return;
        }
    }
    if (event->button() == Qt::LeftButton && !isReadOnly() &&
        toggleCheckboxAt(event->position().toPoint())) {
        return;
    }
    QTextEdit::mousePressEvent(event);
}

void TextEdit::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_columnDrag && event->button() == Qt::LeftButton) {
        finishColumnDrag();
        return;
    }
    if (m_press && event->button() == Qt::LeftButton) {
        // Without a drag, a click on a chip places the cursor and one on a grip goes into the
        // table, so its menu is in reach of the keyboard.
        const MoveSource press = *m_press;
        m_press.reset();
        if (press.table != nullptr) {
            setTextCursor(press.table->cellAt(0, 0).firstCursorPosition());
        } else if (press.content.charFormat().objectType() != ImageObject) {
            setTextCursor(cursorForPosition(press.start));
        }
        return;
    }
    if (m_imageDrag && event->button() == Qt::LeftButton) {
        finishImageDrag();
        return;
    }
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ControlModifier) != 0) {
        const QString href = anchorAt(event->position().toPoint());
        if (const auto target = core::noteLinkTarget(href)) {
            Q_EMIT noteLinkActivated(*target);
            return;
        }
        if (!href.isEmpty()) {
            // Only web and mail links are handed to the desktop; it would run other schemes,
            // such as a local desktop entry, as programs.
            if (core::isWebLink(href)) {
                QDesktopServices::openUrl(QUrl(href, QUrl::StrictMode));
            }
            return;
        }
    }
    QTextEdit::mouseReleaseEvent(event);
}

void TextEdit::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint position = event->position().toPoint();
    if (m_columnDrag) {
        if (m_columnDrag->column < 0) {
            // The table keeps at least its minimum width and stays within the text column.
            m_columnDrag->x = std::clamp<qreal>(
                position.x(), m_columnDrag->left + (m_columnDrag->tableWidth * MinWidth / 100.0),
                m_columnDrag->right);
        } else {
            // Each of the two columns keeps at least its minimum width.
            const qreal minimum = m_columnDrag->tableWidth * MinColumnWidth / 100.0;
            m_columnDrag->x = std::clamp<qreal>(
                position.x(), m_columnDrag->left + minimum,
                std::max(m_columnDrag->left + minimum, m_columnDrag->right - minimum));
        }
        viewport()->update();
        return;
    }
    if (m_press) {
        if ((position - m_press->start).manhattanLength() >= QApplication::startDragDistance()) {
            const MoveSource source = *m_press;
            m_press.reset();
            startMove(source);
        }
        return;
    }
    if (m_imageDrag) {
        const QRectF &start = m_imageDrag->start;
        const qreal available =
            m_objects->availableWidth(document(), m_imageDrag->image.selectionStart());
        const qreal width =
            m_imageDrag->fromLeft ? start.right() - position.x() : position.x() - start.left();
        m_imageDrag->width =
            std::clamp(width, available * MinWidth / 100.0, std::max(1.0, available));
        viewport()->update();
        return;
    }
    if (!isReadOnly()) {
        // A table shows its grip while the mouse is over it or its grip.
        const QList<TableGeometry> tables = tableGeometries();
        QTextTable *hover = nullptr;
        for (const TableGeometry &geometry : tables) {
            if (geometry.box.united(tableGrip(geometry)).contains(position)) {
                hover = geometry.table;
            }
        }
        if (hover != m_hoverTable) {
            m_hoverTable = hover;
            viewport()->update();
        }
        // Column lines, right edges, grips and image handles show what dragging them does.
        const qreal reach = ui::Theme::current()->px(0.25);
        for (const TableGeometry &geometry : tables) {
            const auto near = [&](qreal x) { return std::abs(position.x() - x) <= reach; };
            const bool onLine =
                position.y() >= geometry.box.top() && position.y() <= geometry.box.bottom() &&
                (std::ranges::any_of(geometry.columnLines, near) || near(geometry.box.right()));
            if (onLine || tableGrip(geometry).contains(position)) {
                viewport()->setCursor(onLine ? Qt::SplitHCursor : Qt::OpenHandCursor);
                viewport()->update();
                QTextEdit::mouseMoveEvent(event);
                return;
            }
        }
        if (const auto image = selectedImage()) {
            const auto handles = imageHandles(objectRect(*image));
            for (std::size_t i = 0; i < handles.size(); ++i) {
                if (handles.at(i).contains(position)) {
                    viewport()->setCursor(i == 0 || i == 3 ? Qt::SizeFDiagCursor
                                                           : Qt::SizeBDiagCursor);
                    QTextEdit::mouseMoveEvent(event);
                    return;
                }
            }
        }
    }
    const auto header = codeHeaderAt(position);
    if (QTextFrame *hover = header ? header->frame : nullptr; hover != m_hoverCode) {
        m_hoverCode = hover;
        viewport()->update();
    }
    const bool onButton = header && (header->copy.contains(position) ||
                                     (!isReadOnly() && header->language.contains(position)));
    const bool onLink =
        (event->modifiers() & Qt::ControlModifier) != 0 && !anchorAt(position).isEmpty();
    viewport()->setCursor(onLink || onButton ? Qt::PointingHandCursor : Qt::IBeamCursor);
    QTextEdit::mouseMoveEvent(event);
}

void TextEdit::contextMenuEvent(QContextMenuEvent *event)
{
    const std::unique_ptr<QMenu> menu(createStandardContextMenu());
    menu->addSeparator();
    QAction *markdown = menu->addAction(tr("Copy as Markdown"), this, &TextEdit::copyMarkdown);
    markdown->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    markdown->setEnabled(textCursor().hasSelection());
    if (!isReadOnly()) {
        QAction *plain =
            menu->addAction(tr("Paste without formatting"), this, &TextEdit::pastePlainText);
        plain->setObjectName(QStringLiteral("pastePlainText"));
        plain->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
        const QMimeData *clipboard = QGuiApplication::clipboard()->mimeData();
        plain->setEnabled(clipboard != nullptr && clipboard->hasText());
    }
    // A code block's header controls are in its menu too, so they work from the keyboard. The
    // event comes from the viewport, in its coordinates.
    const QTextCursor cursor = event->reason() == QContextMenuEvent::Mouse
                                   ? cursorForPosition(event->pos())
                                   : textCursor();
    addObjectActions(*menu,
                     event->reason() == QContextMenuEvent::Mouse
                         ? objectAt(event->pos()).value_or(cursor)
                         : objectAtCursor().value_or(cursor),
                     event->globalPos());
    if (QTextFrame *frame = codeBlockOf(cursor.block())) {
        menu->addSeparator();
        const QPointer<QTextFrame> code(frame);
        menu->addAction(tr("Copy code"), this, [this, code] {
            if (code != nullptr) {
                copyCode(*code);
            }
        });
        if (!isReadOnly()) {
            const QPoint position = event->globalPos();
            menu->addAction(tr("Language…"), this,
                            [this, code, position] { chooseLanguage(code, position); });
            menu->addAction(tr("Title…"), this, [this, code] { editTitle(code); });
        }
    }
    menu->exec(event->globalPos());
}

bool TextEdit::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        const QPoint position = viewport()->mapFrom(this, help->pos());
        const QString href = anchorAt(position);
        const auto header = codeHeaderAt(position);
        if (const auto target = core::noteLinkTarget(href)) {
            const auto note = m_linkedNotes.constFind(*target);
            QString tip = tr("Note not found");
            if (note != m_linkedNotes.constEnd()) {
                const QString title = note->title.isEmpty() ? tr("Untitled") : note->title;
                tip = (note->trashed ? tr("%1 (in the trash)\nCtrl+click to open")
                                     : tr("%1\nCtrl+click to open"))
                          .arg(title);
            }
            QToolTip::showText(help->globalPos(), tip, this);
        } else if (!href.isEmpty()) {
            QToolTip::showText(help->globalPos(), tr("%1\nCtrl+click to open").arg(href), this);
        } else if (header && header->copy.contains(position)) {
            QToolTip::showText(help->globalPos(), tr("Copy code"), this);
        } else if (!isReadOnly() && tableGripAt(position) != nullptr) {
            QToolTip::showText(help->globalPos(), tr("Drag to move the table"), this);
        } else if (header && header->language.contains(position) && !isReadOnly()) {
            QToolTip::showText(help->globalPos(), tr("Language"), this);
        } else {
            QToolTip::hideText();
        }
        return true;
    }
    return QTextEdit::event(event);
}

bool TextEdit::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == viewport() && event->type() == QEvent::Leave &&
        (m_hoverCode != nullptr || m_hoverTable != nullptr)) {
        m_hoverCode = nullptr;
        m_hoverTable = nullptr;
        viewport()->update();
    } else if (watched == m_titleEdit && event->type() == QEvent::KeyPress &&
               static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
        finishTitle(false);
        return true;
    }
    return QTextEdit::eventFilter(watched, event);
}

void TextEdit::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        if (const auto object = objectAt(event->position().toPoint())) {
            openObject(*object);
            return;
        }
    }
    QTextEdit::mouseDoubleClickEvent(event);
}

bool TextEdit::toggleCheckboxAt(const QPoint &viewportPosition)
{
    const QTextCursor hit = cursorForPosition(viewportPosition);
    const QTextBlock block = hit.block();
    if (block.blockFormat().marker() == QTextBlockFormat::MarkerType::NoMarker) {
        return false;
    }
    // The checkbox is drawn in the list indent, left of the block's first line of text.
    const QRectF blockRect = document()->documentLayout()->blockBoundingRect(block);
    const QPointF position(viewportPosition.x() + horizontalScrollBar()->value(),
                           viewportPosition.y() + verticalScrollBar()->value());
    const qreal textLeft = blockRect.left() + block.layout()->lineAt(0).x();
    if (position.x() >= textLeft || position.y() < blockRect.top() ||
        position.y() > blockRect.bottom()) {
        return false;
    }
    return toggleChecked(hit);
}

bool TextEdit::linkTypedAddress()
{
    static const QRegularExpression address(QStringLiteral(R"(^https?://\S+$)"),
                                            QRegularExpression::CaseInsensitiveOption);
    const QTextCursor cursor = textCursor();
    const QString text = cursor.block().text();
    const int end = cursor.positionInBlock() - 1; // Before the space just typed.
    int start = end;
    while (start > 0 && !text.at(start - 1).isSpace()) {
        --start;
    }
    QString word = text.mid(start, end - start);
    // Closing punctuation belongs to the sentence, not to the address.
    while (!word.isEmpty() && QStringLiteral(".,;:!?)]}\"'").contains(word.back())) {
        word.chop(1);
    }
    if (!address.match(word).hasMatch()) {
        return false;
    }
    QTextCursor range(cursor.block());
    range.setPosition(cursor.block().position() + start);
    range.setPosition(cursor.block().position() + start + static_cast<int>(word.size()),
                      QTextCursor::KeepAnchor);
    if (range.charFormat().isAnchor()) {
        return false;
    }
    const QString href = core::normaliseLink(word);
    if (href.isEmpty()) {
        return false;
    }
    setLink(range, href, {});
    return true;
}

void TextEdit::markAutoFormat(int typedAt)
{
    m_autoFormat = AutoFormat{.undoSteps = document()->availableUndoSteps(),
                              .position = textCursor().position(),
                              .typedAt = typedAt};
}

bool TextEdit::undoAutoFormat()
{
    const std::optional<AutoFormat> change = std::exchange(m_autoFormat, std::nullopt);
    if (!change || isReadOnly() || textCursor().hasSelection() ||
        textCursor().position() != change->position ||
        document()->availableUndoSteps() != change->undoSteps) {
        return false;
    }
    undo();
    // Undo leaves the cursor where the change was made, which for a link is before the space.
    QTextCursor typed = textCursor();
    typed.setPosition(std::clamp(change->typedAt, 0, document()->characterCount() - 1));
    setTextCursor(typed);
    return true;
}

} // namespace editor
