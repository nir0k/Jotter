#include "core/Export.h"

#include "core/Attachments.h"
#include "core/Directory.h"
#include "core/Html.h"
#include "core/NoteLinks.h"

#include <QFile>
#include <QUrl>

#include <algorithm>
#include <array>

namespace core {

namespace {

/// The longest base name of an exported file, in UTF-16 code units, well within the limits of
/// file systems and archives once an extension and a number are added.
constexpr qsizetype MaxBaseLength = 100;

/// Tells whether a character is refused in file names on some system an export may reach.
bool isRefused(QChar c)
{
    return c.unicode() < 0x20 || c.unicode() == 0x7F || QStringView(u"/\\:*?\"<>|").contains(c);
}

/// Returns a name as one segment of a relative URL.
QString urlSegment(const QString &name)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(name));
}

/// Returns an image file as a `data:` URL, or an empty string when it is missing, too large or
/// not an accepted image.
QString imageData(const QString &path)
{
    auto file = openFile(path, QIODevice::ReadOnly);
    if (!file || (*file)->size() > MaxFileBytes) {
        return {};
    }
    const QByteArray bytes = (*file)->readAll();
    const QString mime = imageMimeType(imageKind(bytes));
    if (mime.isEmpty()) {
        return {};
    }
    return QStringLiteral("data:%1;base64,%2").arg(mime, QString::fromLatin1(bytes.toBase64()));
}

/// Replaces the value of an attribute of a start tag.
void setAttribute(html::Token &token, QStringView name, const QString &value)
{
    for (auto &attribute : token.attributes) {
        if (attribute.first == name) {
            attribute.second = value;
        }
    }
}

/// The elements of the note vocabulary; an export writes no others.
constexpr std::array VocabularyElements{
    QLatin1StringView("a"),    QLatin1StringView("blockquote"), QLatin1StringView("br"),
    QLatin1StringView("code"), QLatin1StringView("col"),        QLatin1StringView("colgroup"),
    QLatin1StringView("em"),   QLatin1StringView("h1"),         QLatin1StringView("h2"),
    QLatin1StringView("h3"),   QLatin1StringView("hr"),         QLatin1StringView("img"),
    QLatin1StringView("li"),   QLatin1StringView("mark"),       QLatin1StringView("ol"),
    QLatin1StringView("p"),    QLatin1StringView("pre"),        QLatin1StringView("s"),
    QLatin1StringView("span"), QLatin1StringView("strong"),     QLatin1StringView("sub"),
    QLatin1StringView("sup"),  QLatin1StringView("table"),      QLatin1StringView("tbody"),
    QLatin1StringView("td"),   QLatin1StringView("th"),         QLatin1StringView("thead"),
    QLatin1StringView("tr"),   QLatin1StringView("u"),          QLatin1StringView("ul")};

/// Tells whether an element of the note vocabulary keeps an attribute in an export: the
/// vocabulary's own, and a style only without functions or escapes, so it cannot load anything.
/// Event handlers and other attributes of a file changed outside Jotter are left out.
bool keepsAttribute(const std::pair<QString, QString> &attribute)
{
    const auto &[name, value] = attribute;
    if (name == QLatin1StringView("style")) {
        return !value.contains(u'(') && !value.contains(u'\\');
    }
    return name.startsWith(QLatin1StringView("data-")) || name == QLatin1StringView("href") ||
           name == QLatin1StringView("src") || name == QLatin1StringView("class") ||
           name == QLatin1StringView("start");
}

/// Writes a start tag with its attributes.
QString startTag(const html::Token &token)
{
    QString tag = u'<' + token.name;
    for (const auto &[name, value] : token.attributes) {
        tag += u' ' + name + QStringLiteral("=\"") + html::escapeAttribute(value) + u'"';
    }
    return tag + u'>';
}

} // namespace

QString exportBaseName(const QString &title, const QString &untitled)
{
    QString name = title;
    for (QChar &c : name) {
        if (isRefused(c)) {
            c = u'_';
        }
    }
    const auto trimmed = [](QChar c) { return c.isSpace() || c == u'.'; };
    while (!name.isEmpty() && trimmed(name.front())) {
        name.removeFirst();
    }
    if (name.size() > MaxBaseLength) {
        // A surrogate pair is kept whole.
        name.truncate(name.at(MaxBaseLength - 1).isHighSurrogate() ? MaxBaseLength - 1
                                                                   : MaxBaseLength);
    }
    while (!name.isEmpty() && trimmed(name.back())) {
        name.removeLast();
    }
    return name.isEmpty() ? untitled : name;
}

QString ExportNames::take(const QString &base)
{
    QString name = base;
    for (int number = 2; m_taken.contains(name.toCaseFolded()); ++number) {
        name = QStringLiteral("%1 (%2)").arg(base).arg(number);
    }
    m_taken.insert(name.toCaseFolded());
    return name;
}

QList<ExportFile> exportHtml(const Library &library, const Note &note, const QString &base,
                             const QHash<QString, QString> &exported, const QString &subtitle)
{
    const QString folder = base + QStringLiteral("_files");
    QList<ExportFile> files{{.path = base + QStringLiteral(".html"), .data = {}, .source = {}}};
    QSet<QString> copied;
    QString body;
    // The end tag of a link that became plain text is left out with its start tag.
    bool dropLinkEnd = false;
    // A script or style sheet in a file changed outside Jotter is left out with its contents.
    QString skipUntil;
    for (html::Token token : html::tokenize(note.body)) {
        if (!skipUntil.isEmpty()) {
            if (token.kind == html::Token::Kind::End && token.name == skipUntil) {
                skipUntil.clear();
            }
            continue;
        }
        if (token.kind == html::Token::Kind::Text) {
            body += html::escapeText(token.text);
            continue;
        }
        if (token.kind == html::Token::Kind::Start && (token.name == QLatin1StringView("script") ||
                                                       token.name == QLatin1StringView("style"))) {
            skipUntil = token.name;
            continue;
        }
        // Other elements outside the vocabulary keep only their text, as the editor reads them.
        if (std::ranges::find(VocabularyElements, token.name) == VocabularyElements.end()) {
            continue;
        }
        const bool link = token.name == QLatin1StringView("a");
        if (token.kind == html::Token::Kind::End) {
            if (link && dropLinkEnd) {
                dropLinkEnd = false;
            } else {
                body += QStringLiteral("</%1>").arg(token.name);
            }
            continue;
        }
        token.attributes.removeIf([](const auto &attribute) { return !keepsAttribute(attribute); });
        if (token.name == QLatin1StringView("img")) {
            // Only the note's own images are embedded; an export loads nothing from elsewhere.
            const auto ref = parseAttachmentSource(token.attribute(u"src"));
            if (!ref) {
                continue;
            }
            const QString data =
                imageData(library.attachmentsDir(ref->noteId) + u'/' + ref->fileName);
            if (data.isEmpty()) {
                // A missing image keeps its name, as the editor shows it.
                body += html::escapeText(ref->fileName);
                continue;
            }
            setAttribute(token, u"src", data);
        } else if (link) {
            const QString href = token.attribute(u"href");
            if (const auto target = noteLinkTarget(href)) {
                const auto file = exported.constFind(*target);
                if (file == exported.cend()) {
                    dropLinkEnd = true;
                    continue;
                }
                setAttribute(token, u"href", urlSegment(*file + QStringLiteral(".html")));
            } else if (const auto ref = parseAttachmentSource(href)) {
                const QString path = folder + u'/' + ref->fileName;
                if (!copied.contains(path)) {
                    copied.insert(path);
                    files.append(
                        {.path = path,
                         .data = {},
                         .source = library.attachmentsDir(ref->noteId) + u'/' + ref->fileName});
                }
                setAttribute(token, u"href", urlSegment(folder) + u'/' + urlSegment(ref->fileName));
            } else if (token.hasAttribute(u"href") && !isWebLink(href)) {
                dropLinkEnd = true;
                continue;
            }
        }
        body += startTag(token);
    }
    files.first().data = exportNoteDocument(note.meta, body, subtitle);
    return files;
}

} // namespace core
