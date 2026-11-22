#include "core/Attachments.h"
#include "editor/Commands.h"
#include "editor/Formats.h"
#include "editor/HtmlCodec.h"
#include "editor/Markdown.h"
#include "editor/Objects.h"
#include "editor/Presenter.h"
#include "editor/TextEdit.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QClipboard>
#include <QDrag>
#include <QDropEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <QTextFrame>
#include <QTextTable>
#include <QUrl>
#include <QtConcurrentRun>

#include <memory>
#include <utility>

namespace editor {

namespace {

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

} // namespace

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

} // namespace editor
