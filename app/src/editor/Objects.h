#pragma once

#include "editor/Formats.h"
#include "ui/Palette.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QTextCharFormat>
#include <QTextObjectInterface>

#include <array>
#include <optional>

class QTextCursor;
class QTextDocument;
class QTextFrame;

namespace editor {

/// The image widths of the image menu in percent of the text column: Small,
/// Medium, Large and Full, which is 0.
inline constexpr std::array<int, 4> ImageWidths{25, 50, 75, 0};

/// Returns the character format of an image.
///
/// @param noteId The note whose attachment folder holds the file.
/// @param fileName The file's name.
/// @param width The width in percent of the text column, or 0 for full width.
/// @param wrap How text flows around the image.
QTextCharFormat imageFormat(const QString &noteId, const QString &fileName, int width,
                            ImageWrap wrap = ImageWrap::Inline);

/// Returns how text flows around an image; ImageWrap::Inline for anything else.
ImageWrap imageWrap(const QTextFormat &format);

/// Returns whether a format is that of an image or an attachment chip.
bool isObject(const QTextFormat &format);

/// Inserts an image or an attachment chip at the cursor in place of its selection. An image
/// wrapped square gets the floating frame Qt lays it out with, at the left of its column; the
/// frame holds no text.
///
/// @param cursor The place; it ends after the object.
/// @param format The object's format, as made by imageFormat() or attachmentFormat().
void insertObject(QTextCursor &cursor, const QTextCharFormat &format);

/// Changes how text flows around an image as one undo step. Square wrapping
/// of a full-width image makes it Medium, as no text would fit beside it.
///
/// @param image A cursor selecting the image; it selects the image again afterwards.
/// @param wrap The new wrapping.
void setImageWrap(QTextCursor &image, ImageWrap wrap);

/// Returns the floating frame of the image wrapped square at a position, or nullptr for any
/// other character.
QTextFrame *floatingFrame(const QTextDocument &document, int position);

/// Returns the floating frames of the images wrapped square in a document, at the top level and
/// in tables' cells.
QList<QTextFrame *> floatingFrames(const QTextDocument &document);

/// Returns the character format of an attachment chip.
///
/// @param noteId The note whose attachment folder holds the file.
/// @param fileName The file's name.
/// @param name The name the chip shows.
/// @param size The file's size in bytes.
QTextCharFormat attachmentFormat(const QString &noteId, const QString &fileName,
                                 const QString &name, qint64 size);

/// Returns the format of pasted content that is not stored yet.
///
/// @param type ImageObject or AttachmentObject.
/// @param source A local file path, or a `data:` URL.
QTextCharFormat pendingFormat(ObjectType type, const QString &source);

/// Returns the glyph of a file's type for its attachment chip: PDF, archive, audio, video,
/// image, code, spreadsheet, document or a plain file.
QString fileTypeGlyph(const QString &fileName);

/// Lays out and draws images and attachment chips in a document.
///
/// An image wrapped square floats in a frame whose size includes the room kept between it and
/// the text; one wrapped top and bottom takes its line's full width, so no text fits beside it.
/// Images keep their original bytes in the library; the scaled versions drawn here are decoded
/// on a worker thread into a bounded cache shared by every editor, and the view repaints when
/// one arrives. An image whose file is missing is drawn as a frame with its name.
// Qt takes object handlers as QObjects that implement QTextObjectInterface.
// NOLINTNEXTLINE(misc-multiple-inheritance)
class ObjectHandler : public QObject, public QTextObjectInterface
{
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    /// Registers the handler for images and chips on @p document's layout.
    explicit ObjectHandler(QTextDocument *document);

    /// Registers a handler that draws in fixed colours and decodes each image when it is drawn,
    /// without the shared cache, for a document painted once away from the interface thread,
    /// such as in an export.
    ///
    /// @param document The document.
    /// @param roles The colour roles to draw in.
    ObjectHandler(QTextDocument *document, const ui::Roles &roles);
    ~ObjectHandler() override;

    /// Sets the library whose attachment folders the objects' files are in.
    void setLibraryRoot(const QString &root);

    /// Returns the absolute path of an object's file, or an empty string without a library.
    QString filePath(const QTextFormat &format) const;

    QSizeF intrinsicSize(QTextDocument *document, int posInDocument,
                         const QTextFormat &format) override;
    void drawObject(QPainter *painter, const QRectF &rect, QTextDocument *document,
                    int posInDocument, const QTextFormat &format) override;

Q_SIGNALS:
    /// Emitted when a scaled image has been decoded and can be drawn.
    void imageLoaded();

public:
    /// Returns the width available to an object at a position: its column, less the
    /// indentation of its block. Image widths are percents of it.
    qreal availableWidth(QTextDocument *document, int position) const;

    /// Returns an image's size as shown, from its natural size and its width setting, without
    /// the room a wrapped image keeps around it.
    QSizeF imageSize(QTextDocument *document, int position, const QTextFormat &format);

private:
    /// Returns the width of the column an object at a position is in: the text column or its
    /// table column, less the cell's padding.
    qreal columnWidth(QTextDocument *document, int position) const;

    /// Returns the size of an attachment chip.
    QSizeF chipSize(QTextDocument *document, int position, const QTextFormat &format) const;

    /// Starts decoding an image at a size in device pixels, unless it is cached or running.
    void requestImage(const QString &path, const QSize &size);

    /// Lays out again the paragraphs beside an image wrapped square when Qt left them as they
    /// were: Qt lays out again only the paragraphs an edit touches and moves the others, so text
    /// after an image that was just wrapped, resized, moved or removed keeps its old lines, and a
    /// moved paragraph whose line went below an image loses that room, so the next one starts
    /// too high.
    void relayoutBesideFloats();

    QString m_root;
    /// The fixed colours of a handler that draws images when asked, or no value to follow the
    /// current theme and decode images in the background.
    std::optional<ui::Roles> m_roles;
    /// Natural sizes of the images read so far, by path; an invalid size for a missing file.
    QHash<QString, QSize> m_naturalSizes;
    /// Images being decoded, by cache key.
    QSet<QString> m_pending;
    /// Where an image wrapped square was laid out.
    struct FloatPlace
    {
        /// The image's position in the text when it was laid out.
        int position = 0;
        /// The top of the image's paragraph.
        qreal top = 0;
        /// The rectangle of the image's frame.
        QRectF rect;
    };

    /// Where each image wrapped square was at the last layout.
    QHash<const QTextFrame *, FloatPlace> m_floats;
    /// Set while relayoutBesideFloats() has the layout redone, which reports an update again.
    bool m_relayout = false;
};

} // namespace editor
