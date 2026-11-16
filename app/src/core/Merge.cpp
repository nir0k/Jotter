#include "core/Merge.h"

#include "core/Tags.h"

#include <QHash>
#include <QSet>

#include <algorithm>

namespace core {

namespace {

/// The changed middle parts of two block lists are matched only up to this many table cells
/// (about 8 MB); larger rewrites are treated as having nothing in common, which makes a change
/// on both sides a conflict rather than a slow merge.
constexpr qsizetype MaxMatchCells = 1'000'000;

/// Elements that have no end tag.
bool isVoidElement(QStringView name)
{
    static const QSet<QString> names{
        QStringLiteral("area"), QStringLiteral("base"),   QStringLiteral("br"),
        QStringLiteral("col"),  QStringLiteral("embed"),  QStringLiteral("hr"),
        QStringLiteral("img"),  QStringLiteral("input"),  QStringLiteral("link"),
        QStringLiteral("meta"), QStringLiteral("source"), QStringLiteral("track"),
        QStringLiteral("wbr")};
    return names.contains(name.toString());
}

/// Pairs equal blocks of two lists along their longest common subsequence.
///
/// @param a The first list.
/// @param b The second list.
/// @return For each block of @p a, the index of its partner in @p b, or -1; partners increase
///         with the index.
QList<qsizetype> matchBlocks(const QStringList &a, const QStringList &b)
{
    QList<qsizetype> match(a.size(), -1);
    // Most edits touch a few blocks; the common start and end are matched without the table.
    qsizetype head = 0;
    while (head < a.size() && head < b.size() && a.at(head) == b.at(head)) {
        match[head] = head;
        ++head;
    }
    qsizetype tail = 0;
    while (tail < a.size() - head && tail < b.size() - head &&
           a.at(a.size() - 1 - tail) == b.at(b.size() - 1 - tail)) {
        match[a.size() - 1 - tail] = b.size() - 1 - tail;
        ++tail;
    }
    const qsizetype n = a.size() - head - tail;
    const qsizetype m = b.size() - head - tail;
    if (n == 0 || m == 0 || n * m > MaxMatchCells) {
        return match;
    }
    // lengths(x, y) is the length of the common subsequence of the middle parts from x and y.
    QList<qsizetype> lengths((n + 1) * (m + 1), 0);
    const auto length = [&](qsizetype x, qsizetype y) -> qsizetype & {
        return lengths[x * (m + 1) + y];
    };
    for (qsizetype x = n - 1; x >= 0; --x) {
        for (qsizetype y = m - 1; y >= 0; --y) {
            length(x, y) = a.at(head + x) == b.at(head + y)
                               ? length(x + 1, y + 1) + 1
                               : std::max(length(x + 1, y), length(x, y + 1));
        }
    }
    for (qsizetype x = 0, y = 0; x < n && y < m;) {
        if (a.at(head + x) == b.at(head + y)) {
            match[head + x] = head + y;
            ++x;
            ++y;
        } else if (length(x + 1, y) >= length(x, y + 1)) {
            ++x;
        } else {
            ++y;
        }
    }
    return match;
}

/// A change of one side against the base: the base blocks from start to end (an empty range
/// for an insertion) replaced by blocks.
struct Hunk
{
    qsizetype start = 0;
    qsizetype end = 0;
    QStringList blocks;

    bool operator==(const Hunk &) const = default;
};

/// Lists the changes of one side against the base, in base order.
///
/// @param base The base blocks.
/// @param side The blocks of the side.
/// @param match The partners of the base blocks in @p side, from matchBlocks().
/// @return The hunks; their base ranges do not overlap.
QList<Hunk> hunks(const QStringList &base, const QStringList &side, const QList<qsizetype> &match)
{
    QList<Hunk> result;
    qsizetype b = 0;
    qsizetype s = 0;
    for (qsizetype i = 0; i <= base.size(); ++i) {
        if (i < base.size() && match.at(i) < 0) {
            continue;
        }
        const qsizetype sideEnd = i < base.size() ? match.at(i) : side.size();
        if (b < i || s < sideEnd) {
            result.append({.start = b, .end = i, .blocks = side.mid(s, sideEnd - s)});
        }
        b = i + 1;
        s = sideEnd + 1;
    }
    return result;
}

/// Tells whether two changes touch the same part of the base: overlapping ranges, an insertion
/// inside the other's range, or two insertions at one place. A change next to another is not
/// a conflict.
bool overlaps(const Hunk &a, const Hunk &b)
{
    if (a.start == a.end && b.start == b.end) {
        return a.start == b.start;
    }
    if (a.start == a.end) {
        return b.start < a.start && a.start < b.end;
    }
    if (b.start == b.end) {
        return a.start < b.start && b.start < a.end;
    }
    return a.start < b.end && b.start < a.end;
}

/// Merges block lists three ways: the changes of both sides against the base are applied
/// together, and the same change made on both sides is applied once.
///
/// @return The merged blocks, or no value when both sides changed the same part differently.
std::optional<QStringList> mergeBlocks(const QStringList &base, const QStringList &local,
                                       const QStringList &remote)
{
    const QList<Hunk> localHunks = hunks(base, local, matchBlocks(base, local));
    const QList<Hunk> remoteHunks = hunks(base, remote, matchBlocks(base, remote));
    QStringList merged;
    qsizetype position = 0;
    const auto apply = [&](const Hunk &hunk) {
        merged.append(base.mid(position, hunk.start - position));
        merged.append(hunk.blocks);
        position = hunk.end;
    };
    qsizetype l = 0;
    qsizetype r = 0;
    while (l < localHunks.size() || r < remoteHunks.size()) {
        if (l == localHunks.size()) {
            apply(remoteHunks.at(r++));
            continue;
        }
        if (r == remoteHunks.size()) {
            apply(localHunks.at(l++));
            continue;
        }
        const Hunk &a = localHunks.at(l);
        const Hunk &b = remoteHunks.at(r);
        if (a == b) {
            apply(a);
            ++l;
            ++r;
        } else if (overlaps(a, b)) {
            return std::nullopt;
        } else if (std::pair(a.start, a.start != a.end) < std::pair(b.start, b.start != b.end)) {
            // At one place an insertion comes before a change of the blocks that follow it.
            apply(localHunks.at(l++));
        } else {
            apply(remoteHunks.at(r++));
        }
    }
    merged.append(base.mid(position));
    return merged;
}

/// Chooses between two versions of a value: the changed one when only one side changed it,
/// otherwise the local one only when @p localWins.
template <typename T> T pick(const T &base, const T &local, const T &remote, bool localWins)
{
    if (local == remote || remote == base) {
        return local;
    }
    if (local == base) {
        return remote;
    }
    return localWins ? local : remote;
}

/// Merges two lists as sets against their base: an element both sides have, or that the side
/// having it added, is kept. Elements keep the remote order, followed by those only the local
/// side has.
///
/// @param keyOf Returns the identity of an element, such as the case-folded tag.
template <typename KeyOf>
QStringList mergeSet(const QStringList &base, const QStringList &local, const QStringList &remote,
                     KeyOf keyOf)
{
    const auto keys = [&](const QStringList &list) {
        QSet<QString> result;
        for (const QString &element : list) {
            result.insert(keyOf(element));
        }
        return result;
    };
    const QSet<QString> baseKeys = keys(base);
    const QSet<QString> localKeys = keys(local);
    const QSet<QString> remoteKeys = keys(remote);
    QStringList merged;
    QSet<QString> added;
    for (const QString &element : remote) {
        const QString key = keyOf(element);
        if ((localKeys.contains(key) || !baseKeys.contains(key)) && !added.contains(key)) {
            merged.append(element);
            added.insert(key);
        }
    }
    for (const QString &element : local) {
        const QString key = keyOf(element);
        if (!remoteKeys.contains(key) && !baseKeys.contains(key) && !added.contains(key)) {
            merged.append(element);
            added.insert(key);
        }
    }
    return merged;
}

/// Merges key-value maps (QMap or QJsonObject) against their base key by key; a key both sides
/// changed keeps the remote value.
template <typename Map> Map mergeKeys(const Map &base, const Map &local, const Map &remote)
{
    Map merged = remote;
    const auto remoteChanged = [&](const QString &key) {
        const bool inBase = base.contains(key);
        const bool inRemote = remote.contains(key);
        return inBase != inRemote || (inRemote && remote.value(key) != base.value(key));
    };
    for (const QString &key : local.keys()) {
        if (!remoteChanged(key)) {
            merged.insert(key, local.value(key));
        }
    }
    for (const QString &key : remote.keys()) {
        if (!local.contains(key) && !remoteChanged(key)) {
            merged.remove(key);
        }
    }
    return merged;
}

/// Merges lists of identified entries, such as collections or rules, against their base.
///
/// An entry changed on one side takes that side, also when that side deleted it; an entry
/// changed on one side and deleted on the other is kept, so no change is lost; an entry
/// changed on both sides keeps the local one only when @p localWins says so. The order is the
/// remote one unless only the local side reordered the entries all three versions have;
/// entries missing from that order follow their predecessor on their own side.
///
/// @param keyOf Returns an entry's identifier.
/// @param localWins Tells whether the local entry wins over a remote one both sides changed.
template <typename T, typename KeyOf, typename LocalWins>
QList<T> mergeEntries(const QList<T> &base, const QList<T> &local, const QList<T> &remote,
                      KeyOf keyOf, LocalWins localWins)
{
    const auto byKey = [&](const QList<T> &list) {
        QHash<QString, const T *> result;
        for (const T &entry : list) {
            result.insert(keyOf(entry), &entry);
        }
        return result;
    };
    const QHash<QString, const T *> baseEntries = byKey(base);
    const QHash<QString, const T *> localEntries = byKey(local);
    const QHash<QString, const T *> remoteEntries = byKey(remote);
    const auto same = [](const T *a, const T *b) { return a == b || (a && b && *a == *b); };
    const auto kept = [&](const QString &key) -> const T * {
        const T *baseEntry = baseEntries.value(key);
        const T *localEntry = localEntries.value(key);
        const T *remoteEntry = remoteEntries.value(key);
        if (same(localEntry, remoteEntry) || same(remoteEntry, baseEntry)) {
            return localEntry;
        }
        if (same(localEntry, baseEntry)) {
            return remoteEntry;
        }
        if (!localEntry || !remoteEntry) {
            return localEntry ? localEntry : remoteEntry;
        }
        return localWins(*localEntry, *remoteEntry) ? localEntry : remoteEntry;
    };
    const auto commonOrder = [&](const QList<T> &list) {
        QStringList keys;
        for (const T &entry : list) {
            const QString key = keyOf(entry);
            if (baseEntries.contains(key) && localEntries.contains(key) &&
                remoteEntries.contains(key)) {
                keys.append(key);
            }
        }
        return keys;
    };
    const QStringList baseOrder = commonOrder(base);
    const bool localOrder = commonOrder(remote) == baseOrder && commonOrder(local) != baseOrder;
    const QList<T> &first = localOrder ? local : remote;
    const QList<T> &second = localOrder ? remote : local;
    QList<T> merged;
    QHash<QString, qsizetype> placed;
    for (const T &entry : first) {
        if (const T *chosen = kept(keyOf(entry))) {
            placed.insert(keyOf(entry), merged.size());
            merged.append(*chosen);
        }
    }
    qsizetype insertAt = 0;
    for (const T &entry : second) {
        const QString key = keyOf(entry);
        if (const auto at = placed.constFind(key); at != placed.constEnd()) {
            insertAt = *at + 1;
            continue;
        }
        if (const T *chosen = kept(key)) {
            merged.insert(insertAt, *chosen);
            for (qsizetype &position : placed) {
                if (position >= insertAt) {
                    ++position;
                }
            }
            placed.insert(key, insertAt);
            ++insertAt;
        }
    }
    return merged;
}

} // namespace

QStringList bodyBlocks(QStringView body)
{
    QStringList blocks;
    const qsizetype size = body.size();
    // The start of the block being read, or -1 between blocks.
    qsizetype start = -1;
    // The nesting depth of elements inside the block; 0 while reading text outside elements.
    int depth = 0;
    const auto close = [&](qsizetype end) {
        if (start >= 0) {
            if (const QStringView block = body.sliced(start, end - start).trimmed();
                !block.isEmpty()) {
                blocks.append(block.toString());
            }
            start = -1;
        }
    };
    qsizetype i = 0;
    while (i < size) {
        const bool tag =
            body.at(i) == u'<' && i + 1 < size &&
            (body.at(i + 1).isLetter() || body.at(i + 1) == u'/' || body.at(i + 1) == u'!');
        if (!tag) {
            if (start < 0 && !body.at(i).isSpace()) {
                start = i;
            }
            ++i;
            continue;
        }
        // The end of the tag, past quoted attribute values that may hold `>`.
        qsizetype end = i + 1;
        if (body.sliced(i).startsWith(u"<!--")) {
            const qsizetype commentEnd = body.indexOf(u"-->", i + 4);
            end = commentEnd < 0 ? size : commentEnd + 3;
        } else {
            QChar quote;
            while (end < size && (!quote.isNull() || body.at(end) != u'>')) {
                if (quote.isNull() && (body.at(end) == u'"' || body.at(end) == u'\'')) {
                    quote = body.at(end);
                } else if (body.at(end) == quote) {
                    quote = QChar();
                }
                ++end;
            }
            end = std::min(end + 1, size);
        }
        const bool closing = body.at(i + 1) == u'/';
        qsizetype nameEnd = i + (closing ? 2 : 1);
        while (nameEnd < end && body.at(nameEnd).isLetterOrNumber()) {
            ++nameEnd;
        }
        const QStringView name =
            body.sliced(i + (closing ? 2 : 1), nameEnd - i - (closing ? 2 : 1));
        const bool single = body.at(i + 1) == u'!' || isVoidElement(name.toString().toLower()) ||
                            body.sliced(i, end - i).endsWith(u"/>");
        if (depth == 0) {
            // Text outside elements ends where an element starts.
            close(i);
            start = i;
            if (closing || single) {
                close(end);
            } else {
                depth = 1;
            }
        } else if (closing) {
            if (--depth == 0) {
                close(end);
            }
        } else if (!single) {
            ++depth;
        }
        i = end;
    }
    close(size);
    return blocks;
}

NoteMerge mergeNote(const std::optional<Note> &base, const Note &local, const Note &remote)
{
    if (local == remote) {
        return {.note = local, .conflict = false};
    }
    const bool localLater = local.meta.updated > remote.meta.updated;
    if (!base) {
        if (local.body != remote.body) {
            return {.note = remote, .conflict = true};
        }
        return {.note = localLater ? local : remote, .conflict = false};
    }
    if (local == *base) {
        return {.note = remote, .conflict = false};
    }
    if (remote == *base) {
        return {.note = local, .conflict = false};
    }
    const NoteMeta &b = base->meta;
    const NoteMeta &l = local.meta;
    const NoteMeta &r = remote.meta;
    NoteMerge result;
    NoteMeta &meta = result.note.meta;
    meta.id = r.id;
    meta.title = pick(b.title, l.title, r.title, localLater);
    meta.icon = pick(b.icon, l.icon, r.icon, localLater);
    meta.color = pick(b.color, l.color, r.color, localLater);
    meta.created = pick(b.created, l.created, r.created, localLater);
    meta.updated = std::max(l.updated, r.updated);
    meta.collections =
        mergeSet(b.collections, l.collections, r.collections, [](const QString &id) { return id; });
    meta.tags = mergeSet(b.tags, l.tags, r.tags, tagKey);
    meta.archived = pick(b.archived, l.archived, r.archived, localLater);
    meta.trashed = pick(b.trashed, l.trashed, r.trashed, localLater);
    meta.location = pick(b.location, l.location, r.location, localLater);
    meta.extra = pick(b.extra, l.extra, r.extra, localLater);

    if (local.body == remote.body || remote.body == base->body) {
        result.note.body = local.body;
    } else if (local.body == base->body) {
        result.note.body = remote.body;
    } else if (const auto blocks = mergeBlocks(bodyBlocks(base->body), bodyBlocks(local.body),
                                               bodyBlocks(remote.body))) {
        result.note.body = blocks->join(u'\n');
    } else {
        result.note.body = remote.body;
        result.conflict = true;
    }
    return result;
}

LibraryInfo mergeLibraryInfo(const std::optional<LibraryInfo> &base, const LibraryInfo &local,
                             const LibraryInfo &remote, const QDateTime &now)
{
    // Without a base, values that differ take the remote side: the local one serves as the
    // base. Lists and maps merge with an empty base instead, which keeps what either side has.
    const LibraryInfo &b = base ? *base : local;
    LibraryInfo merged = remote;
    merged.trashRetentionDays =
        pick(b.trashRetentionDays, local.trashRetentionDays, remote.trashRetentionDays, false);
    merged.extra = mergeKeys(b.extra, local.extra, remote.extra);
    merged.extraSettings = mergeKeys(b.extraSettings, local.extraSettings, remote.extraSettings);
    merged.tagColors = mergeKeys(base ? base->tagColors : QMap<QString, QString>(), local.tagColors,
                                 remote.tagColors);

    QHash<QString, QDateTime> deleted;
    for (const QList<Tombstone> *list : {&local.tombstones, &remote.tombstones}) {
        for (const Tombstone &tombstone : *list) {
            const auto known = deleted.constFind(tombstone.id);
            if (known == deleted.constEnd() || tombstone.deleted < *known) {
                deleted.insert(tombstone.id, tombstone.deleted);
            }
        }
    }
    merged.tombstones.clear();
    for (auto it = deleted.cbegin(); it != deleted.cend(); ++it) {
        merged.tombstones.append({.id = it.key(), .deleted = it.value()});
    }
    std::ranges::sort(merged.tombstones, [](const Tombstone &a, const Tombstone &c) {
        return std::tie(a.deleted, a.id) < std::tie(c.deleted, c.id);
    });
    dropExpiredTombstones(merged.tombstones, now);

    merged.favorites = mergeEntries(
        base ? base->favorites : QList<FavoriteRef>(), local.favorites, remote.favorites,
        [](const FavoriteRef &favorite) {
            return (favorite.kind == FavoriteRef::Kind::Note ? u'n' : u'c') + favorite.id;
        },
        [](const FavoriteRef &, const FavoriteRef &) { return false; });
    merged.favorites.removeIf([&](const FavoriteRef &favorite) {
        return favorite.kind == FavoriteRef::Kind::Note && deleted.contains(favorite.id);
    });
    return merged;
}

QList<Collection> mergeCollections(const QList<Collection> &base, const QList<Collection> &local,
                                   const QList<Collection> &remote)
{
    QList<Collection> merged = mergeEntries(
        base, local, remote, [](const Collection &collection) { return collection.id; },
        [](const Collection &l, const Collection &r) { return l.updated > r.updated; });
    // A parent deleted on one side while the other added a child, or each side moving one
    // collection under the other, would leave collections outside the tree; they move to the
    // top level.
    QHash<QString, const Collection *> byId;
    for (const Collection &collection : merged) {
        byId.insert(collection.id, &collection);
    }
    QSet<QString> detached;
    for (const Collection &collection : merged) {
        if (collection.parent && !byId.contains(*collection.parent)) {
            detached.insert(collection.id);
            continue;
        }
        // A collection on a cycle comes back to itself; one hanging below a cycle does not and
        // keeps its parent.
        QSet<QString> path;
        for (std::optional<QString> parent = collection.parent;
             parent && byId.contains(*parent) && !path.contains(*parent);
             parent = byId.value(*parent)->parent) {
            if (*parent == collection.id) {
                detached.insert(collection.id);
                break;
            }
            path.insert(*parent);
        }
    }
    for (Collection &collection : merged) {
        if (detached.contains(collection.id)) {
            collection.parent.reset();
        }
    }
    return merged;
}

QList<Rule> mergeRules(const QList<Rule> &base, const QList<Rule> &local, const QList<Rule> &remote)
{
    return mergeEntries(
        base, local, remote, [](const Rule &rule) { return rule.id; },
        [](const Rule &l, const Rule &r) { return l.updated > r.updated; });
}

} // namespace core
