#include "core/Note.h"

#include "core/Files.h"
#include "core/Html.h"
#include "core/Numbering.h"
#include "core/TagColors.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QSet>

#include <cmath>

namespace core {

namespace {

constexpr QLatin1StringView MetaOpen(R"(<script type="application/json" id="jotter-meta">)");
constexpr QLatin1StringView MetaClose("</script>");
constexpr QLatin1StringView BodyOpen(R"(<article class="jotter-body">)");
constexpr QLatin1StringView BodyClose("</article>");

// Readable defaults for a browser in the light Warm palette: typography, quotes, checklists,
// code block titles, tables, image wrapping, attachment chips and the tag colours of text and
// highlights.
constexpr QLatin1StringView BaseStyleSheet(
    "body{max-width:46rem;margin:2rem auto;padding:0 1rem;"
    "font-family:system-ui,sans-serif;line-height:1.5;color:#18181b;background:#fff}"
    ".jotter-header{border-bottom:1px solid #eeeae7;margin-bottom:1.5rem}"
    ".jotter-header p{color:#5d5d5f}"
    "blockquote{margin:1rem 0;padding:.25rem 1rem;border-left:3px solid #bdb6af;color:#4d4d50}"
    "ul.checklist{list-style:none;padding-left:1.25rem}"
    "ul.checklist>li::before{content:'\\2610';margin:0 .4rem 0 -1.25rem}"
    "ul.checklist>li[data-checked=true]::before{content:'\\2611'}"
    "ul.checklist>li[data-checked=true]{text-decoration:line-through;color:#6b6b6e}"
    "mark{color:inherit}"
    "pre{background:#faf8f6;border:1px solid #eeeae7;border-radius:.375rem;padding:.75rem;"
    "overflow-x:auto}"
    "pre[data-title]::before{content:attr(data-title);display:block;margin-bottom:.5rem;"
    "font:600 .875rem system-ui,sans-serif}"
    "table{border-collapse:collapse;width:100%;table-layout:fixed}"
    "td,th{border:1px solid #bdb6af;padding:.25rem .5rem;vertical-align:top;text-align:left;"
    "overflow-wrap:anywhere}"
    "th{background:#faf8f6}td>p,th>p{margin:0}"
    "img{max-width:100%;vertical-align:bottom}"
    "img[data-wrap=square]{float:left;margin:0 .75rem .75rem 0}"
    "img[data-wrap=top-bottom]{display:block;margin-bottom:.75rem}"
    "a.attachment{display:inline-block;padding:0 .5rem;border:1px solid #eeeae7;"
    "border-radius:.375rem;background:#faf8f6;color:inherit;text-decoration:none}"
    "a.attachment::before{content:'\\1F4CE\\FE0E';margin-right:.25rem}");

/// Returns the note style sheet: the base rules and one text and one highlight rule per tag
/// colour. Highlights are the colour at 30% over white, so black text stays readable.
const QString &noteStyleSheet()
{
    static const QString sheet = [] {
        QString css = BaseStyleSheet;
        for (const TagColorValue &color : TagColorValues) {
            css += QStringLiteral("[data-color=%1]{color:#%2}mark[data-color=%1]{background:"
                                  "#%3;color:inherit}")
                       .arg(QLatin1StringView(color.key))
                       .arg(color.rgb, 6, 16, QLatin1Char('0'))
                       .arg(paperHighlight(color.rgb), 6, 16, QLatin1Char('0'));
        }
        return css;
    }();
    return sheet;
}

/// Returns the rules that give a browser the widths of a body's images and tables, one per
/// element and width used.
QString widthStyleSheet(QStringView body)
{
    QSet<int> images;
    QSet<int> tables;
    for (const html::Token &token : html::tokenize(body)) {
        if (token.kind != html::Token::Kind::Start) {
            continue;
        }
        const bool image = token.name == QLatin1StringView("img");
        if (!image && token.name != QLatin1StringView("table")) {
            continue;
        }
        const int width = token.attribute(u"data-width").toInt();
        if (width > 0 && width < 100) {
            (image ? images : tables).insert(width);
        }
    }
    QString css;
    const auto addRules = [&css](const QSet<int> &widths, const QString &rule) {
        QList<int> sorted(widths.begin(), widths.end());
        std::ranges::sort(sorted);
        for (const int width : sorted) {
            css += rule.arg(width);
        }
    };
    addRules(images, QStringLiteral("img[data-width=\"%1\"]{max-width:%1%}"));
    addRules(tables, QStringLiteral("table[data-width=\"%1\"]{width:%1%}"));
    return css;
}

QJsonValue optionalTime(const std::optional<QDateTime> &value)
{
    return value ? QJsonValue(formatTime(*value)) : QJsonValue(QJsonValue::Null);
}

/// Builds the metadata object; unknown fields read from the file are written back unchanged.
QJsonObject metaToJson(const NoteMeta &meta)
{
    QJsonObject object = meta.extra;
    object.insert(QStringLiteral("format"), NoteFormat);
    object.insert(QStringLiteral("id"), meta.id);
    object.insert(QStringLiteral("title"), meta.title);
    object.insert(QStringLiteral("icon"), optionalString(meta.icon));
    object.insert(QStringLiteral("color"), optionalString(meta.color));
    object.insert(QStringLiteral("created"), formatTime(meta.created));
    object.insert(QStringLiteral("updated"), formatTime(meta.updated));
    object.insert(QStringLiteral("collections"), QJsonArray::fromStringList(meta.collections));
    object.insert(QStringLiteral("tags"), QJsonArray::fromStringList(meta.tags));
    object.insert(QStringLiteral("archived"), optionalTime(meta.archived));
    object.insert(QStringLiteral("trashed"), optionalTime(meta.trashed));
    if (meta.location) {
        QJsonObject location{
            {QStringLiteral("lat"), meta.location->latitude},
            {QStringLiteral("lon"), meta.location->longitude},
            {QStringLiteral("captured"), formatTime(meta.location->captured)},
        };
        location.insert(QStringLiteral("accuracy"), meta.location->accuracy
                                                        ? QJsonValue(*meta.location->accuracy)
                                                        : QJsonValue(QJsonValue::Null));
        object.insert(QStringLiteral("location"), location);
    } else {
        object.insert(QStringLiteral("location"), QJsonValue::Null);
    }
    return object;
}

/// Reads metadata fields with type checks; the first wrong field names the error.
class MetaReader
{
public:
    explicit MetaReader(const QJsonObject &object)
        : m_object(object)
    {}

    /// The first problem found, empty while the metadata is valid.
    const QString &problem() const { return m_problem; }

    QString string(const QString &key, bool required)
    {
        const QJsonValue value = m_object.value(key);
        if (value.isString()) {
            return value.toString();
        }
        if (required || !value.isUndefined()) {
            report(key);
        }
        return {};
    }

    std::optional<QString> optionalString(const QString &key)
    {
        const QJsonValue value = m_object.value(key);
        if (value.isString()) {
            return value.toString();
        }
        if (!value.isNull() && !value.isUndefined()) {
            report(key);
        }
        return std::nullopt;
    }

    QDateTime time(const QString &key)
    {
        if (auto parsed = parseTime(m_object.value(key).toString())) {
            return *parsed;
        }
        report(key);
        return {};
    }

    std::optional<QDateTime> optionalTime(const QString &key)
    {
        const QJsonValue value = m_object.value(key);
        if (value.isNull() || value.isUndefined()) {
            return std::nullopt;
        }
        if (auto parsed = parseTime(value.toString())) {
            return parsed;
        }
        report(key);
        return std::nullopt;
    }

    QStringList strings(const QString &key)
    {
        const QJsonValue value = m_object.value(key);
        if (value.isUndefined()) {
            return {};
        }
        QStringList list;
        if (!value.isArray()) {
            report(key);
            return list;
        }
        for (const QJsonValue item : value.toArray()) {
            if (!item.isString()) {
                report(key);
                return {};
            }
            list.append(item.toString());
        }
        return list;
    }

    std::optional<GeoLocation> location()
    {
        const QJsonValue value = m_object.value(QStringLiteral("location"));
        if (value.isNull() || value.isUndefined()) {
            return std::nullopt;
        }
        const QJsonObject object = value.toObject();
        const QJsonValue lat = object.value(QStringLiteral("lat"));
        const QJsonValue lon = object.value(QStringLiteral("lon"));
        const QJsonValue accuracy = object.value(QStringLiteral("accuracy"));
        const auto captured = parseTime(object.value(QStringLiteral("captured")).toString());
        if (!lat.isDouble() || !lon.isDouble() || qAbs(lat.toDouble()) > 90.0 ||
            qAbs(lon.toDouble()) > 180.0 || !captured ||
            (!accuracy.isDouble() && !accuracy.isNull() && !accuracy.isUndefined())) {
            report(QStringLiteral("location"));
            return std::nullopt;
        }
        GeoLocation location{.latitude = lat.toDouble(),
                             .longitude = lon.toDouble(),
                             .accuracy = std::nullopt,
                             .captured = *captured};
        if (accuracy.isDouble()) {
            location.accuracy = accuracy.toDouble();
        }
        return location;
    }

private:
    void report(const QString &key)
    {
        if (m_problem.isEmpty()) {
            m_problem = QStringLiteral("invalid field \"%1\"").arg(key);
        }
    }

    const QJsonObject &m_object;
    QString m_problem;
};

/// Builds a complete note document: the title, the head block, the style sheet, the generated
/// header and the body.
///
/// @param meta The note's metadata, for the title and tags.
/// @param body The body HTML.
/// @param head Extra elements of the head, such as the metadata block.
/// @param subtitle A line shown under the title, or empty for none.
/// @return The document in UTF-8.
QByteArray noteHtml(const NoteMeta &meta, QStringView body, const QString &head,
                    const QString &subtitle)
{
    QString tags;
    for (const QString &tag : meta.tags) {
        tags += (tags.isEmpty() ? QStringLiteral("#") : QStringLiteral(" #")) + tag.toHtmlEscaped();
    }
    const QString title = meta.title.toHtmlEscaped();

    QString html;
    html += QStringLiteral("<!doctype html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n");
    html += QStringLiteral("<title>%1</title>\n").arg(title);
    html += head;
    html += QStringLiteral("<style>") + noteStyleSheet() + numberingStyleSheet(body) +
            widthStyleSheet(body) + QStringLiteral("</style>\n");
    html +=
        QStringLiteral("</head>\n<body>\n<header class=\"jotter-header\"><h1>%1</h1>").arg(title);
    if (!subtitle.isEmpty()) {
        html += QStringLiteral("<p>%1</p>").arg(subtitle.toHtmlEscaped());
    }
    if (!tags.isEmpty()) {
        html += QStringLiteral("<p>%1</p>").arg(tags);
    }
    html += QStringLiteral("</header>\n");
    html += BodyOpen + u'\n' + body + u'\n' + BodyClose + u'\n';
    html += QStringLiteral("</body>\n</html>\n");
    return html.toUtf8();
}

} // namespace

Note makeNote()
{
    Note note;
    note.meta.id = newId();
    note.meta.created = now();
    note.meta.updated = note.meta.created;
    return note;
}

Result<Note> parseNoteForNormalization(const QByteArray &data, const QString &path)
{
    const QString text = QString::fromUtf8(data);
    const qsizetype metaStart = text.indexOf(MetaOpen);
    const qsizetype metaEnd =
        metaStart < 0 ? -1 : text.indexOf(MetaClose, metaStart + MetaOpen.size());
    if (metaEnd < 0) {
        return fail(ErrorCode::Malformed, path, QStringLiteral("no metadata block"));
    }
    const QString metaText =
        text.mid(metaStart + MetaOpen.size(), metaEnd - metaStart - MetaOpen.size());
    auto object = parseJsonObject(metaText.toUtf8(), path);
    if (!object) {
        return std::unexpected(object.error());
    }
    const int format = object->value(QStringLiteral("format")).toInt(0);
    if (format > NoteFormat) {
        return fail(ErrorCode::NewerFormat, path);
    }
    if (format < 1) {
        return fail(ErrorCode::Malformed, path, QStringLiteral("invalid field \"format\""));
    }

    MetaReader reader(*object);
    Note note;
    NoteMeta &meta = note.meta;
    meta.id = reader.string(QStringLiteral("id"), true);
    meta.title = reader.string(QStringLiteral("title"), false);
    meta.icon = reader.optionalString(QStringLiteral("icon"));
    meta.color = reader.optionalString(QStringLiteral("color"));
    meta.created = reader.time(QStringLiteral("created"));
    meta.updated = reader.time(QStringLiteral("updated"));
    meta.collections = reader.strings(QStringLiteral("collections"));
    meta.tags = reader.strings(QStringLiteral("tags"));
    meta.archived = reader.optionalTime(QStringLiteral("archived"));
    meta.trashed = reader.optionalTime(QStringLiteral("trashed"));
    meta.location = reader.location();
    if (!reader.problem().isEmpty()) {
        return fail(ErrorCode::Malformed, path, reader.problem());
    }
    if (!isValidId(meta.id) || QFileInfo(path).completeBaseName() != meta.id) {
        return fail(ErrorCode::Malformed, path,
                    QStringLiteral("the identifier does not match the file name"));
    }
    meta.extra =
        unknownFields(*object, {QLatin1StringView("format"), QLatin1StringView("id"),
                                QLatin1StringView("title"), QLatin1StringView("icon"),
                                QLatin1StringView("color"), QLatin1StringView("created"),
                                QLatin1StringView("updated"), QLatin1StringView("collections"),
                                QLatin1StringView("tags"), QLatin1StringView("archived"),
                                QLatin1StringView("trashed"), QLatin1StringView("location")});

    const qsizetype bodyStart = text.indexOf(BodyOpen, metaEnd);
    const qsizetype bodyEnd = text.lastIndexOf(BodyClose);
    if (bodyStart < 0 || bodyEnd < bodyStart) {
        return fail(ErrorCode::Malformed, path, QStringLiteral("no body"));
    }
    note.body = text.mid(bodyStart + BodyOpen.size(), bodyEnd - bodyStart - BodyOpen.size());
    // The writer puts the body on lines of its own; the newlines are not part of the content.
    if (note.body.startsWith(u'\n')) {
        note.body.remove(0, 1);
    }
    if (note.body.endsWith(u'\n')) {
        note.body.chop(1);
    }
    return note;
}

Result<Note> parseNote(const QByteArray &data, const QString &path)
{
    auto note = parseNoteForNormalization(data, path);
    if (note) {
        const QString problem = html::bodyProblem(note->body);
        if (!problem.isEmpty()) {
            return fail(ErrorCode::Malformed, path, problem);
        }
    }
    return note;
}

QByteArray serializeNote(const Note &note)
{
    // '<' is escaped inside the JSON so that no title or tag can end the script element early.
    QString metaText = QString::fromUtf8(toJsonText(metaToJson(note.meta)));
    metaText.replace(u'<', QStringLiteral("\\u003c"));
    metaText.chop(1);
    return noteHtml(note.meta, note.body, MetaOpen + u'\n' + metaText + u'\n' + MetaClose + u'\n',
                    {});
}

QByteArray exportNoteDocument(const NoteMeta &meta, QStringView body, const QString &subtitle)
{
    return noteHtml(meta, body, {}, subtitle);
}

} // namespace core
