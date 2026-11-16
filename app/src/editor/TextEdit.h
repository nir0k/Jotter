#pragma once

#include "core/Attachments.h"
#include "core/Index.h"
#include "core/Numbering.h"
#include "editor/Decorations.h"

#include <QLineF>
#include <QPointer>
#include <QTextCursor>
#include <QTextEdit>

#include <array>
#include <memory>
#include <optional>

class QLineEdit;
class QMenu;
class QTextDocumentFragment;
class QTextFrame;
class QTextTable;

namespace editor {

class LanguagePopup;
class ObjectHandler;

/// The note body editor: QTextEdit with the list, checklist, code block, table, image,
/// attachment, link and paste behaviour.
///
/// Code blocks get a header drawn over their frame's top margin: the title on the left, the
/// language and the copy button on the right. Numbered list markers and
/// table lines are drawn here too, so they follow the numbering styles and the theme. Images,
/// chips and tables, by the grip above a table, are dragged to another place in the note.
class TextEdit : public QTextEdit
{
    Q_OBJECT

public:
    /// Rebinds this view to a document, sharing its object handler with other views.
    /// @param document A document whose owner keeps it alive until this view detaches.
    void setNoteDocument(QTextDocument *document);

    /// The clipboard format of copied note content: the body vocabulary, read back without loss.
    static constexpr QLatin1StringView BodyMimeType{"application/x-jotter-body"};

    explicit TextEdit(QWidget *parent = nullptr);
    ~TextEdit() override;

    /// Removes the snapshots of opened attachments that an earlier run left behind when it did
    /// not exit normally. Only snapshots older than a day go, as a viewer may still show one.
    /// Runs on a worker thread; call it once at start, while no other Jotter runs.
    static void removeOldSnapshots();

    /// Sets the empty space left and right of the text; the scroll bar stays at the edge.
    void setSideMargin(int margin);

    /// Sets the library whose attachment folders hold the images and files, and the open note,
    /// whose folder takes new files.
    ///
    /// @param libraryRoot The library folder, empty without a library.
    /// @param noteId The open note.
    void setAttachmentContext(const QString &libraryRoot, const QString &noteId);

    /// Sets the notes the body links to that the library knows, for the tooltips of note links;
    /// a link to a note missing here is shown as not found.
    void setLinkedNotes(QHash<QString, core::NoteSummary> notes);

    /// Copies files into the note's attachment folder on a worker thread and inserts them at
    /// the cursor: accepted images as images when @p asImages is set, everything else as
    /// attachment chips. Failures are reported with message().
    void insertFiles(const QStringList &paths, bool asImages);

    /// Pastes the clipboard's text without formatting (Ctrl+Shift+V).
    void pastePlainText();

    /// Copies the selection as Markdown text (Ctrl+Shift+C), without HTML or private formats.
    void copyMarkdown();

Q_SIGNALS:
    /// Emitted with a confirmation or a failure to show as a transient message, such as copied
    /// code or a file that could not be added.
    void message(const QString &text);
    /// Emitted when `[[` was typed outside a code block, to choose a note to link to.
    void noteLinkRequested();
    /// Emitted when a link to a note is activated with Ctrl+click.
    void noteLinkActivated(const QString &noteId);

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;
    QMimeData *createMimeDataFromSelection() const override;

private:
    /// A file a pasted or inserted object needs in the note's folder before it is inserted.
    struct PendingFile
    {
        /// The object's position in the content being inserted.
        int position = 0;
        int type = 0;
        /// The file to copy, or empty when @p data holds the bytes.
        QString path;
        QByteArray data;
        /// The file name for @p data.
        QString name;
        /// The format the object had, for the width of an image and the name of a chip.
        QTextCharFormat format;
        /// How @p path is copied: a local file named by pasted HTML has to be an image.
        core::StoreAs storeAs = core::StoreAs::File;
    };
    /// Returns the code blocks in view, top to bottom, in viewport coordinates.
    QList<CodeHeader> codeHeaders() const;

    /// Returns the code block whose box contains @p viewportPosition, if any.
    std::optional<CodeHeader> codeHeaderAt(const QPoint &viewportPosition) const;

    /// Draws the code blocks' headers. The title placeholder shows only on the block under the
    /// mouse or with the cursor, so a block without a title shows none.
    void paintCodeHeaders(QPainter &painter, const QList<CodeHeader> &headers) const;

    /// Handles a click on a code block's header.
    ///
    /// @return false when @p viewportPosition is not on a header's control.
    bool clickCodeHeader(const QPoint &viewportPosition);

    /// Copies a code block's text to the clipboard and confirms it.
    void copyCode(const QTextFrame &frame);

    /// Opens the language list for a code block at a point of the screen.
    void chooseLanguage(QTextFrame *frame, const QPoint &globalPosition);

    /// Shows the title field over a code block's header; Return keeps the title, Escape
    /// cancels, and the cursor goes back to the code.
    void editTitle(QTextFrame *frame);

    /// Ends editing a code block's title, keeping the typed title when @p keep is true.
    void finishTitle(bool keep);

    /// Handles Return: leaves an empty list item or quote, starts the next block as a
    /// paragraph after a heading and unchecked after a checklist item, keeps the indentation in
    /// code, and applies the Markdown shortcuts of a whole line.
    void handleReturn(QKeyEvent *event);

    /// Toggles a checklist item when @p viewportPosition is on its checkbox.
    bool toggleCheckboxAt(const QPoint &viewportPosition);

    /// Turns the address typed before the cursor into a link once a space follows it.
    void linkTypedAddress();

    /// A column line or a table's right edge being dragged. A column line
    /// moves between the outer edges of the two columns it separates; the right edge sets the
    /// table's width, from its left edge.
    struct ColumnDrag
    {
        QPointer<QTextTable> table;
        /// The index of the column right of the line, or -1 for the table's right edge.
        int column = 0;
        /// The table's width, which the minimum column width is a share of; for the right edge,
        /// the text column's width, which the table's width is a share of.
        qreal tableWidth = 0;
        qreal left = 0;
        qreal right = 0;
        qreal x = 0;
    };

    /// Content pressed on to be moved within the note: an image, a chip, or a table by its grip.
    /// It is moved once the mouse goes beyond the drag distance.
    struct MoveSource
    {
        /// A cursor selecting the content.
        QTextCursor content;
        /// The table moved, or nullptr for an image or a chip.
        QPointer<QTextTable> table;
        /// Where the mouse was pressed, in viewport coordinates.
        QPoint start;
    };

    /// Where moved content would go: a position for the content, or a place of moveTable() for
    /// a table, and the line that shows it, in viewport coordinates.
    struct DropPlace
    {
        int position = 0;
        QLineF mark;
    };

    /// An image being resized by one of its handles.
    struct ImageDrag
    {
        QTextCursor image;
        /// The image's rectangle when the drag started.
        QRectF start;
        /// Whether a left handle is dragged, so the width grows to the left.
        bool fromLeft = false;
        qreal width = 0;
    };

    /// Returns the tables in view, top to bottom, in viewport coordinates.
    QList<TableGeometry> tableGeometries() const;

    /// Starts dragging the column line or the table's right edge at a point of the viewport, if
    /// there is one.
    ///
    /// @return false when the point is not on a column line or a right edge.
    bool startColumnDrag(const QPoint &viewportPosition);

    /// Sets a column's width or the table's width from the drag that ended.
    void finishColumnDrag();

    /// Returns the rectangle of the grip a table is dragged by, above its top left corner.
    QRectF tableGrip(const TableGeometry &geometry) const;

    /// Draws the grip of the table under the mouse and of the table with the cursor.
    void paintTableGrips(QPainter &painter, const QList<TableGeometry> &tables) const;

    /// Returns the table whose grip is at a point of the viewport, if any.
    QTextTable *tableGripAt(const QPoint &viewportPosition) const;

    /// Starts moving content within the note by drag and drop; returns when it was dropped.
    void startMove(const MoveSource &source);

    /// Returns where the content being moved would go if dropped at a point of the viewport: a
    /// table goes between top-level paragraphs and frames, outside itself; an image or a chip
    /// goes where the text cursor would, outside code blocks and itself.
    std::optional<DropPlace> dropPlace(const QPoint &viewportPosition) const;

    /// Moves an image or a chip to a position as one undo step; a moved image stays selected.
    void moveObject(const QTextCursor &object, int position);

    /// Moves a table past the paragraph or the frame above (@p direction -1) or below (1),
    /// keeping the cursor in its cell.
    void moveTableBy(QTextTable *table, int direction, const QTextCursor &cell);

    /// Draws the handles of the selected image and the outline of a resize or column drag.
    void paintDragFeedback(QPainter &painter) const;

    /// Returns the rectangle of an image or a chip in viewport coordinates.
    QRectF objectRect(const QTextCursor &object) const;

    /// Returns the selected image, whose handles are shown: the selection when it is exactly one
    /// image.
    std::optional<QTextCursor> selectedImage() const;

    /// Returns the handles of an image's rectangle: top left, top right, bottom left and bottom
    /// right.
    std::array<QRectF, 4> imageHandles(const QRectF &rect) const;

    /// Sets an image's width from a drag: in percent of the text column, snapping to the menu's
    /// widths, as one undo step.
    void finishImageDrag();

    /// Draws the quote bars and the markers of numbered list items in view, after the text;
    /// the document format can draw neither.
    void paintBlockDecorations(QPainter &painter);

    /// Returns a cursor selecting the image or chip under a point of the viewport, if any; a
    /// wrapped image is found where it floats.
    std::optional<QTextCursor> objectAt(const QPoint &viewportPosition) const;

    /// Returns a cursor selecting the image or chip next to the text cursor, for the keyboard.
    std::optional<QTextCursor> objectAtCursor() const;

    /// Adds the actions of a table, an image or a chip to a context menu.
    void addObjectActions(QMenu &menu, const QTextCursor &at, const QPoint &globalPosition);

    /// Opens an object's file with the desktop's default application. Programs are refused,
    /// with Show in folder offered instead.
    void openObject(const QTextCursor &object);

    /// Shows the folder of an object's file in the file manager.
    void showObjectFolder(const QTextCursor &object);

    /// Saves a copy of a chip's file where the person chooses, on a worker thread.
    void saveObjectAs(const QTextCursor &object);

    /// Inserts a document at a cursor as one undo step and moves the editor's cursor after it;
    /// a divider that starts it stays a divider.
    ///
    /// @param content The content to insert, with its files already in this note's folder.
    /// @param cursor Where to insert it.
    void insertDocument(const QTextDocument &content, QTextCursor cursor);

    /// Inserts content at a cursor: its pasted images and the files of other notes are stored
    /// in this note's folder first, on a worker thread, and the content follows when they are.
    void insertContent(const std::shared_ptr<QTextDocument> &content, const QTextCursor &at);

    /// Lists the objects of content that need a file stored before it is inserted.
    QList<PendingFile> pendingFiles(const QTextDocument &content) const;

    /// Returns the folder of the open note's attachments, empty without one.
    QString attachmentFolder() const;

    /// The code block under the mouse, for its title placeholder and button highlight.
    QPointer<QTextFrame> m_hoverCode;
    /// The code block with the cursor.
    QPointer<QTextFrame> m_cursorCode;
    /// The code block whose title is being edited.
    QPointer<QTextFrame> m_titleFrame;
    QLineEdit *m_titleEdit;
    LanguagePopup *m_languages;
    QPointer<QTextFrame> m_languageFrame;
    ObjectHandler *m_objects;
    QString m_libraryRoot;
    QString m_noteId;
    QHash<QString, core::NoteSummary> m_linkedNotes;
    /// The numbered list markers by block number; made again after the document changes.
    QHash<int, QString> m_markers;
    bool m_markersValid = false;
    std::optional<ColumnDrag> m_columnDrag;
    std::optional<ImageDrag> m_imageDrag;
    /// Content pressed on, until it is moved or the mouse is released.
    std::optional<MoveSource> m_press;
    /// Content being moved by drag and drop.
    std::optional<MoveSource> m_move;
    /// The line showing where moved content would go; null without one.
    QLineF m_dropMark;
    /// The table under the mouse, whose grip is shown.
    QPointer<QTextTable> m_hoverTable;
};

} // namespace editor
