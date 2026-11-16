#include "editor/Export.h"

#include "core/Export.h"
#include "core/Files.h"
#include "core/NoteLinks.h"
#include "editor/Decorations.h"
#include "editor/HtmlCodec.h"
#include "editor/Objects.h"
#include "editor/Presenter.h"
#include "ui/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QMutex>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextList>

#include <KSyntaxHighlighting/Repository>
#include <KZip>

#include <functional>
#include <memory>
#include <optional>

namespace editor {

namespace {

/// The width of a JPG's text column and the scale it is drawn at.
constexpr int JpgColumn = 760;
constexpr qreal JpgScale = 2.0;
/// The tallest JPG part in pixels and its quality.
constexpr int JpgMaxHeight = 16000;
constexpr int JpgQuality = 90;

/// The translation context of the export's texts, which have no widget of their own.
struct ExportText
{
    Q_DECLARE_TR_FUNCTIONS(editor::Export)
};

/// Returns the syntax definitions of exports, read on the first export that needs them and kept
/// for the next. The shared repository serves the interface thread only, and a repository of its
/// own costs each export the reading of every definition. Hold exportLock() while using it.
const KSyntaxHighlighting::Repository &exportRepository()
{
    static const KSyntaxHighlighting::Repository repository;
    return repository;
}

/// Serialises exports that draw, as they share exportRepository().
QMutex &exportLock()
{
    static QMutex mutex;
    return mutex;
}

/// A note laid out as the editor shows it, in fixed colours, with its title and updated date at
/// the top, ready to be painted page by page or part by part on a worker thread.
class NoteDocument
{
public:
    /// Reads the note's body into a document of its own.
    ///
    /// @param library The library holding the note's attachments.
    /// @param note The note; its title is the one shown.
    /// @param subtitle The line under the title.
    /// @param roles The colours to draw in.
    /// @param repository The syntax definitions of code blocks.
    NoteDocument(const core::Library &library, const core::Note &note, const QString &subtitle,
                 const ui::Roles &roles, const KSyntaxHighlighting::Repository &repository)
        : m_roles(roles)
        , m_repository(repository)
    {
        const ui::Theme &theme = *ui::Theme::current();
        m_document.setDocumentMargin(0);
        m_document.setDefaultFont(theme.font(ui::TextRole::Body));
        m_document.setIndentWidth(theme.px(2.0));
        QTextOption option = m_document.defaultTextOption();
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        m_document.setDefaultTextOption(option);
        // Owned by the document.
        auto *objects = new ObjectHandler(&m_document, roles);
        objects->setLibraryRoot(library.root());
        readBody(m_document, note.body);
        unlinkNotes();
        addHeading(note.meta.title, subtitle);
        auto *presenter = new Presenter(&m_document, roles, repository);
        // Without an event loop the presenter's own delayed pass would never run.
        presenter->rehighlight();
        m_markers = listMarkers(m_document);
    }

    QTextDocument &document() { return m_document; }

    /// Paints a part of the document; the painter is set up so that document coordinates land
    /// where the part belongs.
    ///
    /// @param painter The painter.
    /// @param area The part, in document coordinates; nothing outside it is drawn.
    void paint(QPainter &painter, const QRectF &area)
    {
        painter.save();
        painter.setClipRect(area, Qt::IntersectClip);
        painter.setRenderHint(QPainter::Antialiasing);
        const QList<CodeHeader> headers = codeHeaders(m_document, {}, area.bottom(), m_repository);
        paintCodeBackgrounds(painter, headers, m_roles);
        paintTables(painter, tableGeometries(m_document, {}, area.bottom()), m_roles);
        paintInlineCode(painter, m_document.begin(), m_roles, {}, area.bottom());
        QAbstractTextDocumentLayout::PaintContext context;
        context.clip = area;
        context.palette.setColor(QPalette::Text, m_roles.onSurface);
        m_document.documentLayout()->draw(&painter, context);
        for (const CodeHeader &header : headers) {
            paintCodeLabels(painter, header, m_roles, true);
        }
        paintCodeLineNumbers(painter, headers, m_roles, {}, area.bottom());
        paintBlockDecorations(painter, m_document.begin(), m_markers, m_roles, {}, area.bottom());
        painter.restore();
    }

private:
    /// Turns links between notes into text drawn as a link without a target, as the notes they
    /// point to are not part of a PDF or an image.
    void unlinkNotes()
    {
        QList<std::pair<QTextCursor, QTextCharFormat>> links;
        for (QTextBlock block = m_document.begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                QTextCharFormat format = fragment.charFormat();
                if (!format.isAnchor() || !core::noteLinkTarget(format.anchorHref())) {
                    continue;
                }
                QTextCursor cursor(&m_document);
                cursor.setPosition(fragment.position());
                cursor.setPosition(fragment.position() + fragment.length(),
                                   QTextCursor::KeepAnchor);
                format.setAnchor(false);
                format.clearProperty(QTextFormat::AnchorHref);
                format.setForeground(m_roles.primary);
                format.setFontUnderline(true);
                links.append({cursor, format});
            }
        }
        for (auto &[cursor, format] : links) {
            cursor.setCharFormat(format);
        }
    }

    /// Puts the title and the subtitle above the body as plain paragraphs of their own.
    void addHeading(const QString &title, const QString &subtitle)
    {
        const ui::Theme &theme = *ui::Theme::current();
        QTextCursor cursor(&m_document);
        // The first paragraph is split, so the body keeps its own first paragraph as it was.
        cursor.insertBlock();
        cursor.movePosition(QTextCursor::Start);
        if (QTextList *list = cursor.currentList()) {
            list->remove(cursor.block());
        }
        cursor.setBlockFormat(QTextBlockFormat());
        QTextCharFormat titleFormat;
        titleFormat.setFont(theme.font(ui::TextRole::PageTitle));
        titleFormat.setForeground(m_roles.onSurface);
        cursor.insertText(title, titleFormat);
        QTextBlockFormat subtitleBlock;
        subtitleBlock.setTopMargin(theme.px(0.25));
        subtitleBlock.setBottomMargin(theme.px(1.25));
        QTextCharFormat subtitleFormat;
        subtitleFormat.setFont(theme.font(ui::TextRole::BodySmall));
        subtitleFormat.setForeground(m_roles.onSurfaceVariant);
        cursor.insertBlock(subtitleBlock, subtitleFormat);
        cursor.insertText(subtitle);
    }

    QTextDocument m_document;
    ui::Roles m_roles;
    const KSyntaxHighlighting::Repository &m_repository;
    QHash<int, QString> m_markers;
};

/// Draws a note into a PDF of pages of a size, with live web links and embedded fonts.
///
/// @param note The laid-out note.
/// @param title The document title of the PDF.
/// @param pageSize The page size.
/// @param canceled Tells whether the export was cancelled; checked before each page.
/// @return The PDF, or no value when cancelled.
std::optional<QByteArray> renderPdf(NoteDocument &note, const QString &title,
                                    QPageSize::PageSizeId pageSize,
                                    const std::function<bool()> &canceled)
{
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    QPdfWriter writer(&buffer);
    // Pixels of the interface's 96 per inch, so the editor's sizes print at their size.
    writer.setResolution(96);
    writer.setPageSize(QPageSize(pageSize));
    writer.setPageMargins(QMarginsF(20, 20, 20, 20), QPageLayout::Millimeter);
    writer.setTitle(title);
    writer.setCreator(QStringLiteral("Jotter"));
    const QRectF page = writer.pageLayout().paintRectPixels(writer.resolution());
    QTextDocument &document = note.document();
    // A page size makes the layout move lines that would cross a page to the next one.
    document.setPageSize(page.size());
    QPainter painter(&writer);
    for (int i = 0; i < document.pageCount(); ++i) {
        if (canceled()) {
            return std::nullopt;
        }
        if (i > 0) {
            writer.newPage();
        }
        const qreal top = i * page.height();
        painter.save();
        painter.translate(0, -top);
        note.paint(painter, QRectF(0, top, page.width(), page.height()));
        painter.restore();
    }
    painter.end();
    return buffer.data();
}

/// Returns where the parts of a tall note start, so that no part is taller than a limit: at
/// the top of a paragraph or a frame of the top level, and inside one only when it is taller
/// than the limit by itself.
QList<qreal> partStarts(QTextDocument &document, qreal limit)
{
    QList<qreal> starts{0};
    const QAbstractTextDocumentLayout *layout = document.documentLayout();
    const auto add = [&](const QRectF &rect) {
        if (rect.bottom() - starts.last() <= limit) {
            return;
        }
        if (rect.top() > starts.last()) {
            starts.append(rect.top());
        }
        while (rect.bottom() - starts.last() > limit) {
            starts.append(starts.last() + limit);
        }
    };
    for (auto it = document.rootFrame()->begin(); !it.atEnd(); ++it) {
        if (QTextFrame *frame = it.currentFrame()) {
            add(layout->frameBoundingRect(frame));
        } else {
            add(layout->blockBoundingRect(it.currentBlock()));
        }
    }
    return starts;
}

/// Draws a note as JPG images on a 760-pixel column at twice its size, split into parts no
/// taller than 16,000 pixels.
///
/// @param note The laid-out note.
/// @param background The colour around the text.
/// @param canceled Tells whether the export was cancelled; checked before each part.
/// @return The images, or no value when cancelled.
std::optional<QList<QByteArray>> renderJpg(NoteDocument &note, const QColor &background,
                                           const std::function<bool()> &canceled)
{
    const qreal margin = ui::Theme::current()->px(2.0);
    QTextDocument &document = note.document();
    document.setTextWidth(JpgColumn);
    const qreal height = document.size().height();
    const QList<qreal> starts = partStarts(document, (JpgMaxHeight / JpgScale) - (2 * margin));
    QList<QByteArray> parts;
    for (qsizetype i = 0; i < starts.size(); ++i) {
        if (canceled()) {
            return std::nullopt;
        }
        const qreal top = starts.at(i);
        const qreal bottom = i + 1 < starts.size() ? starts.at(i + 1) : height;
        const QSizeF size(JpgColumn + (2 * margin), bottom - top + (2 * margin));
        QImage image((size * JpgScale).toSize(), QImage::Format_RGB32);
        image.setDevicePixelRatio(JpgScale);
        image.fill(background);
        QPainter painter(&image);
        painter.translate(margin, margin - top);
        note.paint(painter, QRectF(0, top, JpgColumn, bottom - top));
        painter.end();
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "JPG", JpgQuality);
        parts.append(buffer.data());
    }
    return parts;
}

/// Adds the files of one note to an archive; a library file that has gone is left out, as its
/// link in the note already leads nowhere.
///
/// @return Nothing, or Io with the archive's path.
core::Result<void> addToZip(KZip &zip, const QList<core::ExportFile> &files)
{
    for (const core::ExportFile &file : files) {
        QByteArray data = file.data;
        if (!file.source.isEmpty()) {
            auto bytes = core::readFile(file.source);
            if (!bytes) {
                if (bytes.error().code == core::ErrorCode::NotFound) {
                    continue;
                }
                return std::unexpected(bytes.error());
            }
            data = std::move(*bytes);
        }
        const bool added = zip.writeFile(file.path, data);
        if (!added) {
            return core::fail(core::ErrorCode::Io, zip.fileName(), zip.errorString());
        }
    }
    return {};
}

} // namespace

void exportNotes(QPromise<core::Result<ExportResult>> &promise, const ExportRequest &request)
{
    const core::Library &library = request.library;
    promise.setProgressRange(0, static_cast<int>(request.noteIds.size()));
    const QString untitled = ExportText::tr("Untitled");

    // Names first: an HTML note links to the files of the other notes of the export.
    QList<core::Note> notes;
    QHash<QString, QString> bases;
    core::ExportNames names;
    for (const QString &id : request.noteIds) {
        auto note = library.readNote(id);
        if (!note) {
            promise.addResult(std::unexpected(note.error()));
            return;
        }
        if (note->meta.title.trimmed().isEmpty()) {
            note->meta.title = untitled;
        }
        bases.insert(id, names.take(core::exportBaseName(note->meta.title, untitled)));
        notes.append(*std::move(note));
    }

    QTemporaryFile temporary(QDir::tempPath() + QStringLiteral("/jotter-export-XXXXXX"));
    temporary.setAutoRemove(false);
    if (!temporary.open()) {
        promise.addResult(
            core::fail(core::ErrorCode::Io, temporary.fileTemplate(), temporary.errorString()));
        return;
    }
    const QString path = temporary.fileName();
    temporary.close();
    std::unique_ptr<KZip> zip;
    // Leaves no temporary file behind unless the export reaches the person. An open archive is
    // closed first, as closing it writes the file again.
    const auto discard = [&] {
        zip.reset();
        QFile::remove(path);
    };
    const auto failWith = [&](const core::Error &error) {
        discard();
        promise.addResult(std::unexpected(error));
    };
    const auto canceled = [&promise] { return promise.isCanceled(); };

    if (notes.size() > 1) {
        zip = std::make_unique<KZip>(path);
        if (!zip->open(QIODevice::WriteOnly)) {
            failWith({.code = core::ErrorCode::Io, .path = path, .detail = zip->errorString()});
            return;
        }
    }
    std::optional<QMutexLocker<QMutex>> drawing;
    if (request.format != ExportFormat::Html) {
        drawing.emplace(&exportLock());
    }
    const ui::Roles roles = ui::roles(ui::PaletteId::Warm, false);
    QList<core::ExportFile> files;
    for (qsizetype i = 0; i < notes.size(); ++i) {
        if (canceled()) {
            discard();
            return;
        }
        const core::Note &note = notes.at(i);
        const QString &base = bases.value(note.meta.id);
        const QString subtitle =
            ExportText::tr("Updated %1").arg(core::displayTime(note.meta.updated));
        files.clear();
        if (request.format == ExportFormat::Html) {
            files = core::exportHtml(library, note, base, bases, subtitle);
        } else {
            NoteDocument document(library, note, subtitle, roles, exportRepository());
            if (request.format == ExportFormat::Pdf) {
                const auto pdf = renderPdf(document, note.meta.title, request.pageSize, canceled);
                if (!pdf) {
                    discard();
                    return;
                }
                files.append({.path = base + QStringLiteral(".pdf"), .data = *pdf, .source = {}});
            } else {
                const auto parts = renderJpg(document, roles.surface, canceled);
                if (!parts) {
                    discard();
                    return;
                }
                for (qsizetype part = 0; part < parts->size(); ++part) {
                    const QString name =
                        parts->size() == 1 ? base : QStringLiteral("%1-%2").arg(base).arg(part + 1);
                    files.append({.path = name + QStringLiteral(".jpg"),
                                  .data = parts->at(part),
                                  .source = {}});
                }
            }
        }
        if (zip) {
            if (auto added = addToZip(*zip, files); !added) {
                failWith(added.error());
                return;
            }
        }
        promise.setProgressValue(static_cast<int>(i + 1));
    }

    ExportResult result{.temporaryPath = path, .fileName = {}};
    if (zip) {
        result.fileName = ExportText::tr("Notes") + QStringLiteral(".zip");
    } else if (files.size() == 1 && files.first().source.isEmpty()) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(files.first().data) < 0) {
            failWith({.code = core::ErrorCode::Io, .path = path, .detail = file.errorString()});
            return;
        }
        result.fileName = files.first().path;
    } else {
        // One note that made several files: its folder of attachments or its parts.
        zip = std::make_unique<KZip>(path);
        if (!zip->open(QIODevice::WriteOnly)) {
            failWith({.code = core::ErrorCode::Io, .path = path, .detail = zip->errorString()});
            return;
        }
        if (auto added = addToZip(*zip, files); !added) {
            failWith(added.error());
            return;
        }
        result.fileName = bases.value(notes.first().meta.id) + QStringLiteral(".zip");
    }
    if (zip && !zip->close()) {
        failWith({.code = core::ErrorCode::Io, .path = path, .detail = zip->errorString()});
        return;
    }
    promise.addResult(result);
}

core::Result<void> saveExport(const QString &temporaryPath, const QString &target)
{
    const auto copy = [&]() -> core::Result<void> {
        QFile source(temporaryPath);
        QSaveFile file(target);
        // A file shared through the document portal can only be written in place.
        file.setDirectWriteFallback(true);
        if (!source.open(QIODevice::ReadOnly)) {
            return core::fail(core::ErrorCode::Io, target, source.errorString());
        }
        if (!file.open(QIODevice::WriteOnly)) {
            return core::fail(core::ErrorCode::Io, target, file.errorString());
        }
        constexpr qint64 Chunk = qint64{1} << 20;
        while (!source.atEnd()) {
            const QByteArray bytes = source.read(Chunk);
            if (bytes.isEmpty() || file.write(bytes) != bytes.size()) {
                file.cancelWriting();
                return core::fail(core::ErrorCode::Io, target, file.errorString());
            }
        }
        if (!file.commit()) {
            return core::fail(core::ErrorCode::Io, target, file.errorString());
        }
        return {};
    };
    core::Result<void> saved = copy();
    QFile::remove(temporaryPath);
    return saved;
}

} // namespace editor
