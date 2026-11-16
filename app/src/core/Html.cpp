#include "core/Html.h"

#include "core/Attachments.h"
#include "core/NoteLinks.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace core::html {

namespace {

/// Decodes one character reference without its `&` and `;` (`amp`, `#39`, `#x2014`); returns
/// no value for an unknown name, which is then kept as written.
std::optional<QString> decodeReference(QStringView reference)
{
    static const QHash<QString, QString> named{
        {QStringLiteral("amp"), QStringLiteral("&")},
        {QStringLiteral("lt"), QStringLiteral("<")},
        {QStringLiteral("gt"), QStringLiteral(">")},
        {QStringLiteral("quot"), QStringLiteral("\"")},
        {QStringLiteral("apos"), QStringLiteral("'")},
        {QStringLiteral("nbsp"), QString(QChar(0x00A0))},
    };
    if (reference.startsWith(u'#')) {
        bool ok = false;
        const bool hex = reference.size() > 1 && (reference[1] == u'x' || reference[1] == u'X');
        const uint code = reference.mid(hex ? 2 : 1).toUInt(&ok, hex ? 16 : 10);
        if (ok && code > 0 && code <= 0x10FFFF) {
            const char32_t character = code;
            return QString::fromUcs4(&character, 1);
        }
        return std::nullopt;
    }
    const auto found = named.constFind(reference.toString());
    return found == named.constEnd() ? std::nullopt : std::optional<QString>(*found);
}

/// Decodes the character references of a text run or an attribute value.
QString decodeReferences(QStringView text)
{
    if (!text.contains(u'&')) {
        return text.toString();
    }
    QString decoded;
    decoded.reserve(text.size());
    qsizetype i = 0;
    while (i < text.size()) {
        if (text[i] == u'&') {
            const qsizetype end = text.indexOf(u';', i);
            if (end > i + 1 && end - i <= 10) {
                if (const auto character = decodeReference(text.mid(i + 1, end - i - 1))) {
                    decoded += *character;
                    i = end + 1;
                    continue;
                }
            }
        }
        decoded += text[i];
        ++i;
    }
    return decoded;
}

/// Parses the attributes of a start tag (the text after its name).
QList<std::pair<QString, QString>> parseAttributes(QStringView source)
{
    QList<std::pair<QString, QString>> attributes;
    qsizetype i = 0;
    const auto skipSpace = [&] {
        while (i < source.size() && source[i].isSpace()) {
            ++i;
        }
    };
    while (true) {
        skipSpace();
        if (i >= source.size() || source[i] == u'/') {
            break;
        }
        const qsizetype nameStart = i;
        while (i < source.size() && !source[i].isSpace() && source[i] != u'=' &&
               source[i] != u'/') {
            ++i;
        }
        const QString name = source.mid(nameStart, i - nameStart).toString().toLower();
        skipSpace();
        QString value;
        if (i < source.size() && source[i] == u'=') {
            ++i;
            skipSpace();
            if (i < source.size() && (source[i] == u'"' || source[i] == u'\'')) {
                const QChar quote = source[i];
                const qsizetype end = source.indexOf(quote, i + 1);
                const qsizetype stop = end < 0 ? source.size() : end;
                value = decodeReferences(source.mid(i + 1, stop - i - 1));
                i = stop + 1;
            } else {
                const qsizetype valueStart = i;
                while (i < source.size() && !source[i].isSpace()) {
                    ++i;
                }
                value = decodeReferences(source.mid(valueStart, i - valueStart));
            }
        }
        if (!name.isEmpty()) {
            attributes.append({name, value});
        }
    }
    return attributes;
}

} // namespace

QString Token::attribute(QStringView attributeName) const
{
    for (const auto &[key, value] : attributes) {
        if (key == attributeName) {
            return value;
        }
    }
    return {};
}

bool Token::hasAttribute(QStringView attributeName) const
{
    return std::ranges::any_of(attributes,
                               [&](const auto &pair) { return pair.first == attributeName; });
}

QList<Token> tokenize(QStringView html)
{
    QList<Token> tokens;
    qsizetype i = 0;
    const auto appendText = [&](QStringView text) {
        if (text.isEmpty()) {
            return;
        }
        if (!tokens.isEmpty() && tokens.last().kind == Token::Kind::Text) {
            tokens.last().text += decodeReferences(text);
        } else {
            tokens.append({.kind = Token::Kind::Text,
                           .name = {},
                           .attributes = {},
                           .text = decodeReferences(text)});
        }
    };
    while (i < html.size()) {
        const qsizetype open = html.indexOf(u'<', i);
        if (open < 0) {
            appendText(html.mid(i));
            break;
        }
        appendText(html.mid(i, open - i));
        if (html.mid(open).startsWith(QLatin1StringView("<!--"))) {
            const qsizetype end = html.indexOf(QLatin1StringView("-->"), open + 4);
            i = end < 0 ? html.size() : end + 3;
            continue;
        }
        qsizetype close = open + 1;
        QChar quote;
        for (; close < html.size(); ++close) {
            const QChar character = html[close];
            if (!quote.isNull()) {
                if (character == quote) {
                    quote = QChar();
                }
            } else if (character == u'"' || character == u'\'') {
                quote = character;
            } else if (character == u'>') {
                break;
            }
        }
        if (close == html.size()) {
            close = -1;
        }
        const bool isTag = close > open + 1 && (html[open + 1].isLetter() ||
                                                html[open + 1] == u'/' || html[open + 1] == u'!');
        if (!isTag) {
            appendText(html.mid(open, 1));
            i = open + 1;
            continue;
        }
        const QStringView inside = html.mid(open + 1, close - open - 1);
        i = close + 1;
        if (inside.startsWith(u'!')) {
            continue; // A declaration such as <!doctype html>.
        }
        if (inside.startsWith(u'/')) {
            tokens.append({.kind = Token::Kind::End,
                           .name = inside.mid(1).trimmed().toString().toLower(),
                           .attributes = {},
                           .text = {}});
            continue;
        }
        qsizetype nameEnd = 0;
        while (nameEnd < inside.size() && (inside[nameEnd].isLetterOrNumber())) {
            ++nameEnd;
        }
        tokens.append({.kind = Token::Kind::Start,
                       .name = inside.left(nameEnd).toString().toLower(),
                       .attributes = parseAttributes(inside.mid(nameEnd)),
                       .text = {}});
    }
    return tokens;
}

QString bodyProblem(QStringView body)
{
    static const QHash<QString, QStringList> attributes{
        {QStringLiteral("a"),
         {QStringLiteral("href"), QStringLiteral("class"), QStringLiteral("data-size")}},
        {QStringLiteral("span"), {QStringLiteral("data-color"), QStringLiteral("style")}},
        {QStringLiteral("mark"), {QStringLiteral("data-color"), QStringLiteral("style")}},
        {QStringLiteral("img"),
         {QStringLiteral("src"), QStringLiteral("data-width"), QStringLiteral("data-wrap")}},
        {QStringLiteral("pre"), {QStringLiteral("data-language"), QStringLiteral("data-title")}},
        {QStringLiteral("ul"), {QStringLiteral("class")}},
        {QStringLiteral("ol"), {QStringLiteral("data-style"), QStringLiteral("start")}},
        {QStringLiteral("li"), {QStringLiteral("data-checked")}},
        {QStringLiteral("table"), {QStringLiteral("data-width")}},
        {QStringLiteral("col"), {QStringLiteral("style")}}};
    static const QStringList elements =
        QStringLiteral("a blockquote br code col colgroup em h1 h2 h3 hr img li mark ol p pre s "
                       "span strong sub sup table tbody td th thead tr u ul")
            .split(u' ');
    static const QStringList voids{QStringLiteral("br"), QStringLiteral("hr"),
                                   QStringLiteral("img"), QStringLiteral("col")};
    if (body.contains(QLatin1StringView("<!"))) {
        return QStringLiteral("unsupported body: comment or declaration");
    }
    QStringList stack;
    int lists = 0;
    for (const Token &token : tokenize(body)) {
        if (token.kind == Token::Kind::Text) {
            continue;
        }
        if (!elements.contains(token.name)) {
            return QStringLiteral("unsupported body: element %1").arg(token.name);
        }
        if (token.kind == Token::Kind::End) {
            if (voids.contains(token.name) || stack.isEmpty() || stack.last() != token.name) {
                return QStringLiteral("unsupported body: mismatched closing element %1")
                    .arg(token.name);
            }
            stack.removeLast();
            if (token.name == QLatin1StringView("ul") || token.name == QLatin1StringView("ol")) {
                --lists;
            }
            continue;
        }
        const QString parent = stack.isEmpty() ? QString() : stack.last();
        const auto oneOf = [](const QString &name, QStringView names) {
            while (!names.isEmpty()) {
                const qsizetype space = names.indexOf(u' ');
                if (space < 0) {
                    return names == name;
                }
                if (names.first(space) == name) {
                    return true;
                }
                names = names.sliced(space + 1);
            }
            return false;
        };
        bool structure = true;
        if (oneOf(token.name, u"thead tbody colgroup")) {
            structure = parent == QLatin1StringView("table");
        } else if (token.name == QLatin1StringView("tr")) {
            structure = oneOf(parent, u"table thead tbody");
        } else if (oneOf(token.name, u"td th")) {
            structure = parent == QLatin1StringView("tr");
        } else if (token.name == QLatin1StringView("col")) {
            structure = parent == QLatin1StringView("colgroup");
        } else if (token.name == QLatin1StringView("li")) {
            structure = oneOf(parent, u"ul ol");
        } else if (oneOf(token.name, u"p h1 h2 h3 blockquote ul ol pre table hr")) {
            structure = parent.isEmpty() || oneOf(parent, u"blockquote li td th");
            if (oneOf(token.name, u"pre table") && oneOf(parent, u"td th")) {
                structure = false;
            }
        } else {
            structure =
                oneOf(parent, u"p h1 h2 h3 li td th strong em u s sup sub span mark a code") ||
                (token.name == QLatin1StringView("code") && parent == QLatin1StringView("pre"));
        }
        if (stack.contains(QStringLiteral("pre")) &&
            !(token.name == QLatin1StringView("code") && parent == QLatin1StringView("pre"))) {
            structure = false;
        }
        if (!structure) {
            return QStringLiteral("unsupported body: %1 inside %2").arg(token.name, parent);
        }
        QSet<QString> seen;
        for (const auto &[name, value] : token.attributes) {
            if (seen.contains(name) || !attributes.value(token.name).contains(name)) {
                return QStringLiteral("unsupported body: attribute %1 on %2").arg(name, token.name);
            }
            seen.insert(name);
            if (name == QLatin1StringView("style")) {
                const bool known = (token.name == QLatin1StringView("col") &&
                                    value.startsWith(QLatin1StringView("width:"))) ||
                                   (token.name == QLatin1StringView("span") &&
                                    value.startsWith(QLatin1StringView("font-family:"))) ||
                                   (token.hasAttribute(u"data-color") &&
                                    (value.startsWith(QLatin1StringView("color:")) ||
                                     value.startsWith(QLatin1StringView("background:"))));
                if (!known || value.contains(u';') || value.contains(u'(') ||
                    value.contains(u'\\')) {
                    return QStringLiteral("unsupported body: style");
                }
            }
        }
        if (token.hasAttribute(u"data-width")) {
            const int width = token.attribute(u"data-width").toInt();
            if (width < 10 || width > 99) {
                return QStringLiteral("unsupported body: width");
            }
        }
        if (token.hasAttribute(u"data-checked") &&
            token.attribute(u"data-checked") != QLatin1StringView("true") &&
            token.attribute(u"data-checked") != QLatin1StringView("false")) {
            return QStringLiteral("unsupported body: checklist state");
        }
        if (token.hasAttribute(u"class")) {
            const QString value = token.attribute(u"class");
            const bool known = (token.name == QLatin1StringView("ul") &&
                                value == QLatin1StringView("checklist")) ||
                               (token.name == QLatin1StringView("a") &&
                                (value == QLatin1StringView("attachment") ||
                                 value == QLatin1StringView("note-link")));
            if (!known) {
                return QStringLiteral("unsupported body: class");
            }
        }
        if (token.name == QLatin1StringView("img") &&
            (!parseAttachmentSource(token.attribute(u"src")) ||
             (token.hasAttribute(u"data-wrap") &&
              token.attribute(u"data-wrap") != QLatin1StringView("square") &&
              token.attribute(u"data-wrap") != QLatin1StringView("top-bottom")))) {
            return QStringLiteral("unsupported body: image source or wrap mode");
        }
        if (token.name == QLatin1StringView("a")) {
            const QString address = token.attribute(u"href");
            const bool attachment = token.attribute(u"class") == QLatin1StringView("attachment");
            if (attachment ? !parseAttachmentSource(address)
                           : !noteLinkTarget(address) && normaliseLink(address).isEmpty()) {
                return QStringLiteral("unsupported body: link address");
            }
        }
        if (token.name == QLatin1StringView("ul") || token.name == QLatin1StringView("ol")) {
            if (++lists > 9) {
                return QStringLiteral("unsupported body: list nesting exceeds nine levels");
            }
        }
        if (!voids.contains(token.name)) {
            stack.append(token.name);
        }
    }
    return stack.isEmpty() ? QString() : QStringLiteral("unsupported body: unclosed element");
}

bool isBlockElement(QStringView name)
{
    static const QStringList blocks{
        QStringLiteral("p"),   QStringLiteral("h1"),  QStringLiteral("h2"),
        QStringLiteral("h3"),  QStringLiteral("li"),  QStringLiteral("blockquote"),
        QStringLiteral("pre"), QStringLiteral("tr"),  QStringLiteral("td"),
        QStringLiteral("th"),  QStringLiteral("br"),  QStringLiteral("hr"),
        QStringLiteral("ul"),  QStringLiteral("ol"),  QStringLiteral("table"),
        QStringLiteral("div"), QStringLiteral("img"),
    };
    return blocks.contains(name);
}

QString escapeText(QStringView text)
{
    QString escaped;
    escaped.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case u'&':
            escaped += QStringLiteral("&amp;");
            break;
        case u'<':
            escaped += QStringLiteral("&lt;");
            break;
        case u'>':
            escaped += QStringLiteral("&gt;");
            break;
        default:
            escaped += c;
        }
    }
    return escaped;
}

QString escapeAttribute(QStringView text)
{
    return escapeText(text).replace(u'"', QStringLiteral("&quot;"));
}

} // namespace core::html
