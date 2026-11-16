#include "core/PlainText.h"

#include "core/Html.h"

namespace core {

QString plainTextFromHtml(QStringView html)
{
    QString text;
    text.reserve(html.size());
    bool pendingSpace = false;
    bool pendingBreak = false;
    for (const html::Token &token : html::tokenize(html)) {
        if (token.kind != html::Token::Kind::Text) {
            if (html::isBlockElement(token.name)) {
                pendingBreak = true;
            }
            continue;
        }
        for (const QChar c : token.text) {
            if (c.isSpace()) {
                pendingSpace = true;
                continue;
            }
            if (!text.isEmpty()) {
                if (pendingBreak) {
                    text += u'\n';
                } else if (pendingSpace) {
                    text += u' ';
                }
            }
            pendingSpace = pendingBreak = false;
            text += c;
        }
    }
    return text;
}

QString snippet(const QString &text, qsizetype length)
{
    QString line = text;
    line.replace(u'\n', u' ');
    if (line.size() <= length) {
        return line;
    }
    qsizetype cut = line.lastIndexOf(u' ', length);
    if (cut < length / 2) {
        cut = length;
    }
    return line.left(cut).trimmed() + QStringLiteral("…");
}

} // namespace core
