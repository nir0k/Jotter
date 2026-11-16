#pragma once

#include "core/Index.h"
#include "core/Note.h"

#include <QWidget>

class QFormLayout;
class QLabel;
class QPushButton;
class QTextDocument;

namespace editor {

/// The editor's information panel, on the right of the text: the created
/// and updated times, the collections and tags, the word and character counts, the number of
/// attachments, the geotag with Copy and Remove, and the notes that link here.
class InfoPanel : public QWidget
{
    Q_OBJECT

public:
    explicit InfoPanel(QWidget *parent = nullptr);

    /// Shows a note's metadata.
    ///
    /// @param meta The note's metadata.
    /// @param collections The names of its collections, in the tree's order.
    void setNote(const core::NoteMeta &meta, const QStringList &collections);

    /// Counts the words, characters and attachments of the note's text.
    void setText(const QTextDocument &document);

    /// Shows the note's geotag.
    ///
    /// @param location The geotag, or no value.
    /// @param unavailable Whether the position was asked for and not available, which the
    ///        panel says when there is no geotag.
    /// @param removable Whether Remove location is offered: the note can be changed.
    void setLocation(const std::optional<core::GeoLocation> &location, bool unavailable,
                     bool removable);

    /// Lists the notes that link to this one, each opening its note.
    void setBacklinks(const QList<core::NoteSummary> &notes);

Q_SIGNALS:
    /// Emitted when a backlink is chosen.
    void noteActivated(const QString &noteId);
    void removeLocationRequested();

protected:
    void changeEvent(QEvent *event) override;

private:
    void retranslate();

    QLabel *m_title;
    QFormLayout *m_form;
    QLabel *m_created;
    QLabel *m_updated;
    QLabel *m_collections;
    QLabel *m_tags;
    QLabel *m_words;
    QLabel *m_characters;
    QLabel *m_attachments;
    QWidget *m_locationBox;
    QLabel *m_location;
    QPushButton *m_copyLocation;
    QPushButton *m_removeLocation;
    /// The backlinks, one button each.
    QWidget *m_backlinks;
    std::optional<core::GeoLocation> m_geotag;
    bool m_unavailable = false;
    bool m_removable = false;
    std::optional<core::NoteMeta> m_meta;
    QStringList m_collectionNames;
};

} // namespace editor
