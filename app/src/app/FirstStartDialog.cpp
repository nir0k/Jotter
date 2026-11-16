#include "app/FirstStartDialog.h"

#include "core/Library.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <algorithm>

FirstStartDialog::FirstStartDialog(const QString &language, QWidget *parent)
    : QDialog(parent)
    , m_mark(new QLabel(this))
    , m_globe(new QLabel(this))
    , m_language(new QComboBox(this))
    , m_title(new QLabel(this))
    , m_text(new QLabel(this))
    , m_locationLabel(new QLabel(this))
    , m_location(new QLineEdit(this))
    , m_browse(new QPushButton(this))
    , m_hint(new QLabel(this))
    , m_notice(new ui::Notice(this))
    , m_create(new QPushButton(this))
    , m_openExisting(new QPushButton(this))
    , m_downloadButton(new QPushButton(this))
{
    const ui::Theme &theme = *ui::Theme::current();
    // Languages are listed in their own names, so a person can find theirs without reading
    // the current one.
    m_language->addItem(QStringLiteral("English"), QStringLiteral("en"));
    m_language->addItem(QStringLiteral("Русский"), QStringLiteral("ru"));
    m_language->setCurrentIndex(std::max(0, m_language->findData(language)));
    ui::setTextRole(m_title, ui::TextRole::PageTitle);
    ui::setTextRole(m_text, ui::TextRole::Muted);
    ui::setTextRole(m_locationLabel, ui::TextRole::Label);
    ui::setTextRole(m_hint, ui::TextRole::Hint);
    m_mark->setAlignment(Qt::AlignCenter);
    m_title->setAlignment(Qt::AlignCenter);
    m_text->setAlignment(Qt::AlignCenter);
    m_text->setWordWrap(true);
    m_hint->setWordWrap(true);
    m_location->setText(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                        QStringLiteral("/Jotter"));
    m_create->setProperty("role", QStringLiteral("primary"));
    m_create->setDefault(true);
    m_openExisting->setFlat(true);
    m_openExisting->setIcon(ui::icon(QStringLiteral("Folder")));
    m_downloadButton->setFlat(true);
    m_downloadButton->setIcon(ui::icon(QStringLiteral("Cloud")));
    updateArtwork();

    // The form sits in a card laid out like a sign-in form: the location, the
    // primary action, the other ways to get a library and, at the bottom, the language.
    auto *card = new QFrame(this);
    card->setProperty("section", true);
    auto *locationRow = new QHBoxLayout;
    locationRow->addWidget(m_location, 1);
    locationRow->addWidget(m_browse);
    auto *createRow = new QHBoxLayout;
    createRow->addStretch();
    createRow->addWidget(m_create);
    auto *languageRow = new QHBoxLayout;
    languageRow->addWidget(m_globe);
    languageRow->addWidget(m_language);
    languageRow->addStretch();
    const auto divider = [card] {
        auto *line = new QFrame(card);
        line->setProperty("divider", true);
        return line;
    };
    auto *form = new QVBoxLayout(card);
    const int cardPad = theme.px(1.25);
    form->setContentsMargins(cardPad, cardPad, cardPad, cardPad);
    form->setSpacing(theme.px(0.5));
    form->addWidget(m_locationLabel);
    form->addLayout(locationRow);
    form->addWidget(m_hint);
    form->addWidget(m_notice);
    form->addLayout(createRow);
    form->addSpacing(theme.px(0.25));
    form->addWidget(divider());
    form->addWidget(m_openExisting);
    form->addWidget(m_downloadButton);
    form->addWidget(divider());
    form->addLayout(languageRow);

    // A narrow column centred in the window, so a maximised or tiled window keeps the form at
    // its size and leaves the rest to the background.
    auto *column = new QWidget(this);
    column->setMaximumWidth(theme.px(28));
    auto *columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(0, 0, 0, 0);
    columnLayout->setSpacing(theme.px(0.5));
    columnLayout->addWidget(m_mark);
    columnLayout->addSpacing(theme.px(0.25));
    columnLayout->addWidget(m_title);
    columnLayout->addWidget(m_text);
    columnLayout->addSpacing(theme.px(1.0));
    columnLayout->addWidget(card);

    auto *row = new QHBoxLayout;
    row->addStretch(1);
    // A large stretch lets the column grow up to its maximum width before the sides do.
    row->addWidget(column, 100);
    row->addStretch(1);
    auto *layout = new QVBoxLayout(this);
    const int pad = theme.px(2.0);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->addStretch(1);
    layout->addLayout(row);
    layout->addStretch(1);

    connect(m_language, &QComboBox::activated, this,
            [this](int index) { Q_EMIT languageChosen(m_language->itemData(index).toString()); });
    connect(&theme, &ui::Theme::changed, this, &FirstStartDialog::updateArtwork);
    connect(m_location, &QLineEdit::textChanged, this, &FirstStartDialog::updateLocationHint);
    connect(m_browse, &QPushButton::clicked, this, &FirstStartDialog::browse);
    connect(m_create, &QPushButton::clicked, this, &FirstStartDialog::createOrOpen);
    connect(m_openExisting, &QPushButton::clicked, this, &FirstStartDialog::openExisting);
    connect(m_downloadButton, &QPushButton::clicked, this, &FirstStartDialog::downloadFromServer);
    retranslate();
    // The location is the decision of this screen, so it takes the focus rather than the
    // language list at the bottom.
    m_location->setFocus();
    resize(theme.px(34), sizeHint().height());
}

void FirstStartDialog::updateArtwork()
{
    const ui::Theme &theme = *ui::Theme::current();
    const qreal ratio = devicePixelRatioF();
    const int markSize = theme.px(4.0);
    m_mark->setPixmap(QApplication::windowIcon().pixmap(QSize(markSize, markSize), ratio));
    const int glyphSize = theme.px(1.0);
    m_globe->setPixmap(
        ui::icon(QStringLiteral("Globe")).pixmap(QSize(glyphSize, glyphSize), ratio));
}

void FirstStartDialog::setProblem(const QString &text)
{
    m_notice->showNotice(ui::Notice::Kind::Warning, text);
}

void FirstStartDialog::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QDialog::changeEvent(event);
}

void FirstStartDialog::retranslate()
{
    setWindowTitle(tr("Welcome to Jotter"));
    m_language->setAccessibleName(tr("Language"));
    m_title->setText(tr("Welcome to Jotter"));
    m_text->setText(tr("Jotter keeps your notes in a library: a folder of ordinary files that any "
                       "browser can open, even without Jotter."));
    m_locationLabel->setText(tr("Where to keep the library"));
    m_browse->setText(tr("Choose…"));
    m_openExisting->setText(tr("Open existing library…"));
    m_downloadButton->setText(tr("Download from SFTP…"));
    updateLocationHint();
}

void FirstStartDialog::updateLocationHint()
{
    const QString path = m_location->text().trimmed();
    const bool existing = !path.isEmpty() && core::Library::exists(path);
    m_create->setText(existing ? tr("Open library") : tr("Create library"));
    m_create->setEnabled(!path.isEmpty());
    m_hint->setText(existing ? tr("This folder already holds a Jotter library; it will be opened.")
                             : tr("A new folder is created there if it does not exist."));
}

void FirstStartDialog::createOrOpen()
{
    const QString path = QDir::cleanPath(m_location->text().trimmed());
    if (core::Library::exists(path)) {
        m_libraryPath = path;
        accept();
        return;
    }
    auto library = core::Library::create(path);
    if (!library) {
        m_notice->showNotice(ui::Notice::Kind::Error, core::describe(library.error()));
        return;
    }
    m_libraryPath = library->root();
    accept();
}

void FirstStartDialog::openExisting()
{
    const QString path =
        QFileDialog::getExistingDirectory(this, tr("Open existing library"), m_location->text());
    if (path.isEmpty()) {
        return;
    }
    if (!core::Library::exists(path)) {
        m_notice->showNotice(ui::Notice::Kind::Error,
                             tr("%1 is not a Jotter library: it has no library.json.")
                                 .arg(QDir::toNativeSeparators(path)));
        return;
    }
    m_libraryPath = path;
    accept();
}

void FirstStartDialog::downloadFromServer()
{
    DownloadDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted || !dialog.library()) {
        return;
    }
    m_download = dialog.library();
    m_libraryPath = m_download->root;
    accept();
}

void FirstStartDialog::browse()
{
    const QString path = QFileDialog::getExistingDirectory(
        this, tr("Where to keep the library"), QFileInfo(m_location->text()).absolutePath());
    if (!path.isEmpty()) {
        m_location->setText(QDir::toNativeSeparators(path));
    }
}
