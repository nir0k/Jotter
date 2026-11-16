#include "core/Tags.h"

#include <QSet>

#include <algorithm>

namespace core {

QString normalizedTag(QStringView typed)
{
    typed = typed.trimmed();
    while (typed.startsWith(u'#')) {
        typed = typed.mid(1);
    }
    QStringList parts;
    for (const QStringView part : typed.split(u'/', Qt::SkipEmptyParts)) {
        const QStringView trimmed = part.trimmed();
        if (!trimmed.isEmpty()) {
            parts.append(trimmed.toString());
        }
    }
    return parts.join(u'/');
}

bool isTagWithin(const QString &candidate, const QString &root)
{
    const QString key = tagKey(candidate);
    const QString rootKey = tagKey(root);
    return key == rootKey || key.startsWith(rootKey + u'/');
}

std::optional<QString> renamedTag(const QString &tag, const QString &from, const QString &to)
{
    if (!isTagWithin(tag, from)) {
        return std::nullopt;
    }
    return to + tag.mid(from.size());
}

QString tagKey(const QString &tag)
{
    return tag.toCaseFolded();
}

QString tagColor(const QMap<QString, QString> &colors, const QString &tag)
{
    QString key = tagKey(tag);
    while (!key.isEmpty()) {
        if (const auto found = colors.constFind(key); found != colors.cend()) {
            return *found;
        }
        key.truncate(std::max<qsizetype>(key.lastIndexOf(u'/'), 0));
    }
    return {};
}

QStringList mergeTags(const QStringList &first, const QStringList &second)
{
    QStringList merged;
    QSet<QString> seen;
    for (const QStringList *list : {&first, &second}) {
        for (const QString &tag : *list) {
            if (!seen.contains(tagKey(tag))) {
                seen.insert(tagKey(tag));
                merged.append(tag);
            }
        }
    }
    return merged;
}

} // namespace core
