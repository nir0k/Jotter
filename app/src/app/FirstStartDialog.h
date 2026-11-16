#pragma once

#include "app/DownloadDialog.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
namespace ui {
class Notice;
} // namespace ui

/// The first start: choose the interface language and where to
/// keep the library, then create it, open an existing one or download one from SFTP. Errors
/// stay in the dialog.
class FirstStartDialog : public QDialog
{
    Q_OBJECT

public:
    /// @param language The current interface language (`en` or `ru`).
    explicit FirstStartDialog(const QString &language, QWidget *parent = nullptr);

    /// Explains why the dialog is shown again, such as a library folder that is gone.
    void setProblem(const QString &text);

    /// The folder of the library created or chosen, valid after the dialog was accepted.
    QString libraryPath() const { return m_libraryPath; }

    /// The library downloaded from SFTP with its connection, when that was the choice.
    const std::optional<DownloadedLibrary> &download() const { return m_download; }

Q_SIGNALS:
    /// Emitted when the person picks another language; it applies at once.
    void languageChosen(const QString &language);

protected:
    void changeEvent(QEvent *event) override;

private:
    void retranslate();
    /// Draws the product mark and the globe at the current size, scale and theme.
    void updateArtwork();
    void updateLocationHint();
    void createOrOpen();
    void openExisting();
    /// Downloads a library from SFTP and accepts with it.
    void downloadFromServer();
    void browse();

    QLabel *m_mark;
    QLabel *m_globe;
    QComboBox *m_language;
    QLabel *m_title;
    QLabel *m_text;
    QLabel *m_locationLabel;
    QLineEdit *m_location;
    QPushButton *m_browse;
    QLabel *m_hint;
    ui::Notice *m_notice;
    QPushButton *m_create;
    QPushButton *m_openExisting;
    QPushButton *m_downloadButton;
    QString m_libraryPath;
    std::optional<DownloadedLibrary> m_download;
};
