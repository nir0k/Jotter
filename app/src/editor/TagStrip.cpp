#include "editor/TagStrip.h"

#include "core/TagColors.h"
#include "core/Tags.h"
#include "ui/Icons.h"
#include "ui/StyleSheet.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QCoreApplication>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QResizeEvent>
#include <QStringListModel>
#include <QToolButton>

#include <ranges>

namespace editor {

namespace {

/// Tells whether @p tags has @p tag, compared as tags are (case-folded).
bool hasTag(const QStringList &tags, const QString &tag)
{
    return std::ranges::any_of(tags, [&](const QString &candidate) {
        return core::tagKey(candidate) == core::tagKey(tag);
    });
}

/// Height shared by the pills, the field and +, so the row keeps its height while adding.
int itemHeight()
{
    const ui::Theme &theme = *ui::Theme::current();
    return QFontMetrics(theme.font(ui::TextRole::LabelSmall)).height() + theme.px(0.3);
}

/// One tag as a pill, in the tag's colour. Pills are not buttons; a removable
/// pill takes the focus so Delete can remove it, and shows × while hovered or focused. × lies
/// over the end of the name rather than taking room of its own, so the pill keeps its width and
/// the row does not move when it appears.
class TagPill : public QFrame
{
public:
    /// @param tag The tag shown.
    /// @param color The colour key the style sheet draws the pill in, a tag colour key or a custom
    ///              colour; empty for neutral.
    /// @param remove Called to remove the tag; empty for a read-only pill.
    TagPill(const QString &tag, const QString &color, std::function<void()> remove, QWidget *parent)
        : QFrame(parent)
        , m_remove(std::move(remove))
        , m_button(new QToolButton(this))
    {
        setProperty("tagPill", true);
        setProperty("tagColor", color);
        if (core::customColorValue(color)) {
            setStyleSheet(ui::customPillStyleSheet(color, ui::Theme::current()->roles()));
        }
        setAttribute(Qt::WA_StyledBackground);
        setAccessibleName(tag);
        auto *label = new QLabel(tag, this);
        ui::setTextRole(label, ui::TextRole::LabelSmall);
        const ui::Theme &theme = *ui::Theme::current();
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(theme.px(0.6), 0, theme.px(0.6), 0);
        layout->addWidget(label);
        m_button->raise();
        m_button->setIcon(ui::icon(QStringLiteral("Close")));
        m_button->setIconSize(QSize(theme.px(0.75), theme.px(0.75)));
        m_button->setFixedSize(theme.px(1), theme.px(1));
        m_button->setFocusPolicy(Qt::NoFocus);
        const QString removeText =
            QCoreApplication::translate("editor::TagStrip", "Remove tag %1").arg(tag);
        m_button->setToolTip(removeText);
        m_button->setAccessibleName(removeText);
        m_button->hide();
        setFocusPolicy(m_remove ? Qt::StrongFocus : Qt::NoFocus);
        setFixedHeight(itemHeight());
        if (m_remove) {
            connect(m_button, &QToolButton::clicked, this, [this] { m_remove(); });
        }
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        // × sits at the end of the name, inside the pill's rounded end.
        m_button->move(width() - m_button->width() - ui::Theme::current()->px(0.25),
                       (height() - m_button->height()) / 2);
        QFrame::resizeEvent(event);
    }

    void enterEvent(QEnterEvent *event) override
    {
        m_button->setVisible(m_remove != nullptr);
        QFrame::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        m_button->setVisible(m_remove && hasFocus());
        QFrame::leaveEvent(event);
    }

    void focusInEvent(QFocusEvent *event) override
    {
        m_button->setVisible(m_remove != nullptr);
        QFrame::focusInEvent(event);
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        m_button->setVisible(m_remove && underMouse());
        QFrame::focusOutEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (m_remove && (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)) {
            m_remove();
            return;
        }
        QFrame::keyPressEvent(event);
    }

private:
    std::function<void()> m_remove;
    QToolButton *m_button;
};

} // namespace

TagStrip::TagStrip(QWidget *parent)
    : QWidget(parent)
    , m_layout(new ui::FlowLayout(this, ui::Theme::current()->px(0.4)))
    , m_icon(new QLabel(this))
    , m_caption(new QLabel(this))
    , m_add(new QToolButton(this))
    , m_field(new QLineEdit(this))
    , m_suggestions(new QStringListModel(this))
    , m_completer(new QCompleter(m_suggestions, this))
{
    ui::setTextRole(m_caption, ui::TextRole::Muted);
    m_field->setProperty("tagField", true);
    m_add->setProperty("tagAdd", true);
    m_field->hide();
    m_field->installEventFilter(this);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setFilterMode(Qt::MatchContains);
    m_field->setCompleter(m_completer);
    m_completer->popup()->setProperty("popupList", true);
    updateIcons();
    retranslate();
    rebuild();

    connect(m_add, &QToolButton::clicked, this, &TagStrip::startAdding);
    // Enter adds the tag and keeps the field open for the next one; Enter in the empty field
    // closes it, giving the focus back to +.
    connect(m_field, &QLineEdit::returnPressed, this, [this] {
        // The completer passes Enter to the field before it activates the chosen completion,
        // which then adds the tag itself.
        const QAbstractItemView *popup = m_completer->popup();
        if (popup->isVisible() && popup->currentIndex().isValid()) {
            return;
        }
        if (m_field->text().trimmed().isEmpty()) {
            m_field->clear();
            finishAdding();
            return;
        }
        addTyped();
        updateSuggestions();
    });
    // A completion chosen with the mouse or the keyboard is added at once, like a typed one.
    // It is queued: the field takes the completion's text after this signal.
    connect(
        m_completer, qOverload<const QString &>(&QCompleter::activated), this,
        [this](const QString &tag) {
            m_field->setText(tag);
            addTyped();
            updateSuggestions();
        },
        Qt::QueuedConnection);
    // The interface size can change: pills are made again in the new size.
    connect(ui::Theme::current(), &ui::Theme::changed, this, [this] {
        updateIcons();
        rebuild();
    });
}

void TagStrip::setTags(const QStringList &tags)
{
    if (tags == m_tags) {
        return;
    }
    m_tags = tags;
    rebuild();
}

void TagStrip::setTagColors(const QMap<QString, QString> &colors)
{
    if (colors == m_tagColors) {
        return;
    }
    m_tagColors = colors;
    rebuild();
}

void TagStrip::setSuggestionSource(std::function<QStringList()> source)
{
    m_suggestionSource = std::move(source);
}

void TagStrip::setReadOnly(bool readOnly)
{
    if (readOnly == m_readOnly) {
        return;
    }
    m_readOnly = readOnly;
    if (readOnly) {
        m_field->clear();
        m_field->hide();
    }
    rebuild();
}

void TagStrip::startAdding()
{
    if (m_readOnly) {
        return;
    }
    updateSuggestions();
    m_add->hide();
    m_field->show();
    m_field->setFocus(Qt::OtherFocusReason);
}

bool TagStrip::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_field) {
        if (event->type() == QEvent::KeyPress &&
            static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape &&
            !m_completer->popup()->isVisible()) {
            m_field->clear();
            finishAdding();
            return true;
        }
        // The completion popup takes the focus for a moment; only a real move away closes.
        if (event->type() == QEvent::FocusOut &&
            static_cast<QFocusEvent *>(event)->reason() != Qt::PopupFocusReason) {
            addTyped();
            finishAdding();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TagStrip::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QWidget::changeEvent(event);
}

void TagStrip::rebuild()
{
    // Pills are deleted later: one may be removing itself from its own key handler.
    for (QWidget *pill : std::as_const(m_pills)) {
        pill->hide();
        pill->deleteLater();
    }
    m_pills.clear();
    while (const QLayoutItem *item = m_layout->takeAt(0)) {
        delete item;
    }
    m_layout->addWidget(m_icon);
    m_layout->addWidget(m_caption);
    for (const QString &tag : std::as_const(m_tags)) {
        auto *pill = new TagPill(
            tag, core::tagColor(m_tagColors, tag),
            m_readOnly ? std::function<void()>() : [this, tag] { remove(tag); }, this);
        m_pills.append(pill);
        m_layout->addWidget(pill);
        pill->show();
    }
    m_layout->addWidget(m_field);
    m_layout->addWidget(m_add);
    m_add->setVisible(!m_readOnly && m_field->isHidden());
    setVisible(!m_readOnly || !m_tags.isEmpty());
    // The tab order follows the row: pills, then the field or +.
    QWidget *previous = nullptr;
    for (QWidget *next : std::as_const(m_pills)) {
        if (previous != nullptr) {
            setTabOrder(previous, next);
        }
        previous = next;
    }
    if (previous != nullptr) {
        setTabOrder(previous, m_field);
    }
    setTabOrder(m_field, m_add);
}

void TagStrip::retranslate()
{
    m_caption->setText(tr("Tags:"));
    m_add->setToolTip(tr("Add tag"));
    m_add->setAccessibleName(tr("Add tag"));
    m_field->setPlaceholderText(tr("Tag"));
    m_field->setAccessibleName(tr("New tag"));
}

void TagStrip::updateIcons()
{
    const ui::Theme &theme = *ui::Theme::current();
    const int size = theme.px(1.0);
    m_field->setFixedHeight(itemHeight());
    m_completer->popup()->setFont(theme.font(ui::TextRole::LabelSmall));
    m_add->setFixedSize(itemHeight(), itemHeight());
    m_icon->setPixmap(
        ui::icon(QStringLiteral("Tag")).pixmap(QSize(size, size), devicePixelRatio()));
    m_add->setIcon(ui::icon(QStringLiteral("Add")));
    m_add->setIconSize(QSize(size, size));
}

void TagStrip::updateSuggestions()
{
    // The library's tags, without those the note already has.
    QStringList suggestions = m_suggestionSource ? m_suggestionSource() : QStringList();
    suggestions.removeIf([this](const QString &tag) { return hasTag(m_tags, tag); });
    m_suggestions->setStringList(suggestions);
}

void TagStrip::addTyped()
{
    const QString tag = core::normalizedTag(m_field->text());
    m_field->clear();
    if (tag.isEmpty() || hasTag(m_tags, tag)) {
        return;
    }
    Q_EMIT tagsChanged(m_tags + QStringList{tag});
}

void TagStrip::finishAdding()
{
    if (m_field->isHidden()) {
        return;
    }
    // + takes the focus before the field goes, so the keyboard stays in the strip.
    m_add->setVisible(!m_readOnly);
    if (m_field->hasFocus() && m_add->isVisible()) {
        m_add->setFocus(Qt::OtherFocusReason);
    }
    m_field->hide();
}

void TagStrip::remove(const QString &tag)
{
    // A pill removed from the keyboard passes the focus on, so Delete can be pressed again.
    const bool hadFocus =
        std::ranges::any_of(m_pills, [](QWidget *pill) { return pill->hasFocus(); });
    QStringList tags = m_tags;
    tags.removeAll(tag);
    Q_EMIT tagsChanged(tags);
    if (hadFocus) {
        if (!m_pills.isEmpty()) {
            m_pills.constFirst()->setFocus(Qt::TabFocusReason);
        } else if (m_add->isVisible()) {
            m_add->setFocus(Qt::TabFocusReason);
        }
    }
}

} // namespace editor
