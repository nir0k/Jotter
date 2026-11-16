#include "editor/InfoPanel.h"

#include "core/Files.h"
#include "editor/Formats.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSet>
#include <QTextBlock>
#include <QTextDocument>
#include <QVBoxLayout>

namespace editor {

InfoPanel::InfoPanel(QWidget *parent)
    : QWidget(parent)
    , m_title(new QLabel(this))
    , m_form(new QFormLayout)
    , m_created(new QLabel(this))
    , m_updated(new QLabel(this))
    , m_collections(new QLabel(this))
    , m_tags(new QLabel(this))
    , m_words(new QLabel(this))
    , m_characters(new QLabel(this))
    , m_attachments(new QLabel(this))
    , m_locationBox(new QWidget(this))
    , m_location(new QLabel(m_locationBox))
    , m_copyLocation(new QPushButton(m_locationBox))
    , m_removeLocation(new QPushButton(m_locationBox))
    , m_backlinks(new QWidget(this))
{
    setProperty("infoPanel", true);
    setAttribute(Qt::WA_StyledBackground);
    const ui::Theme &theme = *ui::Theme::current();
    setFixedWidth(theme.px(15));
    ui::setTextRole(m_title, ui::TextRole::SectionTitle);
    auto *layout = new QVBoxLayout(this);
    const int pad = theme.px(1.0);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(theme.px(0.75));
    layout->addWidget(m_title);
    m_form->setContentsMargins(0, 0, 0, 0);
    m_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    m_form->setVerticalSpacing(theme.px(0.5));
    for (QLabel *value :
         {m_created, m_updated, m_collections, m_tags, m_words, m_characters, m_attachments}) {
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_form->addRow(new QLabel(this), value);
    }
    m_location->setWordWrap(true);
    m_location->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *locationLayout = new QVBoxLayout(m_locationBox);
    locationLayout->setContentsMargins(0, 0, 0, 0);
    locationLayout->setSpacing(theme.px(0.3));
    locationLayout->addWidget(m_location);
    auto *locationButtons = new QHBoxLayout;
    locationButtons->addWidget(m_copyLocation);
    locationButtons->addWidget(m_removeLocation);
    locationButtons->addStretch();
    locationLayout->addLayout(locationButtons);
    m_form->addRow(new QLabel(this), m_locationBox);
    auto *backlinks = new QVBoxLayout(m_backlinks);
    backlinks->setContentsMargins(0, 0, 0, 0);
    backlinks->setSpacing(0);
    m_form->addRow(new QLabel(this), m_backlinks);
    m_form->setRowVisible(m_backlinks, false);
    layout->addLayout(m_form);
    layout->addStretch();
    connect(m_copyLocation, &QPushButton::clicked, this, [this] {
        if (m_geotag) {
            // Decimal degrees with a point, as maps and other programs take them.
            QApplication::clipboard()->setText(
                QStringLiteral("%1, %2").arg(QString::number(m_geotag->latitude, 'f', 5),
                                             QString::number(m_geotag->longitude, 'f', 5)));
        }
    });
    connect(m_removeLocation, &QPushButton::clicked, this, &InfoPanel::removeLocationRequested);
    retranslate();
}

void InfoPanel::setLocation(const std::optional<core::GeoLocation> &location, bool unavailable,
                            bool removable)
{
    m_geotag = location;
    m_unavailable = unavailable;
    m_removable = removable;
    const QLocale locale;
    if (location) {
        QString text = QStringLiteral("%1, %2").arg(locale.toString(location->latitude, 'f', 5),
                                                    locale.toString(location->longitude, 'f', 5));
        if (location->accuracy) {
            text +=
                u'\n' + tr("Accurate to %1 m").arg(locale.toString(qRound(*location->accuracy)));
        }
        m_location->setText(text);
    } else {
        m_location->setText(unavailable
                                ? tr("The position was not available when the note was created.")
                                : tr("None"));
    }
    m_copyLocation->setVisible(location.has_value());
    m_removeLocation->setVisible(location.has_value() && removable);
}

void InfoPanel::setBacklinks(const QList<core::NoteSummary> &notes)
{
    QLayout *list = m_backlinks->layout();
    while (const QLayoutItem *item = list->takeAt(0)) {
        // A backlink's own button may be the one being clicked.
        item->widget()->deleteLater();
        delete item;
    }
    for (const core::NoteSummary &note : notes) {
        auto *button =
            new QPushButton(note.title.isEmpty() ? tr("Untitled") : note.title, m_backlinks);
        button->setProperty("flat", true);
        button->setToolTip(tr("Open this note"));
        connect(button, &QPushButton::clicked, this,
                [this, id = note.id] { Q_EMIT noteActivated(id); });
        list->addWidget(button);
    }
    m_form->setRowVisible(m_backlinks, !notes.isEmpty());
}

void InfoPanel::setNote(const core::NoteMeta &meta, const QStringList &collections)
{
    m_meta = meta;
    m_collectionNames = collections;
    m_created->setText(core::displayTime(meta.created));
    m_updated->setText(core::displayTime(meta.updated));
    m_collections->setText(collections.isEmpty() ? tr("None") : collections.join(u", "));
    QStringList tags;
    for (const QString &tag : meta.tags) {
        tags.append(u'#' + tag);
    }
    m_tags->setText(tags.isEmpty() ? tr("None") : tags.join(u' '));
}

void InfoPanel::setText(const QTextDocument &document)
{
    qsizetype words = 0;
    qsizetype characters = 0;
    QSet<std::pair<QString, QString>> files;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        bool inWord = false;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const QTextCharFormat format = fragment.charFormat();
            if (format.objectType() == ImageObject || format.objectType() == AttachmentObject) {
                files.insert(
                    {format.stringProperty(ObjectNote), format.stringProperty(ObjectFile)});
                inWord = false;
                continue;
            }
            // A word is a run of letters and digits; characters count everything but line
            // breaks.
            for (const QChar c : fragment.text()) {
                if (c == QChar::LineSeparator) {
                    inWord = false;
                    continue;
                }
                ++characters;
                const bool letter = c.isLetterOrNumber() || c == u'\'' || c == QChar(0x2019);
                if (letter && !inWord) {
                    ++words;
                }
                inWord = letter;
            }
        }
    }
    const QLocale locale;
    m_words->setText(locale.toString(words));
    m_characters->setText(locale.toString(characters));
    m_attachments->setText(locale.toString(files.size()));
}

void InfoPanel::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QWidget::changeEvent(event);
}

void InfoPanel::retranslate()
{
    m_title->setText(tr("Note info"));
    const std::array labels{tr("Created"),     tr("Updated"),  tr("Collections"),
                            tr("Tags"),        tr("Words"),    tr("Characters"),
                            tr("Attachments"), tr("Location"), tr("Linked from")};
    for (int row = 0; row < m_form->rowCount(); ++row) {
        if (auto *label =
                qobject_cast<QLabel *>(m_form->itemAt(row, QFormLayout::LabelRole)->widget())) {
            label->setText(labels.at(static_cast<std::size_t>(row)));
            ui::setTextRole(label, ui::TextRole::LabelSmall);
        }
    }
    m_copyLocation->setText(tr("Copy"));
    m_removeLocation->setText(tr("Remove"));
    m_removeLocation->setToolTip(tr("Remove location"));
    if (m_meta) {
        setNote(*m_meta, m_collectionNames);
    }
    setLocation(m_geotag, m_unavailable, m_removable);
}

} // namespace editor
