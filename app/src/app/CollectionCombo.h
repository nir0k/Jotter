#pragma once

#include "core/Library.h"

#include <QList>
#include <QStringList>

class QComboBox;

/// Fills a combo box with the collection tree, each collection with its icon in its colour and
/// indented by its depth, its identifier as the item data.
///
/// @param box The combo box; its items are appended.
/// @param collections The collection tree.
/// @param excluded Collections left out, such as those being deleted.
void addCollectionItems(QComboBox &box, const QList<core::Collection> &collections,
                        const QStringList &excluded = {});
