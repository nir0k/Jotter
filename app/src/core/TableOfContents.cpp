#include "core/TableOfContents.h"

#include "core/Html.h"
#include "core/NoteLinks.h"

#include <QCollator>
#include <QCoreApplication>
#include <QLocale>
#include <QMultiHash>
#include <QSet>

#include <ranges>

namespace core {

namespace {

/// The style of the table of contents: readable type in a narrow column, in light colours.
constexpr QLatin1StringView
    Style("body{font-family:system-ui,sans-serif;line-height:1.5;max-width:45rem;margin:2rem auto;"
          "padding:0 1rem;color:#1f2328;background:#fff}"
          "h1{font-size:1.6rem}h2,h3,h4,h5,h6{margin:1.5rem 0 .5rem}"
          "ul{list-style:none;padding:0;margin:0}li{margin:.25rem 0}"
          "a{color:#0969da;text-decoration:none}a:hover{text-decoration:underline}"
          ".meta{color:#59636e;font-size:.85em;margin-left:.5rem}");

/// Writes one note as an entry: its link, its updated date and its tags.
QString entry(const NoteSummary &note, const QLocale &locale)
{
    const QString title = note.title.isEmpty()
                              ? QCoreApplication::translate("TableOfContents", "Untitled")
                              : note.title;
    QString meta = locale.toString(note.updated.toLocalTime().date(), QLocale::ShortFormat);
    for (const QString &tag : note.tags) {
        meta += QStringLiteral(" #") + tag;
    }
    return QStringLiteral("<li><a href=\"notes/%1\">%2</a><span class=\"meta\">%3</span></li>\n")
        .arg(html::escapeAttribute(noteLinkAddress(note.id)), html::escapeText(title),
             html::escapeText(meta));
}

QString list(const QList<const NoteSummary *> &notes, const QLocale &locale)
{
    QString text = QStringLiteral("<ul>\n");
    for (const NoteSummary *note : notes) {
        text += entry(*note, locale);
    }
    return text + QStringLiteral("</ul>\n");
}

} // namespace

QByteArray tableOfContents(const QList<NoteSummary> &notes, const QList<Collection> &collections)
{
    const QLocale locale;
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    QList<const NoteSummary *> sorted;
    for (const NoteSummary &note : notes) {
        if (!note.trashed) {
            sorted.append(&note);
        }
    }
    std::ranges::stable_sort(sorted, [&](const NoteSummary *a, const NoteSummary *b) {
        return collator.compare(a->title, b->title) < 0;
    });

    QMultiHash<QString, const NoteSummary *> byCollection;
    QList<const NoteSummary *> loose;
    QList<const NoteSummary *> archived;
    for (const NoteSummary *note : std::as_const(sorted)) {
        if (note->archived) {
            archived.append(note);
            continue;
        }
        if (note->collections.isEmpty()) {
            loose.append(note);
        }
        for (const QString &collection : note->collections) {
            byCollection.insert(collection, note);
        }
    }

    const QList<CollectionLevel> tree = collectionTree(collections);
    // A collection is listed when it or one of its descendants holds notes; the tree lists each
    // parent before its children, so walking it backwards reaches every child first.
    QSet<QString> used;
    for (const CollectionLevel &level : tree | std::views::reverse) {
        const Collection &collection = *level.collection;
        if (byCollection.contains(collection.id) || used.contains(collection.id)) {
            used.insert(collection.id);
            if (collection.parent) {
                used.insert(*collection.parent);
            }
        }
    }

    QString body;
    for (const auto &[collection, depth] : tree) {
        if (!used.contains(collection->id)) {
            continue;
        }
        const int level = std::min(6, depth + 2);
        body +=
            QStringLiteral("<h%1>%2</h%1>\n").arg(level).arg(html::escapeText(collection->name));
        QList<const NoteSummary *> own = byCollection.values(collection->id);
        std::ranges::stable_sort(own, [&](const NoteSummary *a, const NoteSummary *b) {
            return collator.compare(a->title, b->title) < 0;
        });
        if (!own.isEmpty()) {
            body += list(own, locale);
        }
    }
    if (!loose.isEmpty()) {
        body += QStringLiteral("<h2>%1</h2>\n")
                    .arg(html::escapeText(QCoreApplication::translate(
                        "TableOfContents", "Notes without a collection")));
        body += list(loose, locale);
    }
    if (!archived.isEmpty()) {
        body +=
            QStringLiteral("<h2>%1</h2>\n")
                .arg(html::escapeText(QCoreApplication::translate("TableOfContents", "Archive")));
        body += list(archived, locale);
    }
    if (body.isEmpty()) {
        body = QStringLiteral("<p>%1</p>\n")
                   .arg(html::escapeText(QCoreApplication::translate(
                       "TableOfContents", "This library has no notes yet.")));
    }
    const QString title = QCoreApplication::translate("TableOfContents", "Jotter library");
    return QStringLiteral("<!doctype html>\n<html lang=\"%1\">\n<head>\n<meta charset=\"utf-8\">\n"
                          "<title>%2</title>\n<style>%3</style>\n</head>\n<body>\n<h1>%2</h1>\n%4"
                          "</body>\n</html>\n")
        .arg(locale.bcp47Name(), html::escapeText(title), Style, body)
        .toUtf8();
}

} // namespace core
