#include "core/Attachments.h"
#include "core/Directory.h"
#include "core/Files.h"
#include "editor/Commands.h"
#include "editor/Formats.h"
#include "editor/HtmlCodec.h"
#include "editor/Markdown.h"
#include "editor/Objects.h"
#include "editor/Presenter.h"
#include "editor/TextEdit.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QActionGroup>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextFrame>
#include <QTextTable>
#include <QThreadPool>
#include <QUrl>
#include <QtConcurrentRun>

#include <memory>

namespace editor {

namespace {

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

/// Returns the folder that holds the snapshots of opened attachments, one `open-*` folder each.
QString snapshotFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
           QStringLiteral("/jotter");
}

} // namespace

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

} // namespace editor
