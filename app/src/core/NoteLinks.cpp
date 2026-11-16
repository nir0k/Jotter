#include "core/NoteLinks.h"

#include "core/Files.h"
#include "core/Html.h"

#include <QUrl>

namespace core {

QString noteLinkAddress(const QString &noteId)
{
    return noteId + QStringLiteral(".html");
}

std::optional<QString> noteLinkTarget(QStringView address)
{
    if (!address.endsWith(u".html")) {
        return std::nullopt;
    }
    const QString id = address.chopped(5).toString();
    return isValidId(id) ? std::optional<QString>(id) : std::nullopt;
}

namespace {

/// Tells whether a parsed address is a valid absolute URL with a scheme notes accept.
bool isWebUrl(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    return url.isValid() &&
           (scheme == QLatin1StringView("http") || scheme == QLatin1StringView("https") ||
            scheme == QLatin1StringView("mailto"));
}

} // namespace

bool isWebLink(const QString &address)
{
    return isWebUrl(QUrl(address, QUrl::StrictMode));
}

QString normaliseLink(const QString &address)
{
    QString text = address.trimmed();
    if (text.isEmpty()) {
        return {};
    }
    if (!text.contains(QLatin1StringView("://")) &&
        !text.startsWith(QLatin1StringView("mailto:"))) {
        text = (text.contains(u'@') && !text.contains(u'/') ? QStringLiteral("mailto:")
                                                            : QStringLiteral("https://")) +
               text;
    }
    const QUrl url(text, QUrl::StrictMode);
    return isWebUrl(url) ? url.toString(QUrl::FullyEncoded) : QString();
}

std::optional<QString> renameNoteLinks(const QString &body, const QString &noteId,
                                       const QString &oldTitle, const QString &newTitle)
{
    if (oldTitle.isEmpty() || oldTitle == newTitle) {
        return std::nullopt;
    }
    // The codec writes a link without other formatting as the element and its escaped text, so
    // a link that still shows the old title has exactly this form.
    const QString open = QStringLiteral("<a class=\"note-link\" href=\"%1\">")
                             .arg(html::escapeAttribute(noteLinkAddress(noteId)));
    const QString before = open + html::escapeText(oldTitle) + QStringLiteral("</a>");
    if (!body.contains(before)) {
        return std::nullopt;
    }
    QString renamed = body;
    renamed.replace(before, open + html::escapeText(newTitle) + QStringLiteral("</a>"));
    return renamed;
}

} // namespace core
