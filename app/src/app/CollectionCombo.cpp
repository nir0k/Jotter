#include "app/CollectionCombo.h"

#include "ui/Icons.h"
#include "ui/TagColor.h"

#include <QComboBox>

void addCollectionItems(QComboBox &box, const QList<core::Collection> &collections,
                        const QStringList &excluded)
{
    for (const auto &[collection, depth] : core::collectionTree(collections)) {
        if (excluded.contains(collection->id)) {
            continue;
        }
        const auto color = ui::colorFromKey(collection->color.value_or(QString()));
        box.addItem(color ? ui::icon(collection->icon, *color) : ui::icon(collection->icon),
                    QString(qsizetype{depth} * 3, u' ') + collection->name, collection->id);
    }
}
