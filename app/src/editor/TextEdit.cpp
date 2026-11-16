#include "editor/TextEdit.h"

#include "core/Attachments.h"
#include "core/Directory.h"
#include "core/Files.h"
#include "core/NoteLinks.h"
#include "editor/Commands.h"
#include "editor/Formats.h"
#include "editor/HtmlCodec.h"
#include "editor/LanguagePopup.h"
#include "editor/Markdown.h"
#include "editor/Objects.h"
#include "editor/Presenter.h"
#include "editor/Syntax.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QActionGroup>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>
#include <QThreadPool>
#include <QToolTip>
#include <QUrl>
#include <QtConcurrentRun>

#include <memory>

namespace editor {

namespace {

/// Renames an attachment's file back and forth with undo and redo, so the chip and its file
/// never part.
bool isObject(const QTextFormat &format)
{
    return format.objectType() == ImageObject || format.objectType() == AttachmentObject;
}

/// Returns the clipboard and drag data of note content: the body vocabulary, which Jotter reads
/// back without loss, as HTML for other programs, and the plain text.
QMimeData *bodyMimeData(const QTextDocumentFragment &fragment)
{
    // Qt's own clipboard data lists a fixed set of formats, so it is not extended but replaced.
    auto *mime = new QMimeData;
    QTextDocument content;
    QTextCursor(&content).insertFragment(fragment);
    const QString body = writeBody(content);
    mime->setData(TextEdit::BodyMimeType, body.toUtf8());
    mime->setHtml(body);
    mime->setText(fragment.toPlainText().remove(QChar::ObjectReplacementCharacter));
    return mime;
}

/// Returns a width set by dragging, in percent of its column: the widths of the image menu when
/// within 3 % of them, and no less than MinWidth.
int snappedWidth(qreal percent)
{
    for (const int snap : {25, 50, 75, 100}) {
        if (std::abs(percent - snap) <= 3) {
            percent = snap;
        }
    }
    return std::clamp(static_cast<int>(std::lround(percent)), MinWidth, 100);
}

/// The bytes and the file name of a picture in a `data:` URL, as pasted from a web page.
std::optional<std::pair<QByteArray, QString>> decodeDataUrl(const QString &url)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"(^data:image/(png|jpeg|gif|webp);base64,(.*)$)"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = pattern.match(url);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    const QString type = match.captured(1);
    return std::pair{QByteArray::fromBase64(match.captured(2).toLatin1()),
                     TextEdit::tr("Pasted image") + u'.' +
                         (type == QLatin1StringView("jpeg") ? QStringLiteral("jpg") : type)};
}

/// Returns the folder that holds the snapshots of opened attachments, one `open-*` folder each.
QString snapshotFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
           QStringLiteral("/jotter");
}

} // namespace

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

QList<TableGeometry> TextEdit::tableGeometries() const
{
    return editor::tableGeometries(
        *document(), QPointF(-horizontalScrollBar()->value(), -verticalScrollBar()->value()),
        viewport()->height());
}

bool TextEdit::startColumnDrag(const QPoint &viewportPosition)
{
    const qreal reach = ui::Theme::current()->px(0.25);
    for (const TableGeometry &geometry : tableGeometries()) {
        if (viewportPosition.y() < geometry.box.top() ||
            viewportPosition.y() > geometry.box.bottom()) {
            continue;
        }
        const QList<qreal> &lines = geometry.columnLines;
        for (qsizetype i = 0; i < lines.size(); ++i) {
            if (std::abs(viewportPosition.x() - lines.at(i)) > reach) {
                continue;
            }
            m_columnDrag =
                ColumnDrag{.table = geometry.table,
                           .column = static_cast<int>(i) + 1,
                           .tableWidth = geometry.box.width(),
                           .left = i == 0 ? geometry.box.left() : lines.at(i - 1),
                           .right = i + 1 < lines.size() ? lines.at(i + 1) : geometry.box.right(),
                           .x = lines.at(i)};
            return true;
        }
        if (std::abs(viewportPosition.x() - geometry.box.right()) <= reach) {
            // The table's width is a share of the text column, from the table's left edge.
            const qreal column = document()->textWidth() - (2 * document()->documentMargin());
            m_columnDrag = ColumnDrag{.table = geometry.table,
                                      .column = -1,
                                      .tableWidth = column,
                                      .left = geometry.box.left(),
                                      .right = geometry.box.left() + column,
                                      .x = geometry.box.right()};
            return true;
        }
    }
    return false;
}

void TextEdit::finishColumnDrag()
{
    if (!m_columnDrag) {
        return;
    }
    const ColumnDrag drag = *m_columnDrag;
    m_columnDrag.reset();
    viewport()->update();
    if (drag.table == nullptr || drag.right <= drag.left) {
        return;
    }
    if (drag.column < 0) {
        setTableWidth(*drag.table,
                      snappedWidth((drag.x - drag.left) * 100.0 / std::max(1.0, drag.tableWidth)));
        return;
    }
    QList<qreal> widths = columnWidths(drag.table->format(), drag.table->columns());
    const qreal pair = widths.at(drag.column - 1) + widths.at(drag.column);
    widths[drag.column - 1] = pair * (drag.x - drag.left) / (drag.right - drag.left);
    widths[drag.column] = pair - widths.at(drag.column - 1);
    resizeColumns(*drag.table, widths);
}

QRectF TextEdit::tableGrip(const TableGeometry &geometry) const
{
    const ui::Theme &theme = *ui::Theme::current();
    const qreal size = theme.px(1.0);
    // The grip stands in the room a table keeps above it.
    return {geometry.box.left(), geometry.box.top() - size - theme.px(0.25), size, size};
}

void TextEdit::paintTableGrips(QPainter &painter, const QList<TableGeometry> &tables) const
{
    if (isReadOnly()) {
        return;
    }
    const QTextTable *cursorTable = hasFocus() ? textCursor().currentTable() : nullptr;
    const QPoint mouse = viewport()->mapFromGlobal(QCursor::pos());
    for (const TableGeometry &geometry : tables) {
        if (geometry.table != m_hoverTable && geometry.table != cursorTable) {
            continue;
        }
        const QRectF grip = tableGrip(geometry);
        ui::icon(QStringLiteral("Grip"),
                 grip.contains(mouse) ? ui::IconTone::Primary : ui::IconTone::Idle)
            .paint(&painter, grip.toRect());
    }
}

QTextTable *TextEdit::tableGripAt(const QPoint &viewportPosition) const
{
    for (const TableGeometry &geometry : tableGeometries()) {
        if (tableGrip(geometry).contains(viewportPosition)) {
            return geometry.table;
        }
    }
    return nullptr;
}

QRectF TextEdit::objectRect(const QTextCursor &object) const
{
    const int position = object.selectionStart();
    if (QTextFrame *frame = floatingFrame(*document(), position)) {
        // An image wrapped square is drawn at the top left of its frame, which includes the
        // room kept around it.
        const QPointF corner =
            document()->documentLayout()->frameBoundingRect(frame).topLeft() -
            QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value());
        return {corner, m_objects->imageSize(document(), position, object.charFormat())};
    }
    const QTextBlock block = document()->findBlock(position);
    const int inBlock = position - block.position();
    const QTextLine line = block.layout()->lineForTextPosition(inBlock);
    if (!line.isValid()) {
        return {};
    }
    const QPointF offset(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
    const QRectF rect = document()->documentLayout()->blockBoundingRect(block).translated(offset);
    const qreal left = line.cursorToX(inBlock);
    const qreal height =
        m_objects->intrinsicSize(document(), position, object.charFormat()).height();
    // An image stands on the baseline; a chip is centred on the line.
    if (object.charFormat().objectType() == ImageObject) {
        // An image wrapped top and bottom takes its whole line but is drawn at its own width.
        return {QPointF(rect.left() + left, rect.top() + line.y() + line.ascent() - height),
                m_objects->imageSize(document(), position, object.charFormat())};
    }
    return {rect.left() + left, rect.top() + line.y() + ((line.height() - height) / 2),
            line.cursorToX(inBlock + 1) - left, height};
}

std::optional<QTextCursor> TextEdit::selectedImage() const
{
    QTextCursor cursor = textCursor();
    if (cursor.selectionEnd() - cursor.selectionStart() != 1 ||
        cursor.charFormat().objectType() != ImageObject) {
        return std::nullopt;
    }
    return cursor;
}

std::array<QRectF, 4> TextEdit::imageHandles(const QRectF &rect) const
{
    const qreal size = ui::Theme::current()->px(0.6);
    const auto at = [size](QPointF corner) {
        return QRectF(corner - QPointF(size / 2, size / 2), QSizeF(size, size));
    };
    return {at(rect.topLeft()), at(rect.topRight()), at(rect.bottomLeft()), at(rect.bottomRight())};
}

void TextEdit::paintDragFeedback(QPainter &painter) const
{
    const ui::Roles &r = ui::Theme::current()->roles();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    if (m_columnDrag && m_columnDrag->table != nullptr) {
        for (const TableGeometry &geometry : tableGeometries()) {
            if (geometry.table == m_columnDrag->table) {
                painter.setPen(QPen(r.primary, 2));
                painter.drawLine(QPointF(m_columnDrag->x, geometry.box.top()),
                                 QPointF(m_columnDrag->x, geometry.box.bottom()));
            }
        }
    }
    if (!m_dropMark.isNull()) {
        painter.setPen(QPen(r.primary, 2));
        painter.drawLine(m_dropMark);
    }
    if (m_imageDrag) {
        // The image keeps its proportions and its place while it is resized: on the line, or at
        // the top of its frame when it floats.
        const QRectF &start = m_imageDrag->start;
        const qreal height = m_imageDrag->width * start.height() / std::max(1.0, start.width());
        const bool floating =
            floatingFrame(*document(), m_imageDrag->image.selectionStart()) != nullptr;
        painter.setPen(QPen(r.primary, 2, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(start.left(), floating ? start.top() : start.bottom() - height,
                                m_imageDrag->width, height));
    } else if (const auto image = selectedImage(); image && !isReadOnly()) {
        const QRectF rect = objectRect(*image);
        painter.setPen(QPen(r.primary, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect);
        painter.setPen(QPen(r.surface, 1));
        painter.setBrush(r.primary);
        for (const QRectF &handle : imageHandles(rect)) {
            painter.drawRect(handle);
        }
    }
    painter.restore();
}

void TextEdit::finishImageDrag()
{
    if (!m_imageDrag) {
        return;
    }
    const ImageDrag drag = *m_imageDrag;
    m_imageDrag.reset();
    viewport()->update();
    const int position = drag.image.selectionStart();
    const qreal available = m_objects->availableWidth(document(), position);
    const int width = snappedWidth(drag.width * 100.0 / std::max(1.0, available));
    QTextCharFormat format = drag.image.charFormat();
    if (width >= 100) {
        format.clearProperty(ImageWidth);
    } else {
        format.setProperty(ImageWidth, width);
    }
    QTextCursor(drag.image).setCharFormat(format);
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
    if (event->key() == Qt::Key_Space && modifiers == Qt::NoModifier &&
        codeBlockOf(textCursor().block()) == nullptr && !applyMarkdownPrefix(textCursor())) {
        linkTypedAddress();
    }
    if (event->text() == QLatin1StringView("`")) {
        if (QTextCursor typed = textCursor(); applyMarkdownCode(typed)) {
            setTextCursor(typed);
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
    if (applyMarkdownLine(cursor)) {
        setTextCursor(cursor);
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

QList<CodeHeader> TextEdit::codeHeaders() const
{
    return editor::codeHeaders(
        *document(), QPointF(-horizontalScrollBar()->value(), -verticalScrollBar()->value()),
        viewport()->height(), syntaxRepository());
}

std::optional<CodeHeader> TextEdit::codeHeaderAt(const QPoint &viewportPosition) const
{
    for (const CodeHeader &header : codeHeaders()) {
        if (header.box.contains(viewportPosition)) {
            return header;
        }
    }
    return std::nullopt;
}

void TextEdit::paintCodeHeaders(QPainter &painter, const QList<CodeHeader> &headers) const
{
    const ui::Theme &theme = *ui::Theme::current();
    const ui::Roles &r = theme.roles();
    const QTextFrame *cursorFrame = hasFocus() ? codeBlockOf(textCursor().block()) : nullptr;
    const int icon = theme.px(1.0);
    const int chevron = theme.px(0.75);
    for (const CodeHeader &header : headers) {
        // The title field covers the title while it is edited.
        const bool editing = header.frame == m_titleFrame;
        paintCodeLabels(painter, header, r, !editing);
        const bool active = header.frame == m_hoverCode || header.frame == cursorFrame;
        if (!editing && active && !isReadOnly() &&
            header.frame->frameFormat().stringProperty(CodeTitle).isEmpty()) {
            painter.setFont(theme.font(ui::TextRole::Label));
            painter.setPen(ui::mix(r.onSurfaceVariant, 0.6F, r.surfaceContainerHighest));
            painter.drawText(header.title, Qt::AlignLeft | Qt::AlignVCenter, tr("Title"));
        }
        if (!isReadOnly()) {
            ui::icon(QStringLiteral("ChevronDown"))
                .paint(&painter,
                       QRect(header.language.right() - chevron,
                             header.language.center().y() - (chevron / 2), chevron, chevron));
        }
        ui::icon(QStringLiteral("Copy"),
                 header.frame == m_hoverCode ? ui::IconTone::Primary : ui::IconTone::Idle)
            .paint(&painter, QRect(header.copy.center().x() - (icon / 2),
                                   header.copy.center().y() - (icon / 2), icon, icon));
    }
}

bool TextEdit::clickCodeHeader(const QPoint &viewportPosition)
{
    const auto header = codeHeaderAt(viewportPosition);
    if (!header) {
        return false;
    }
    if (header->copy.contains(viewportPosition)) {
        copyCode(*header->frame);
        return true;
    }
    if (isReadOnly()) {
        return false;
    }
    if (header->language.contains(viewportPosition)) {
        chooseLanguage(header->frame, viewport()->mapToGlobal(header->language.bottomLeft()));
        return true;
    }
    if (header->title.contains(viewportPosition)) {
        editTitle(header->frame);
        return true;
    }
    return false;
}

void TextEdit::copyCode(const QTextFrame &frame)
{
    QGuiApplication::clipboard()->setText(codeText(frame));
    Q_EMIT message(tr("Code copied"));
}

void TextEdit::chooseLanguage(QTextFrame *frame, const QPoint &globalPosition)
{
    if (frame == nullptr || isReadOnly()) {
        return;
    }
    m_languageFrame = frame;
    m_languages->open(globalPosition, frame->frameFormat().stringProperty(CodeLanguage));
}

void TextEdit::editTitle(QTextFrame *frame)
{
    if (frame == nullptr || isReadOnly()) {
        return;
    }
    const auto headers = codeHeaders();
    const auto it = std::ranges::find(headers, frame, &CodeHeader::frame);
    if (it == headers.end()) {
        ensureCursorVisible();
        return;
    }
    m_titleFrame = frame;
    m_titleEdit->setFont(ui::Theme::current()->font(ui::TextRole::Label));
    m_titleEdit->setPlaceholderText(tr("Title"));
    m_titleEdit->setText(frame->frameFormat().stringProperty(CodeTitle));
    m_titleEdit->setGeometry(it->title);
    m_titleEdit->show();
    m_titleEdit->setFocus();
    m_titleEdit->selectAll();
    viewport()->update();
}

void TextEdit::finishTitle(bool keep)
{
    if (!m_titleEdit->isVisible()) {
        return;
    }
    const QPointer<QTextFrame> frame = m_titleFrame;
    m_titleFrame = nullptr;
    m_titleEdit->hide();
    if (frame == nullptr) {
        return;
    }
    const QString title = m_titleEdit->text().simplified();
    if (keep && title != frame->frameFormat().stringProperty(CodeTitle)) {
        setCodeBlockProperty(*frame, CodeTitle, title);
    }
    setTextCursor(frame->firstCursorPosition());
    setFocus();
    viewport()->update();
}

bool TextEdit::canInsertFromMimeData(const QMimeData *source) const
{
    return source->hasText() || source->hasHtml() || source->hasImage() || source->hasUrls() ||
           source->hasFormat(BodyMimeType);
}

void TextEdit::insertFromMimeData(const QMimeData *source)
{
    // Code is plain text, so a code block takes the text of anything pasted into it.
    if (codeBlockOf(textCursor().block()) != nullptr) {
        if (source->hasText()) {
            insertPlainText(source->text());
        }
        return;
    }
    auto content = std::make_shared<QTextDocument>();
    if (source->hasFormat(BodyMimeType)) {
        readBody(*content, QString::fromUtf8(source->data(BodyMimeType)));
    } else if (source->hasHtml()) {
        // Copying Markdown often supplies an HTML wrapper too; it must not bypass conversion.
        // Bare URLs alone are not a reason to discard the source's rich formatting.
        if (!source->hasText() || !readMarkdown(*content, source->text(), true)) {
            readBody(*content, source->html(), ReadMode::Paste);
        }
    } else if (source->hasImage()) {
        // A picture copied from an image viewer or a screenshot tool keeps its own bytes when the
        // clipboard offers them in a format notes accept.
        QByteArray bytes;
        for (const QString &format : source->formats()) {
            if (format.startsWith(QLatin1StringView("image/")) &&
                core::imageKind(source->data(format)) != core::ImageKind::None) {
                bytes = source->data(format);
                break;
            }
        }
        if (bytes.isEmpty()) {
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            qvariant_cast<QImage>(source->imageData()).save(&buffer, "PNG");
        }
        QTextCharFormat pending = pendingFormat(ImageObject, QString());
        pending.setProperty(PendingSource, bytes);
        QTextCursor(content.get()).insertText(QString(QChar::ObjectReplacementCharacter), pending);
    } else if (source->hasUrls()) {
        QStringList paths;
        for (const QUrl &url : source->urls()) {
            if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) {
                paths.append(url.toLocalFile());
            }
        }
        if (!paths.isEmpty()) {
            insertFiles(paths, true);
        } else if (source->hasText()) {
            insertPlainText(source->text());
        }
        return;
    } else if (source->hasText()) {
        if (!readMarkdown(*content, source->text())) {
            insertPlainText(source->text());
            return;
        }
    }
    // Tables do not nest and cells hold no code blocks, so a cell takes such content as text.
    const QList<QTextFrame *> frames = content->rootFrame()->childFrames();
    if (textCursor().currentTable() != nullptr && std::ranges::any_of(frames, [](QTextFrame *f) {
            return qobject_cast<QTextTable *>(f) != nullptr ||
                   f->frameFormat().boolProperty(CodeBlock);
        })) {
        insertPlainText(content->toPlainText());
        return;
    }
    insertContent(content, textCursor());
}

QMimeData *TextEdit::createMimeDataFromSelection() const
{
    return bodyMimeData(textCursor().selection());
}

void TextEdit::copyMarkdown()
{
    if (!textCursor().hasSelection()) {
        return;
    }
    QTextDocument content;
    QTextCursor(&content).insertFragment(textCursor().selection());
    QGuiApplication::clipboard()->setText(writeMarkdown(content));
}

void TextEdit::pastePlainText()
{
    if (!isReadOnly()) {
        insertPlainText(QGuiApplication::clipboard()->text());
    }
}

void TextEdit::insertFiles(const QStringList &paths, bool asImages)
{
    auto content = std::make_shared<QTextDocument>();
    QTextCursor cursor(content.get());
    for (const QString &path : paths) {
        if (!cursor.atStart()) {
            cursor.insertText(QStringLiteral(" "));
        }
        cursor.insertText(QString(QChar::ObjectReplacementCharacter),
                          pendingFormat(asImages ? ImageObject : AttachmentObject, path));
    }
    insertContent(content, textCursor());
}

QList<TextEdit::PendingFile> TextEdit::pendingFiles(const QTextDocument &content) const
{
    QList<PendingFile> files;
    for (QTextBlock block = content.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const QTextCharFormat format = fragment.charFormat();
            if (!isObject(format)) {
                continue;
            }
            PendingFile file{.position = 0,
                             .type = format.objectType(),
                             .path = {},
                             .data = {},
                             .name = {},
                             .format = format,
                             .storeAs = format.objectType() == ImageObject ? core::StoreAs::Image
                                                                           : core::StoreAs::File};
            const QVariant source = format.property(PendingSource);
            if (source.typeId() == QMetaType::QByteArray) {
                file.data = source.toByteArray();
                file.name = tr("Pasted image") + QStringLiteral(".png");
            } else if (const QString text = source.toString(); text.startsWith(u"data:")) {
                const auto decoded = decodeDataUrl(text);
                if (!decoded) {
                    continue;
                }
                std::tie(file.data, file.name) = *decoded;
            } else if (text.startsWith(u"file:")) {
                // A web page can put any local address into copied HTML, so only an image is
                // taken from it.
                file.path = QUrl(text).toLocalFile();
                file.storeAs = core::StoreAs::ImageOnly;
            } else if (!text.isEmpty()) {
                file.path = text;
            } else if (format.stringProperty(ObjectNote) != m_noteId) {
                // Content copied from another note brings its files along, so it does not
                // depend on that note's folder.
                file.path = core::attachmentsDir(m_libraryRoot, format.stringProperty(ObjectNote)) +
                            u'/' + format.stringProperty(ObjectFile);
            } else {
                continue;
            }
            for (int i = 0; i < fragment.length(); ++i) {
                file.position = fragment.position() + i;
                files.append(file);
            }
        }
    }
    return files;
}

void TextEdit::insertDocument(const QTextDocument &content, QTextCursor cursor)
{
    // Qt merges the first block of a fragment into the block at the cursor and drops the
    // format of an empty one, so a leading divider is given its own block and format.
    const QTextBlockFormat first = content.begin().blockFormat();
    const bool divider = content.begin().length() == 1 &&
                         first.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth);
    cursor.beginEditBlock();
    if (divider) {
        cursor.removeSelectedText();
        if (!cursor.atBlockStart()) {
            cursor.insertBlock();
        }
    }
    const int block = cursor.blockNumber();
    cursor.insertFragment(QTextDocumentFragment(&content));
    if (divider) {
        QTextCursor(document()->findBlockByNumber(block)).setBlockFormat(first);
    }
    cursor.endEditBlock();
    setTextCursor(cursor);
}

void TextEdit::insertContent(const std::shared_ptr<QTextDocument> &content, const QTextCursor &at)
{
    const QList<PendingFile> files = pendingFiles(*content);
    const QString folder = attachmentFolder();
    if (files.isEmpty()) {
        insertDocument(*content, at);
        return;
    }
    if (folder.isEmpty()) {
        return;
    }
    // Copies run on a worker; the cursor follows the edits made meanwhile, and the content goes
    // in once the files are stored, unless another note was opened.
    const QString noteId = m_noteId;
    QtConcurrent::run([files, folder] {
        QList<core::Result<core::StoredFile>> stored;
        for (const PendingFile &file : files) {
            stored.append(file.path.isEmpty() ? core::storeData(folder, file.name, file.data)
                                              : core::storeFile(folder, file.path, file.storeAs));
        }
        return stored;
    })
        .then(this, [this, content, files, noteId, folder,
                     at](const QList<core::Result<core::StoredFile>> &stored) {
            if (noteId != m_noteId || folder != attachmentFolder() || at.document() != document() ||
                isReadOnly()) {
                return;
            }
            QStringList errors;
            // From the end, so the positions of the objects before stay valid.
            for (qsizetype i = files.size() - 1; i >= 0; --i) {
                const PendingFile &file = files.at(i);
                QTextCursor object(content.get());
                object.setPosition(file.position);
                object.setPosition(file.position + 1, QTextCursor::KeepAnchor);
                const auto &result = stored.at(i);
                if (!result || (file.type == ImageObject && file.path.isEmpty() &&
                                result->kind == core::ImageKind::None)) {
                    errors.append(result ? QFileInfo(file.name).fileName()
                                         : core::describe(result.error()));
                    object.removeSelectedText();
                    continue;
                }
                const QString name = file.format.stringProperty(AttachmentName);
                object.setCharFormat(
                    file.type == ImageObject && result->kind != core::ImageKind::None
                        ? imageFormat(noteId, result->fileName, file.format.intProperty(ImageWidth),
                                      imageWrap(file.format))
                        : attachmentFormat(noteId, result->fileName,
                                           name.isEmpty() ? result->fileName : name, result->size));
            }
            insertDocument(*content, at);
            if (!errors.isEmpty()) {
                Q_EMIT message(tr("Not added: %1").arg(errors.join(QStringLiteral("; "))));
            }
        });
}

std::optional<QTextCursor> TextEdit::objectAt(const QPoint &viewportPosition) const
{
    // Images wrapped square float beside the text, away from their place in it.
    for (const QTextFrame *frame : floatingFrames(*document())) {
        QTextCursor object(document());
        object.setPosition(frame->firstPosition() - 1);
        object.setPosition(frame->firstPosition(), QTextCursor::KeepAnchor);
        if (isObject(object.charFormat()) && objectRect(object).contains(viewportPosition)) {
            return object;
        }
    }
    const QPointF point(viewportPosition.x() + horizontalScrollBar()->value(),
                        viewportPosition.y() + verticalScrollBar()->value());
    const int position = document()->documentLayout()->hitTest(point, Qt::FuzzyHit);
    // The hit is the position before or after the object, depending on the half clicked.
    for (const int candidate : {position, position - 1}) {
        if (candidate < 0 || candidate + 1 >= document()->characterCount()) {
            continue;
        }
        QTextCursor object(document());
        object.setPosition(candidate);
        object.setPosition(candidate + 1, QTextCursor::KeepAnchor);
        if (!isObject(object.charFormat())) {
            continue;
        }
        const QTextBlock block = object.block();
        const int inBlock = candidate - block.position();
        const QTextLine line = block.layout()->lineForTextPosition(inBlock);
        const QRectF rect = document()->documentLayout()->blockBoundingRect(block);
        const QRectF box(rect.left() + line.cursorToX(inBlock), rect.top() + line.y(),
                         line.cursorToX(inBlock + 1) - line.cursorToX(inBlock), line.height());
        if (box.contains(point)) {
            return object;
        }
    }
    return std::nullopt;
}

std::optional<QTextCursor> TextEdit::objectAtCursor() const
{
    const QTextCursor cursor = textCursor();
    const int start = cursor.hasSelection() ? cursor.selectionStart() : cursor.position();
    for (const int candidate : {start, start - 1}) {
        if (candidate < 0 || (cursor.hasSelection() && candidate != start)) {
            continue;
        }
        QTextCursor object(document());
        object.setPosition(candidate);
        if (!object.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor)) {
            continue;
        }
        if (isObject(object.charFormat())) {
            return object;
        }
    }
    return std::nullopt;
}

void TextEdit::addObjectActions(QMenu &menu, const QTextCursor &at, const QPoint &globalPosition)
{
    Q_UNUSED(globalPosition);
    const bool editable = !isReadOnly();
    const QTextCharFormat format = at.hasSelection() ? at.charFormat() : QTextCharFormat();
    if (format.objectType() == ImageObject) {
        menu.addSeparator();
        if (editable) {
            const std::array names{tr("Small"), tr("Medium"), tr("Large"), tr("Full width")};
            for (std::size_t i = 0; i < ImageWidths.size(); ++i) {
                const int width = ImageWidths.at(i);
                QAction *size = menu.addAction(names.at(i));
                size->setCheckable(true);
                size->setChecked(format.intProperty(ImageWidth) == width);
                connect(size, &QAction::triggered, this, [at, format, width] {
                    QTextCharFormat changed = format;
                    if (width > 0) {
                        changed.setProperty(ImageWidth, width);
                    } else {
                        changed.clearProperty(ImageWidth);
                    }
                    QTextCursor(at).setCharFormat(changed);
                });
            }
            menu.addSeparator();
            QMenu *wrapMenu = menu.addMenu(tr("Wrap text"));
            auto *wraps = new QActionGroup(wrapMenu);
            for (const auto &[wrap, glyph, name] :
                 {std::tuple{ImageWrap::Inline, QStringLiteral("WrapInline"),
                             tr("In line with text")},
                  std::tuple{ImageWrap::Square, QStringLiteral("WrapSquare"), tr("Square")},
                  std::tuple{ImageWrap::TopAndBottom, QStringLiteral("WrapTopBottom"),
                             tr("Top and bottom")}}) {
                QAction *action = wrapMenu->addAction(ui::icon(glyph), name);
                action->setCheckable(true);
                action->setChecked(imageWrap(format) == wrap);
                wraps->addAction(action);
                connect(action, &QAction::triggered, this, [this, at, wrap] {
                    QTextCursor image = at;
                    setImageWrap(image, wrap);
                    setTextCursor(image);
                });
                if (wrap == ImageWrap::Inline) {
                    wrapMenu->addSeparator();
                }
            }
            menu.addSeparator();
        }
        menu.addAction(tr("Open image"), this, [this, at] { openObject(at); });
        menu.addAction(tr("Show in folder"), this, [this, at] { showObjectFolder(at); });
    } else if (format.objectType() == AttachmentObject) {
        menu.addSeparator();
        menu.addAction(tr("Open"), this, [this, at] { openObject(at); });
        menu.addAction(tr("Save as…"), this, [this, at] { saveObjectAs(at); });
        menu.addAction(tr("Show in folder"), this, [this, at] { showObjectFolder(at); });
        if (editable) {
            menu.addAction(tr("Remove"), this, [at] { QTextCursor(at).removeSelectedText(); });
        }
    }
    if (editable && at.currentTable() != nullptr) {
        menu.addSeparator();
        const auto change = [this, at](TableChange kind) {
            QTextCursor cursor = at;
            changeTable(cursor, kind);
            setTextCursor(cursor);
        };
        menu.addAction(tr("Insert row above"), this, [=] { change(TableChange::RowAbove); });
        menu.addAction(tr("Insert row below"), this, [=] { change(TableChange::RowBelow); });
        menu.addAction(tr("Insert column left"), this, [=] { change(TableChange::ColumnLeft); });
        menu.addAction(tr("Insert column right"), this, [=] { change(TableChange::ColumnRight); });
        menu.addAction(tr("Delete row"), this, [=] { change(TableChange::DeleteRow); });
        menu.addAction(tr("Delete column"), this, [=] { change(TableChange::DeleteColumn); });
        QAction *header = menu.addAction(tr("Header row"));
        header->setCheckable(true);
        header->setChecked(at.currentTable()->format().headerRowCount() > 0);
        connect(header, &QAction::triggered, this, [=] { change(TableChange::ToggleHeader); });
        menu.addSeparator();
        // Column widths can be dragged; these keep them in reach of the keyboard.
        QTextTable *table = at.currentTable();
        const int column = table->cellAt(at).column();
        menu.addAction(tr("Column width…"), this,
                       [this, table, column] {
                           const QList<qreal> widths =
                               columnWidths(table->format(), table->columns());
                           const int most = 100 - (MinColumnWidth * (table->columns() - 1));
                           bool ok = false;
                           const int width = QInputDialog::getInt(
                               this, tr("Column width"), tr("Width in percent of the table"),
                               static_cast<int>(std::lround(widths.at(column))), MinColumnWidth,
                               most, 1, &ok);
                           if (ok) {
                               resizeColumns(*table, withColumnWidth(widths, column, width));
                           }
                       })
            ->setEnabled(table->columns() > 1);
        menu.addAction(tr("Equal column widths"), this, [table] { resizeColumns(*table, {}); })
            ->setEnabled(!hasEqualColumns(table->format(), table->columns()));
        // The table's width and place can be dragged too.
        menu.addAction(tr("Table width…"), this, [this, table] {
            const int current = tableWidth(table->format());
            bool ok = false;
            const int width = QInputDialog::getInt(
                this, tr("Table width"), tr("Width in percent of the text column"),
                current > 0 ? current : 100, MinWidth, 100, 1, &ok);
            if (ok) {
                setTableWidth(*table, width);
            }
        });
        menu.addAction(tr("Full width"), this, [table] { setTableWidth(*table, 100); })
            ->setEnabled(tableWidth(table->format()) > 0);
        menu.addSeparator();
        menu.addAction(tr("Move up"), this, [=, this] { moveTableBy(table, -1, at); })
            ->setEnabled(tableMovePlace(*table, -1) >= 0);
        menu.addAction(tr("Move down"), this, [=, this] { moveTableBy(table, 1, at); })
            ->setEnabled(tableMovePlace(*table, 1) >= 0);
        menu.addSeparator();
        menu.addAction(tr("Delete table"), this, [=] { change(TableChange::DeleteTable); });
    }
}

void TextEdit::moveTableBy(QTextTable *table, int direction, const QTextCursor &cell)
{
    const QTextTableCell at = table->cellAt(cell);
    if (const QTextTable *moved = moveTable(*table, tableMovePlace(*table, direction))) {
        setTextCursor(moved->cellAt(at.row(), at.column()).firstCursorPosition());
        ensureCursorVisible();
    }
}

void TextEdit::startMove(const MoveSource &source)
{
    m_move = source;
    auto *drag = new QDrag(this);
    drag->setMimeData(bodyMimeData(source.content.selection()));
    // A drop in another program copies; only a drop here moves.
    drag->exec(Qt::MoveAction | Qt::CopyAction, Qt::MoveAction);
    m_move.reset();
    m_dropMark = {};
    viewport()->update();
}

std::optional<TextEdit::DropPlace> TextEdit::dropPlace(const QPoint &viewportPosition) const
{
    if (!m_move) {
        return std::nullopt;
    }
    const QTextCursor hit = cursorForPosition(viewportPosition);
    if (m_move->table != nullptr) {
        const QTextBlock block = hit.block();
        QTextFrame *frame = topFrameOf(block);
        if (frame == m_move->table) {
            return std::nullopt;
        }
        // The table goes before or after the paragraph or the frame under the mouse, by the
        // half the mouse is in.
        const QAbstractTextDocumentLayout *layout = document()->documentLayout();
        const QRectF rect =
            (frame != nullptr ? layout->frameBoundingRect(frame) : layout->blockBoundingRect(block))
                .translated(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
        const bool after = viewportPosition.y() > rect.center().y();
        const qreal y = after ? rect.bottom() : rect.top();
        return DropPlace{.position = tablePlace(block, after),
                         .mark = QLineF(0, y, viewport()->width(), y)};
    }
    const int position = hit.position();
    const QTextCursor &content = m_move->content;
    if (codeBlockOf(hit.block()) != nullptr ||
        (position >= content.selectionStart() && position <= content.selectionEnd())) {
        return std::nullopt;
    }
    const QRect caret = cursorRect(hit);
    return DropPlace{.position = position,
                     .mark = QLineF(caret.left(), caret.top(), caret.left(), caret.bottom())};
}

void TextEdit::moveObject(const QTextCursor &object, int position)
{
    QTextCursor target(document());
    target.setPosition(position);
    QTextCursor source = object;
    source.beginEditBlock();
    const QTextDocumentFragment fragment = source.selection();
    source.removeSelectedText();
    const int start = target.position();
    target.insertFragment(fragment);
    source.endEditBlock();
    // A moved image stays selected, with its handles; the cursor follows a chip.
    if (fragment.toPlainText() == QString(QChar::ObjectReplacementCharacter) &&
        object.charFormat().objectType() == ImageObject) {
        target.setPosition(start);
        target.setPosition(start + 1, QTextCursor::KeepAnchor);
    }
    setTextCursor(target);
}

void TextEdit::dragEnterEvent(QDragEnterEvent *event)
{
    if (m_move && event->source() == this) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    QTextEdit::dragEnterEvent(event);
}

void TextEdit::dragMoveEvent(QDragMoveEvent *event)
{
    if (!m_move || event->source() != this) {
        QTextEdit::dragMoveEvent(event);
        return;
    }
    const auto place = dropPlace(event->position().toPoint());
    m_dropMark = place ? place->mark : QLineF();
    viewport()->update();
    if (place) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void TextEdit::dragLeaveEvent(QDragLeaveEvent *event)
{
    m_dropMark = {};
    viewport()->update();
    QTextEdit::dragLeaveEvent(event);
}

void TextEdit::dropEvent(QDropEvent *event)
{
    if (!m_move || event->source() != this) {
        QTextEdit::dropEvent(event);
        return;
    }
    m_dropMark = {};
    viewport()->update();
    const auto place = dropPlace(event->position().toPoint());
    if (!place) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::MoveAction);
    event->accept();
    if (m_move->table != nullptr) {
        if (const QTextTable *moved = moveTable(*m_move->table, place->position)) {
            setTextCursor(moved->cellAt(0, 0).firstCursorPosition());
        }
        return;
    }
    moveObject(m_move->content, place->position);
}

void TextEdit::removeOldSnapshots()
{
    QThreadPool::globalInstance()->start([] {
        const QDir folder(snapshotFolder());
        const QDateTime limit = QDateTime::currentDateTimeUtc().addDays(-1);
        for (const QFileInfo &entry :
             folder.entryInfoList({QStringLiteral("open-*")}, QDir::Dirs | QDir::NoSymLinks)) {
            if (entry.lastModified() < limit) {
                if (auto removed = core::removePath(entry.absoluteFilePath(), true); !removed) {
                    qWarning("Cannot remove an old attachment snapshot: %s",
                             qPrintable(removed.error().detail));
                }
            }
        }
    });
}

void TextEdit::openObject(const QTextCursor &object)
{
    const QString path = m_objects->filePath(object.charFormat());
    // Handlers receive an owned snapshot, never an untrusted library path that could be
    // redirected through a replaced parent while the desktop opens it asynchronously.
    QtConcurrent::run([path]() -> core::Result<std::shared_ptr<QTemporaryDir>> {
        // The snapshot lives in the cache folder, not in /tmp, which is often memory; it is
        // copied a megabyte at a time, so a large file is never held in memory.
        const QString cache = snapshotFolder();
        if (!QDir().mkpath(cache)) {
            return core::fail(core::ErrorCode::Io, cache);
        }
        auto temporary = std::make_shared<QTemporaryDir>(cache + QStringLiteral("/open-XXXXXX"));
        if (!temporary->isValid()) {
            return core::fail(core::ErrorCode::Io, path, temporary->errorString());
        }
        if (auto copied =
                core::copyFileAtomic(path, temporary->filePath(QFileInfo(path).fileName()));
            !copied) {
            return std::unexpected(copied.error());
        }
        return temporary;
    }).then(this, [this, path](const core::Result<std::shared_ptr<QTemporaryDir>> &result) {
        if (!result) {
            Q_EMIT message(core::describe(result.error()));
            return;
        }
        const QString copy = (*result)->filePath(QFileInfo(path).fileName());
        if (core::isRunnable(copy)) {
            QMessageBox box(QMessageBox::Warning, tr("Open file"),
                            tr("“%1” is a program or script, so Jotter does not open it.")
                                .arg(QFileInfo(path).fileName()),
                            QMessageBox::NoButton, this);
            const auto *show = box.addButton(tr("Show in folder"), QMessageBox::AcceptRole);
            box.addButton(tr("Cancel"), QMessageBox::RejectRole);
            box.exec();
            if (box.clickedButton() == show) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
            }
            return;
        }
        // A document handler may open the path later. Keep the snapshot through process exit.
        static QList<std::shared_ptr<QTemporaryDir>> opened;
        opened.append(*result);
        QDesktopServices::openUrl(QUrl::fromLocalFile(copy));
    });
}

void TextEdit::showObjectFolder(const QTextCursor &object)
{
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QFileInfo(m_objects->filePath(object.charFormat())).absolutePath()));
}

void TextEdit::saveObjectAs(const QTextCursor &object)
{
    const QTextCharFormat format = object.charFormat();
    const QString source = m_objects->filePath(format);
    const QString target = QFileDialog::getSaveFileName(
        this, tr("Save as"),
        QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
            .filePath(format.stringProperty(AttachmentName)));
    if (target.isEmpty()) {
        return;
    }
    QtConcurrent::run([source, target] {
        return core::copyFileAtomic(source, target);
    }).then(this, [this, target](const core::Result<void> &copied) {
        Q_EMIT message(
            copied
                ? tr("Saved to %1").arg(QDir::toNativeSeparators(target))
                : tr("The file could not be saved to %1.").arg(QDir::toNativeSeparators(target)));
    });
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

void TextEdit::linkTypedAddress()
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
        return;
    }
    QTextCursor range(cursor.block());
    range.setPosition(cursor.block().position() + start);
    range.setPosition(cursor.block().position() + start + static_cast<int>(word.size()),
                      QTextCursor::KeepAnchor);
    if (range.charFormat().isAnchor()) {
        return;
    }
    const QString href = core::normaliseLink(word);
    if (!href.isEmpty()) {
        setLink(range, href, {});
    }
}

} // namespace editor
