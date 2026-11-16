#include "editor/Objects.h"

#include "core/Attachments.h"
#include "core/Directory.h"
#include "editor/Commands.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QCache>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QMimeDatabase>
#include <QPainter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextList>
#include <QTextTable>
#include <QtConcurrentRun>

#include <sys/stat.h>

namespace editor {

namespace {

/// Scaled images shared by all editors, with their cost in kilobytes: 64 MB in all.
QCache<QString, QImage> &imageCache()
{
    static QCache<QString, QImage> cache(qsizetype{64} * 1024);
    return cache;
}

/// Includes inode and nanosecond timestamps so replacing equal-sized files invalidates pixels.
QString imageVersion(const QString &path)
{
    struct stat status{};
    if (::stat(QFile::encodeName(path).constData(), &status) != 0) {
        return path + QStringLiteral("|missing");
    }
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8")
        .arg(path)
        .arg(status.st_dev)
        .arg(status.st_ino)
        .arg(status.st_size)
        .arg(status.st_mtim.tv_sec)
        .arg(status.st_mtim.tv_nsec)
        .arg(status.st_ctim.tv_sec)
        .arg(status.st_ctim.tv_nsec);
}

/// Keys a decoded size by the precise source-file version.
QString cacheKey(const QString &path, const QSize &size)
{
    return QStringLiteral("%1|%2x%3").arg(imageVersion(path)).arg(size.width()).arg(size.height());
}

/// Returns the size an image has once its orientation is applied.
QSize orientedSize(const QImageReader &reader, QSize size)
{
    if (reader.transformation().testFlag(QImageIOHandler::TransformationRotate90)) {
        size.transpose();
    }
    return size;
}

/// Sets up a reader for an image file with the decoder of its kind, detected from its bytes, so
/// only the PNG, JPEG, GIF and WebP decoders ever see a note's files, whatever other image
/// plugins are installed.
///
/// @param reader The reader to set up.
/// @param path The image file.
/// @return Whether the file is an accepted image kind; otherwise the reader is left unset.
bool openImage(QImageReader &reader, const QString &path, std::unique_ptr<QFile> &file)
{
    auto opened = core::openFile(path, QIODevice::ReadOnly);
    if (!opened) {
        return false;
    }
    file = std::move(*opened);
    const QString mime = core::imageMimeType(core::imageKind(file->peek(16)));
    const QList<QByteArray> formats = mime.isEmpty()
                                          ? QList<QByteArray>()
                                          : QImageReader::imageFormatsForMimeType(mime.toLatin1());
    if (formats.isEmpty()) {
        return false;
    }
    reader.setDevice(file.get());
    reader.setFormat(formats.first());
    reader.setAutoDetectImageFormat(false);
    reader.setAutoTransform(true);
    return true;
}

/// Decodes an image file at a size in device pixels, with its orientation applied.
QImage decodeImage(const QString &path, const QSize &size)
{
    std::unique_ptr<QFile> file;
    QImageReader reader;
    if (!openImage(reader, path, file)) {
        return {};
    }
    // The reader scales before it rotates, so a rotated image is asked for transposed.
    reader.setScaledSize(orientedSize(reader, size));
    return reader.read();
}

/// The parts of a chip in its rectangle: the padding and the gaps between glyph, name and size.
struct ChipMetrics
{
    QFont font;
    qreal padding;
    qreal gap;
    qreal glyph;
    qreal height;
};

ChipMetrics chipMetrics(const QTextDocument *document)
{
    const ui::Theme &theme = *ui::Theme::current();
    const QFontMetricsF metrics(document->defaultFont());
    return {.font = document->defaultFont(),
            .padding = static_cast<qreal>(theme.px(0.5)),
            .gap = static_cast<qreal>(theme.px(0.35)),
            .glyph = metrics.height(),
            .height = metrics.height() + theme.px(0.3)};
}

QString sizeText(qint64 size)
{
    return QLocale().formattedDataSize(size, 1, QLocale::DataSizeTraditionalFormat);
}

/// Returns the room kept between a wrapped image and the text, in pixels.
qreal wrapGap()
{
    return ui::Theme::current()->px(0.75);
}

/// Tells whether a laid-out paragraph has a line that Qt moved down to below a floating image,
/// which leaves room above the line.
bool hasLineBelowFloat(const QTextBlock &block)
{
    const QTextLayout *layout = block.layout();
    qreal bottom = 0;
    for (int i = 0; i < layout->lineCount(); ++i) {
        const QTextLine line = layout->lineAt(i);
        if (line.y() > bottom + 0.5) {
            return true;
        }
        bottom = line.y() + line.height();
    }
    return false;
}

} // namespace

QTextCharFormat imageFormat(const QString &noteId, const QString &fileName, int width,
                            ImageWrap wrap)
{
    QTextCharFormat format;
    format.setObjectType(ImageObject);
    format.setProperty(ObjectNote, noteId);
    format.setProperty(ObjectFile, fileName);
    if (width > 0) {
        format.setProperty(ImageWidth, width);
    }
    if (wrap != ImageWrap::Inline) {
        format.setProperty(ImageWrapMode, static_cast<int>(wrap));
    }
    return format;
}

ImageWrap imageWrap(const QTextFormat &format)
{
    const int wrap = format.intProperty(ImageWrapMode);
    if (format.objectType() != ImageObject || (wrap != static_cast<int>(ImageWrap::Square) &&
                                               wrap != static_cast<int>(ImageWrap::TopAndBottom))) {
        return ImageWrap::Inline;
    }
    return static_cast<ImageWrap>(wrap);
}

void insertObject(QTextCursor &cursor, const QTextCharFormat &format)
{
    if (imageWrap(format) != ImageWrap::Square) {
        QTextCharFormat inLine = format;
        inLine.clearProperty(QTextFormat::ObjectIndex);
        cursor.insertText(QString(QChar::ObjectReplacementCharacter), inLine);
        return;
    }
    // Qt floats an object whose format refers to a frame with a floating position.
    cursor.insertImage(format.toImageFormat(), QTextFrameFormat::FloatLeft);
}

void setImageWrap(QTextCursor &image, ImageWrap wrap)
{
    const int position = image.selectionStart();
    image.setPosition(position);
    image.setPosition(position + 1, QTextCursor::KeepAnchor);
    QTextCharFormat format = image.charFormat();
    if (format.objectType() != ImageObject || imageWrap(format) == wrap) {
        return;
    }
    if (wrap == ImageWrap::Inline) {
        format.clearProperty(ImageWrapMode);
    } else {
        format.setProperty(ImageWrapMode, static_cast<int>(wrap));
    }
    if (wrap == ImageWrap::Square && format.intProperty(ImageWidth) == 0) {
        format.setProperty(ImageWidth, ImageWidths.at(1));
    }
    // The frame of an image wrapped square cannot be added or dropped by a format change, so the
    // image is inserted again.
    image.beginEditBlock();
    insertObject(image, format);
    image.endEditBlock();
    image.setPosition(position);
    image.setPosition(position + 1, QTextCursor::KeepAnchor);
}

QList<QTextFrame *> floatingFrames(const QTextDocument &document)
{
    // The floats of a table's cells are the table's children.
    QList<QTextFrame *> frames;
    const auto addFloats = [&frames](const QTextFrame &parent) {
        for (QTextFrame *frame : parent.childFrames()) {
            if (frame->frameFormat().position() != QTextFrameFormat::InFlow) {
                frames.append(frame);
            }
        }
    };
    const QTextFrame *root = document.rootFrame();
    addFloats(*root);
    for (const QTextFrame *frame : root->childFrames()) {
        if (qobject_cast<const QTextTable *>(frame) != nullptr) {
            addFloats(*frame);
        }
    }
    return frames;
}

QTextFrame *floatingFrame(const QTextDocument &document, int position)
{
    // A cursor's format leaves out the object index, so the character's fragment is read.
    const QTextBlock block = document.findBlock(position);
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (!fragment.contains(position)) {
            continue;
        }
        auto *frame = qobject_cast<QTextFrame *>(document.objectForFormat(fragment.charFormat()));
        return frame != nullptr && frame->frameFormat().position() != QTextFrameFormat::InFlow
                   ? frame
                   : nullptr;
    }
    return nullptr;
}

QTextCharFormat attachmentFormat(const QString &noteId, const QString &fileName,
                                 const QString &name, qint64 size)
{
    QTextCharFormat format;
    format.setObjectType(AttachmentObject);
    format.setProperty(ObjectNote, noteId);
    format.setProperty(ObjectFile, fileName);
    format.setProperty(AttachmentName, name);
    format.setProperty(AttachmentSize, size);
    // The chip sits on the middle of the line like a word, not on top of the baseline.
    format.setVerticalAlignment(QTextCharFormat::AlignMiddle);
    return format;
}

QTextCharFormat pendingFormat(ObjectType type, const QString &source)
{
    QTextCharFormat format;
    format.setObjectType(type);
    format.setProperty(PendingSource, source);
    return format;
}

QString fileTypeGlyph(const QString &fileName)
{
    const QMimeType type = QMimeDatabase().mimeTypeForFile(fileName, QMimeDatabase::MatchExtension);
    const QString name = type.name();
    const auto any = [&](std::initializer_list<const char *> names) {
        return std::ranges::any_of(names, [&](const char *candidate) {
            return type.inherits(QString::fromLatin1(candidate));
        });
    };
    if (name.startsWith(QLatin1StringView("image/"))) {
        return QStringLiteral("FileImage");
    }
    if (name.startsWith(QLatin1StringView("audio/"))) {
        return QStringLiteral("FileAudio");
    }
    if (name.startsWith(QLatin1StringView("video/"))) {
        return QStringLiteral("FileVideo");
    }
    if (any({"application/pdf"})) {
        return QStringLiteral("FilePdf");
    }
    if (any({"application/zip", "application/x-tar", "application/x-7z-compressed",
             "application/vnd.rar", "application/x-compressed-tar", "application/gzip",
             "application/x-xz", "application/x-bzip2", "application/zstd"})) {
        return QStringLiteral("FileZip");
    }
    if (any({"text/csv", "application/vnd.oasis.opendocument.spreadsheet",
             "application/vnd.ms-excel",
             "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"})) {
        return QStringLiteral("FileSpreadsheet");
    }
    if (any({"application/json", "application/xml", "application/x-shellscript"}) ||
        (name.startsWith(QLatin1StringView("text/x-")) &&
         type.inherits(QStringLiteral("text/plain")))) {
        return QStringLiteral("FileCode");
    }
    if (any({"text/plain", "application/rtf", "application/vnd.oasis.opendocument.text",
             "application/msword",
             "application/vnd.openxmlformats-officedocument.wordprocessingml.document"})) {
        return QStringLiteral("Document");
    }
    return QStringLiteral("File");
}

ObjectHandler::ObjectHandler(QTextDocument *document)
    : QObject(document)
{
    QAbstractTextDocumentLayout *layout = document->documentLayout();
    layout->registerHandler(ImageObject, this);
    layout->registerHandler(AttachmentObject, this);
    // Qt keeps the floating frame of a removed wrapped image for undo, and the layout keeps
    // text away from where the image was until the frame's layout data is dropped. The signal
    // comes after the frames are updated and before the layout follows the change.
    connect(layout, &QAbstractTextDocumentLayout::update, this,
            &ObjectHandler::relayoutBesideFloats);
    connect(document, &QTextDocument::contentsChange, this,
            [document](int position, int removed, int added) {
                Q_UNUSED(position);
                Q_UNUSED(added);
                if (removed == 0) {
                    return;
                }
                const QTextFrame *root = document->rootFrame();
                for (QTextFrame *frame :
                     document->findChildren<QTextFrame *>(Qt::FindDirectChildrenOnly)) {
                    if (frame != root && frame->parentFrame() == nullptr &&
                        frame->layoutData() != nullptr) {
                        frame->setLayoutData(nullptr);
                    }
                }
            });
}

ObjectHandler::ObjectHandler(QTextDocument *document, const ui::Roles &roles)
    : ObjectHandler(document)
{
    m_roles = roles;
}

ObjectHandler::~ObjectHandler() = default;

void ObjectHandler::setLibraryRoot(const QString &root)
{
    m_root = root;
    m_naturalSizes.clear();
}

QString ObjectHandler::filePath(const QTextFormat &format) const
{
    if (m_root.isEmpty()) {
        return {};
    }
    return core::attachmentsDir(m_root, format.stringProperty(ObjectNote)) + u'/' +
           format.stringProperty(ObjectFile);
}

QSizeF ObjectHandler::intrinsicSize(QTextDocument *document, int posInDocument,
                                    const QTextFormat &format)
{
    if (format.objectType() != ImageObject) {
        return chipSize(document, posInDocument, format);
    }
    const QSizeF size = imageSize(document, posInDocument, format);
    switch (imageWrap(format)) {
    case ImageWrap::Square: {
        // A floating image wider than its column would widen it, so the room beside it is cut
        // short there.
        const qreal column = std::floor(columnWidth(document, posInDocument)) - 1;
        return {std::min(size.width() + wrapGap(), std::max(column, size.width())),
                size.height() + wrapGap()};
    }
    case ImageWrap::TopAndBottom:
        // A pixel short of the line, which fits no text and does not widen the column.
        return {std::max(std::floor(availableWidth(document, posInDocument)) - 1, size.width()),
                size.height() + wrapGap()};
    case ImageWrap::Inline:
        break;
    }
    return size;
}

qreal ObjectHandler::columnWidth(QTextDocument *document, int position) const
{
    // No cursor here: placing one lays the line out, which asks for this object's size again.
    qreal width =
        (document->textWidth() > 0 ? document->textWidth() : document->pageSize().width()) -
        (2 * document->documentMargin());
    if (const auto *table = qobject_cast<const QTextTable *>(document->frameAt(position))) {
        const QTextTableFormat format = table->format();
        const int percent = tableWidth(format);
        const qreal tableWidth = (width - format.leftMargin() - format.rightMargin()) *
                                 (percent > 0 ? percent : 100) / 100.0;
        const QList<qreal> widths = columnWidths(format, table->columns());
        width = (tableWidth * widths.value(table->cellAt(position).column()) / 100.0) -
                (2 * format.cellPadding());
    }
    return width;
}

qreal ObjectHandler::availableWidth(QTextDocument *document, int position) const
{
    qreal width = columnWidth(document, position);
    const QTextBlock block = document->findBlock(position);
    width -= block.blockFormat().leftMargin();
    if (const QTextList *list = block.textList()) {
        width -= list->format().indent() * document->indentWidth();
    }
    return std::max(width, 16.0);
}

QSizeF ObjectHandler::imageSize(QTextDocument *document, int position, const QTextFormat &format)
{
    const QString path = filePath(format);
    const QString version = imageVersion(path);
    if (m_naturalSizes.size() > 512) {
        m_naturalSizes.clear();
    }
    auto natural = m_naturalSizes.constFind(version);
    if (natural == m_naturalSizes.constEnd()) {
        // Only the header is read here; the pixels are decoded on a worker thread.
        std::unique_ptr<QFile> file;
        QImageReader reader;
        natural = m_naturalSizes.insert(
            version, openImage(reader, path, file) ? orientedSize(reader, reader.size()) : QSize());
    }
    const qreal available = availableWidth(document, position);
    const int percent = format.intProperty(ImageWidth);
    qreal width = percent > 0 ? available * percent / 100.0 : available;
    if (!natural->isValid() || natural->isEmpty()) {
        // A missing or unreadable file keeps a frame with its name.
        return {std::min(width, static_cast<qreal>(ui::Theme::current()->px(16))),
                static_cast<qreal>(ui::Theme::current()->px(3))};
    }
    // An image is never drawn larger than it is.
    width = std::min(width, static_cast<qreal>(natural->width()));
    return {width, width * natural->height() / natural->width()};
}

QSizeF ObjectHandler::chipSize(QTextDocument *document, int position,
                               const QTextFormat &format) const
{
    const ChipMetrics m = chipMetrics(document);
    const QFontMetricsF metrics(m.font);
    const qreal width =
        (2 * m.padding) + m.glyph + m.gap +
        metrics.horizontalAdvance(format.stringProperty(AttachmentName)) + m.gap +
        metrics.horizontalAdvance(sizeText(format.property(AttachmentSize).toLongLong()));
    return {std::min(width, availableWidth(document, position)), m.height};
}

void ObjectHandler::drawObject(QPainter *painter, const QRectF &rect, QTextDocument *document,
                               int posInDocument, const QTextFormat &format)
{
    const ui::Theme &theme = *ui::Theme::current();
    const ui::Roles &r = m_roles ? *m_roles : theme.roles();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const qreal radius = theme.px(0.375);

    if (format.objectType() == ImageObject) {
        // A wrapped image's rectangle includes the room kept around it.
        const QRectF box(rect.topLeft(), imageSize(document, posInDocument, format));
        const QString path = filePath(format);
        const QSize natural = m_naturalSizes.value(imageVersion(path));
        if (natural.isValid() && !natural.isEmpty()) {
            // An export decodes at twice its size at least, so a PDF prints sharp.
            const qreal ratio = m_roles ? std::max(2.0, painter->device()->devicePixelRatioF())
                                        : painter->device()->devicePixelRatioF();
            const QSize pixels = (box.size() * ratio).toSize();
            if (m_roles) {
                painter->drawImage(box, decodeImage(path, pixels));
            } else if (const QImage *image = imageCache().object(cacheKey(path, pixels))) {
                painter->drawImage(box, *image);
            } else {
                painter->fillRect(box, r.surfaceContainerHighest);
                requestImage(path, pixels);
            }
        } else {
            painter->setPen(QPen(r.outline, 1));
            painter->setBrush(r.surfaceContainerHighest);
            painter->drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
            const int glyph = theme.px(1.25);
            const QRectF icon(box.left() + theme.px(0.75), box.center().y() - (glyph / 2.0), glyph,
                              glyph);
            ui::paintGlyph(*painter, icon, QStringLiteral("Image"), r.onSurfaceVariant);
            painter->setPen(r.onSurfaceVariant);
            const QRectF text =
                box.adjusted(icon.right() - box.left() + theme.px(0.5), 0, -theme.px(0.75), 0);
            painter->drawText(text, Qt::AlignVCenter | Qt::AlignLeft,
                              painter->fontMetrics().elidedText(format.stringProperty(ObjectFile),
                                                                Qt::ElideMiddle,
                                                                static_cast<int>(text.width())));
        }
        painter->restore();
        return;
    }

    // An attachment chip: the file type's glyph, the name and the size.
    const ChipMetrics m = chipMetrics(document);
    painter->setPen(QPen(r.outline, 1));
    painter->setBrush(r.surfaceContainerHighest);
    painter->drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    const QRectF glyph(rect.left() + m.padding, rect.center().y() - (m.glyph / 2), m.glyph,
                       m.glyph);
    ui::paintGlyph(*painter, glyph.adjusted(1, 1, -1, -1),
                   fileTypeGlyph(format.stringProperty(ObjectFile)), r.onSurfaceVariant);
    painter->setFont(m.font);
    const QFontMetricsF metrics(m.font);
    const QString size = sizeText(format.property(AttachmentSize).toLongLong());
    const qreal sizeWidth = metrics.horizontalAdvance(size);
    const QRectF sizeRect(rect.right() - m.padding - sizeWidth, rect.top(), sizeWidth,
                          rect.height());
    const QRectF nameRect(glyph.right() + m.gap, rect.top(),
                          std::max(0.0, sizeRect.left() - m.gap - glyph.right() - m.gap),
                          rect.height());
    painter->setPen(r.onSurface);
    painter->drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft,
                      metrics.elidedText(format.stringProperty(AttachmentName), Qt::ElideMiddle,
                                         nameRect.width()));
    painter->setPen(r.onSurfaceVariant);
    painter->drawText(sizeRect, Qt::AlignVCenter | Qt::AlignRight, size);
    painter->restore();
}

void ObjectHandler::relayoutBesideFloats()
{
    if (m_relayout) {
        return;
    }
    auto *document = static_cast<QTextDocument *>(parent());
    const QAbstractTextDocumentLayout *layout = document->documentLayout();
    const QTextFrame *root = document->rootFrame();
    // The floats of table cells are laid out with their whole table.
    const auto places = [&] {
        QHash<const QTextFrame *, FloatPlace> found;
        for (QTextFrame *frame : floatingFrames(*document)) {
            if (frame->parentFrame() == root) {
                const int position = frame->firstPosition() - 1;
                found.insert(frame,
                             {.position = position,
                              .top = layout->blockBoundingRect(document->findBlock(position)).top(),
                              .rect = layout->frameBoundingRect(frame)});
            }
        }
        return found;
    };
    // Returns the paragraph at a height, looking from a paragraph near it.
    const auto blockAt = [&](QTextBlock block, qreal y) {
        while (block.previous().isValid() && layout->blockBoundingRect(block).top() > y) {
            block = block.previous();
        }
        while (block.next().isValid() && layout->blockBoundingRect(block.next()).top() <= y) {
            block = block.next();
        }
        return block;
    };
    const QHash<const QTextFrame *, FloatPlace> current = places();
    // The text from the first paragraph to lay out again down to this height.
    int from = -1;
    qreal bottom = 0;
    const auto include = [&](int position, qreal y) {
        from = from < 0 ? position : std::min(from, position);
        bottom = std::max(bottom, y);
    };
    for (auto it = m_floats.cbegin(); it != m_floats.cend(); ++it) {
        const auto now = current.constFind(it.key());
        if (now == current.cend() || now->top != it->top || now->rect != it->rect) {
            const QTextBlock near =
                document->findBlock(std::min(it->position, document->characterCount() - 1));
            include(blockAt(near, it->top).position(), it->rect.bottom());
        }
    }
    int firstMoved = -1;
    for (auto it = current.cbegin(); it != current.cend(); ++it) {
        if (!m_floats.contains(it.key())) {
            include(it->position, it->rect.bottom());
        }
        // Paragraphs with a line below the image are laid out again from the first one, so
        // none of them is moved by its lines' height alone.
        for (QTextBlock block = document->findBlock(it->position); block.isValid();
             block = block.next()) {
            const QRectF rect = layout->blockBoundingRect(block);
            if (rect.top() > it->rect.bottom()) {
                break;
            }
            if (QTextCursor(block).currentFrame() != root || !hasLineBelowFloat(block)) {
                continue;
            }
            firstMoved = firstMoved < 0 ? block.position() : std::min(firstMoved, block.position());
            const QTextBlock next = block.next();
            if (next.isValid() && layout->blockBoundingRect(next).top() + 0.5 < rect.bottom()) {
                include(firstMoved, rect.bottom());
            }
        }
    }
    m_floats = current;
    if (from < 0) {
        return;
    }
    const QTextBlock last = blockAt(document->findBlock(from), bottom);
    m_relayout = true;
    document->markContentsDirty(from, last.position() + last.length() - from);
    m_relayout = false;
    m_floats = places();
}

void ObjectHandler::requestImage(const QString &path, const QSize &size)
{
    const QString key = cacheKey(path, size);
    if (size.isEmpty() || m_pending.contains(key)) {
        return;
    }
    m_pending.insert(key);
    QtConcurrent::run([path, size] {
        return decodeImage(path, size);
    }).then(this, [this, key, path, size](const QImage &image) {
        m_pending.remove(key);
        if (!image.isNull() && cacheKey(path, size) == key) {
            const auto kilobytes = std::max<qsizetype>(1, image.sizeInBytes() / 1024);
            imageCache().insert(key, new QImage(image), kilobytes);
            Q_EMIT imageLoaded();
        }
    });
}

} // namespace editor
