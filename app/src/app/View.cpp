#include "app/View.h"

#include <QCoreApplication>

#include <array>

namespace {

struct KindName
{
    View::Kind kind = View::Kind::All;
    QLatin1StringView name;
};

constexpr std::array<KindName, 7> KindNames{{
    {.kind = View::Kind::All, .name = QLatin1StringView("all")},
    {.kind = View::Kind::Unfiled, .name = QLatin1StringView("unfiled")},
    {.kind = View::Kind::FavoriteNote, .name = QLatin1StringView("favorite")},
    {.kind = View::Kind::Collection, .name = QLatin1StringView("collection")},
    {.kind = View::Kind::Tag, .name = QLatin1StringView("tag")},
    {.kind = View::Kind::Archive, .name = QLatin1StringView("archive")},
    {.kind = View::Kind::Trash, .name = QLatin1StringView("trash")},
}};

} // namespace

QString View::key() const
{
    for (const KindName &entry : KindNames) {
        if (entry.kind == kind) {
            return id.isEmpty() ? QString(entry.name) : entry.name + u':' + id;
        }
    }
    return QStringLiteral("all");
}

View View::fromKey(const QString &key)
{
    const qsizetype colon = key.indexOf(u':');
    const QString name = colon < 0 ? key : key.left(colon);
    for (const KindName &entry : KindNames) {
        if (entry.name == name) {
            return {.kind = entry.kind, .id = colon < 0 ? QString() : key.mid(colon + 1)};
        }
    }
    return {};
}

core::NoteFilter filterFor(const View &view, const core::LibraryInfo &info,
                           const QList<core::Collection> &collections, bool subcollections)
{
    using Filter = core::NoteFilter::View;
    switch (view.kind) {
    case View::Kind::All:
        return {};
    case View::Kind::Unfiled:
        return {.view = Filter::Unfiled, .ids = {}, .tag = {}};
    case View::Kind::FavoriteNote: {
        QStringList ids;
        for (const core::FavoriteRef &favorite : info.favorites) {
            if (favorite.kind == core::FavoriteRef::Kind::Note) {
                ids.append(favorite.id);
            }
        }
        return {.view = Filter::Notes, .ids = ids, .tag = {}};
    }
    case View::Kind::Collection: {
        // A collection view lists the notes of the collection and its descendants.
        return {.view = Filter::Collections,
                .ids = subcollections ? core::collectionSubtree(collections, view.id)
                                      : QStringList{view.id},
                .tag = {},
                .anyState = false};
    }
    case View::Kind::Tag:
        return {.view = Filter::Tag, .ids = {}, .tag = view.id};
    case View::Kind::Archive:
        return {.view = Filter::Archive, .ids = {}, .tag = {}};
    case View::Kind::Trash:
        return {.view = Filter::Trash, .ids = {}, .tag = {}};
    }
    return {};
}

QString titleFor(const View &view, const QList<core::Collection> &collections)
{
    switch (view.kind) {
    case View::Kind::All:
        return QCoreApplication::translate("View", "All notes");
    case View::Kind::Unfiled:
        return QCoreApplication::translate("View", "Unfiled notes");
    case View::Kind::FavoriteNote:
        return QCoreApplication::translate("View", "Favourites");
    case View::Kind::Collection:
        for (const core::Collection &collection : collections) {
            if (collection.id == view.id) {
                return collection.name;
            }
        }
        return {};
    case View::Kind::Tag:
        return u'#' + view.id;
    case View::Kind::Archive:
        return QCoreApplication::translate("View", "Archive");
    case View::Kind::Trash:
        return QCoreApplication::translate("View", "Trash");
    }
    return {};
}
