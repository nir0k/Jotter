#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>

namespace core {

/// Turns a typed tag into its stored form: without a leading `#`, without
/// empty path parts (`a//b`, `/a`, `a/`) and without spaces around each part.
///
/// @param typed The tag as typed.
/// @return The tag, or an empty string when nothing is left.
QString normalizedTag(QStringView typed);

/// Tells whether @p candidate is @p root or one of its children (`work/meetings` in `work`).
bool isTagWithin(const QString &candidate, const QString &root);

/// Renames a tag inside a renamed parent: `work/meetings` with `work` → `job` becomes
/// `job/meetings`. Returns no value for a tag outside @p from.
std::optional<QString> renamedTag(const QString &tag, const QString &from, const QString &to);

/// Returns the key tags are compared by: case-folded.
QString tagKey(const QString &tag);

/// Returns the colour of a tag: its own or, without one, its nearest parent's.
///
/// @param colors Tag colour keys by tag key, as `library.json` holds them.
/// @param tag The tag.
/// @return The tag colour key, or an empty string for a tag without colour.
QString tagColor(const QMap<QString, QString> &colors, const QString &tag);

/// Joins two tag lists, keeping the first spelling of each tag.
QStringList mergeTags(const QStringList &first, const QStringList &second);

} // namespace core
