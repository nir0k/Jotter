#include "core/Attachments.h"
#include "core/Directory.h"
#include "core/Files.h"
#include "core/LibrarySession.h"
#include "core/NoteLinks.h"
#include "editor/Formats.h"
#include "editor/HtmlCodec.h"
#include "editor/NoteEditor.h"
#include "editor/NotePicker.h"
#include "editor/Objects.h"
#include "editor/TagStrip.h"
#include "editor/TextEdit.h"
#include "ui/Theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCompleter>
#include <QImage>
#include <QLineEdit>
#include <QMenu>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>

using namespace Qt::StringLiterals;

/// Checks the editor pane against a real library: saving what was typed, the tag strip,
/// read-only notes, and a note changed elsewhere while it is open.
class NoteEditorTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        m_theme.emplace();
        m_theme->apply();
    }

    void init()
    {
        m_dir.emplace();
        QVERIFY(core::Library::create(m_dir->path()));
        auto session = core::LibrarySession::open(m_dir->path());
        QVERIFY(session);
        m_session = std::move(*session);
        QSignalSpy firstScan(m_session.get(), &core::LibrarySession::notesChanged);
        QVERIFY(firstScan.wait());
        m_editor.emplace();
        m_editor->setSession(m_session.get());
    }

    void cleanup()
    {
        m_editor.reset();
        m_session.reset();
        m_dir.reset();
    }

    /// Typed text and title reach the file when the editor saves.
    void savesTyping()
    {
        core::Note note = core::makeNote();
        QVERIFY(m_session->saveNote(note));
        m_editor->openNote(note.meta.id);
        m_editor->findChild<QLineEdit *>()->setText(u"Plans"_s);
        auto *text = m_editor->findChild<editor::TextEdit *>();
        QTest::keyClicks(text, u"Buy milk"_s);
        m_editor->saveNow();

        const auto saved = m_session->library().readNote(note.meta.id);
        QVERIFY(saved);
        QCOMPARE(saved->meta.title, u"Plans"_s);
        QCOMPARE(saved->body, u"<p>Buy milk</p>"_s);
    }

    /// The actual toolbar action selects inline or block code and saves the resulting note.
    void codeToolbar_data()
    {
        QTest::addColumn<int>("start");
        QTest::addColumn<int>("end");
        QTest::addColumn<QString>("expected");
        QTest::newRow("fragment") << 7 << 11 << u"<p>before <code>code</code> after</p>"_s;
        QTest::newRow("whole line") << 0 << 17 << u"<pre><code>before code after</code></pre>"_s;
    }

    void codeToolbar()
    {
        QFETCH(int, start);
        QFETCH(int, end);
        QFETCH(QString, expected);
        core::Note note = core::makeNote();
        note.body = u"<p>before code after</p>"_s;
        QVERIFY(m_session->saveNote(note));
        m_editor->openNote(note.meta.id);
        auto *text = m_editor->findChild<editor::TextEdit *>();
        QVERIFY(text != nullptr);
        QTextCursor cursor(text->document());
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        text->setTextCursor(cursor);
        QAction *code = nullptr;
        for (QAction *action : m_editor->actions()) {
            if (action->text() == u"Code block"_s) {
                code = action;
                break;
            }
        }
        QVERIFY(code != nullptr);
        QVERIFY(code->isEnabled());
        code->trigger();
        QCOMPARE(editor::writeBody(*text->document()), expected);
        m_editor->saveNow();
        const auto saved = m_session->library().readNote(note.meta.id);
        QVERIFY(saved);
        QCOMPARE(saved->body, expected);
        text->undo();
        QCOMPARE(editor::writeBody(*text->document()), note.body);
    }

    /// `[[` opens the note picker, which searches titles; choosing a note replaces the brackets
    /// with a link showing its title.
    void noteLinkFromBrackets()
    {
        core::Note target = core::makeNote();
        target.meta.title = u"Budget"_s;
        QVERIFY(m_session->saveNote(target));
        core::Note note = core::makeNote();
        QVERIFY(m_session->saveNote(note));
        m_editor->openNote(note.meta.id);
        auto *text = m_editor->findChild<editor::TextEdit *>();
        QTest::keyClicks(text, u"see [["_s);
        auto *picker = m_editor->findChild<editor::NotePicker *>();
        QVERIFY(picker->isVisible());
        auto *search = picker->findChild<QLineEdit *>();
        QTest::keyClicks(search, u"budg"_s);
        QTest::keyClick(search, Qt::Key_Return);
        QVERIFY(!picker->isVisible());
        m_editor->saveNow();
        QCOMPARE(
            m_session->library().readNote(note.meta.id)->body,
            u"<p>see <a class=\"note-link\" href=\"%1.html\">Budget</a></p>"_s.arg(target.meta.id));
        QCOMPARE(m_session->index().backlinks(target.meta.id)->size(), 1);
    }

    /// Tags are added and removed in the strip under the title and saved at once.
    void tagStripChangesTags()
    {
        core::Note note = core::makeNote();
        note.meta.tags = {u"home"_s};
        QVERIFY(m_session->saveNote(note));
        m_editor->openNote(note.meta.id);
        auto *strip = m_editor->findChild<editor::TagStrip *>();
        const auto tags = [&] { return m_session->library().readNote(note.meta.id)->meta.tags; };

        strip->startAdding();
        auto *field = strip->findChild<QLineEdit *>();
        QTest::keyClicks(field, u"#work / plans"_s);
        QTest::keyClick(field, Qt::Key_Return);
        QCOMPARE(tags(), (QStringList{u"home"_s, u"work/plans"_s}));
        // A tag the note has, in another case, adds nothing.
        QTest::keyClicks(field, u"HOME"_s);
        QTest::keyClick(field, Qt::Key_Return);
        QCOMPARE(tags().size(), 2);

        QTest::keyClick(pill(strip, u"home"_s), Qt::Key_Delete);
        QCOMPARE(tags(), QStringList{u"work/plans"_s});
    }

    /// Enter keeps the field open, so tags are added one after another, also when a completion
    /// is chosen with the keyboard; Enter in the empty field closes it.
    void tagStripAddsTagsInARow()
    {
        editor::TagStrip strip;
        strip.setSuggestionSource([] { return QStringList{u"travel"_s, u"work"_s}; });
        QObject::connect(&strip, &editor::TagStrip::tagsChanged, &strip,
                         &editor::TagStrip::setTags);
        QSignalSpy changed(&strip, &editor::TagStrip::tagsChanged);
        strip.resize(600, 100);
        strip.show();
        QVERIFY(QTest::qWaitForWindowExposed(&strip));
        strip.startAdding();
        auto *field = strip.findChild<QLineEdit *>();
        QTest::keyClicks(field, u"home"_s);
        QTest::keyClick(field, Qt::Key_Return);
        QVERIFY(field->isVisible());
        QVERIFY(field->text().isEmpty());
        // The field moves on past the new pill.
        QTRY_VERIFY(!field->geometry().intersects(pill(&strip, u"home"_s)->geometry()));

        QTest::keyClicks(field, u"tra"_s);
        QAbstractItemView *popup = field->completer()->popup();
        QTRY_VERIFY(popup->isVisible());
        QTest::keyClick(popup, Qt::Key_Down);
        QTest::keyClick(popup, Qt::Key_Return);
        QTRY_COMPARE(changed.size(), 2);
        QCOMPARE(changed.last().first().toStringList(), (QStringList{u"home"_s, u"travel"_s}));
        QVERIFY(field->isVisible());
        QVERIFY(field->text().isEmpty());

        QTest::keyClick(field, Qt::Key_Return);
        QVERIFY(field->isHidden());
        QCOMPARE(changed.size(), 2);
    }

    /// A tag renamed elsewhere in Jotter reaches the open note, and a later save keeps it.
    void renamedTagReachesOpenNote()
    {
        core::Note note = core::makeNote();
        note.meta.tags = {u"work"_s};
        QVERIFY(m_session->saveNote(note));
        m_editor->openNote(note.meta.id);
        QVERIFY(m_session->renameTag(u"work"_s, u"job"_s));
        QVERIFY(pill(m_editor->findChild<editor::TagStrip *>(), u"job"_s) != nullptr);

        QTest::keyClicks(m_editor->findChild<editor::TextEdit *>(), u"more"_s);
        m_editor->saveNow();
        QCOMPARE(m_session->library().readNote(note.meta.id)->meta.tags, QStringList{u"job"_s});
    }

    /// Renaming a note in another window renames the link in the open note, and a later save
    /// keeps it (regression).
    void renamedLinkReachesOpenNote()
    {
        core::Note target = core::makeNote();
        target.meta.title = u"Old"_s;
        QVERIFY(m_session->saveNote(target));
        core::Note linking = core::makeNote();
        linking.body = u"<p><a class=\"note-link\" href=\"%1\">Old</a></p>"_s.arg(
            core::noteLinkAddress(target.meta.id));
        QVERIFY(m_session->saveNote(linking));
        m_editor->openNote(linking.meta.id);

        target.meta.title = u"New"_s;
        QVERIFY(m_session->saveNote(target));
        auto *text = m_editor->findChild<editor::TextEdit *>();
        QTRY_COMPARE(text->toPlainText(), u"New"_s);

        text->moveCursor(QTextCursor::End);
        QTest::keyClicks(text, u"more "_s);
        m_editor->saveNow();
        const QString body = m_session->library().readNote(linking.meta.id)->body;
        QVERIFY2(body.contains(u"\">New") && !body.contains(u"Old"), qPrintable(body));
    }

    /// Renaming a note that others link to disables the editor while the links are renamed;
    /// the title being typed gets the keyboard focus back afterwards (regression).
    void titleKeepsFocusWhileLinksRename()
    {
        core::Note target = core::makeNote();
        target.meta.title = u"Old"_s;
        QVERIFY(m_session->saveNote(target));
        core::Note linking = core::makeNote();
        linking.body = u"<p><a class=\"note-link\" href=\"%1\">Old</a></p>"_s.arg(
            core::noteLinkAddress(target.meta.id));
        QVERIFY(m_session->saveNote(linking));
        m_editor->show();
        m_editor->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&*m_editor));
        m_editor->openNote(target.meta.id);
        QLineEdit *title = nullptr;
        for (QLineEdit *edit : m_editor->findChildren<QLineEdit *>()) {
            if (edit->property("titleField").toBool()) {
                title = edit;
            }
        }
        QVERIFY(title != nullptr);
        title->setFocus();
        title->end(false);
        QTest::keyClicks(title, u"er"_s);

        QSignalSpy mutation(m_session.get(), &core::LibrarySession::mutationChanged);
        QVERIFY(m_editor->saveNow());
        QCOMPARE(mutation.count(), 1);
        QVERIFY(!m_editor->isEnabled());
        QTRY_VERIFY(m_editor->isEnabled());
        QCOMPARE(QApplication::focusWidget(), title);
        QTest::keyClicks(title, u"!"_s);
        QCOMPARE(title->text(), u"Older!"_s);
    }

    /// A trashed note opens read-only and is never written.
    void trashedIsReadOnly()
    {
        core::Note note = core::makeNote();
        note.meta.trashed = core::now();
        QVERIFY(m_session->saveNote(note));
        m_editor->openNote(note.meta.id);
        QVERIFY(m_editor->findChild<editor::TextEdit *>()->isReadOnly());
        QVERIFY(m_editor->findChild<QLineEdit *>()->isReadOnly());
    }

    /// The text's own menus, such as its context menu, open where they are asked to; only the
    /// toolbar's menus are moved above the toolbar (regression).
    void textMenuOpensInPlace()
    {
        core::Note note = core::makeNote();
        QVERIFY(m_session->saveNote(note));
        m_editor->resize(800, 600);
        m_editor->show();
        QVERIFY(QTest::qWaitForWindowExposed(&*m_editor));
        m_editor->openNote(note.meta.id);
        auto *text = m_editor->findChild<editor::TextEdit *>();
        QMenu menu(text);
        menu.addAction(u"Copy"_s);
        const QPoint position = text->mapToGlobal(QPoint(20, 20));
        menu.popup(position);
        QVERIFY(QTest::qWaitForWindowExposed(&menu));
        QCOMPARE(menu.pos(), position);
    }

    /// When another program changes the open note while it has unsaved edits, the outside
    /// version is shown and the local text survives as a conflict copy.
    void conflictKeepsBothVersions()
    {
        core::Note note = core::makeNote();
        note.meta.title = u"Shared"_s;
        note.body = u"<p>original</p>"_s;
        QVERIFY(m_session->saveNote(note));
        m_editor->openNote(note.meta.id);
        auto *text = m_editor->findChild<editor::TextEdit *>();
        text->moveCursor(QTextCursor::End);
        QTest::keyClicks(text, u" local"_s);

        note.body = u"<p>outside</p>"_s;
        QVERIFY(core::writeFileAtomic(m_session->library().notePath(note.meta.id),
                                      core::serializeNote(note)));
        m_editor->checkOutsideChanges();

        QCOMPARE(text->toPlainText(), u"outside"_s);
        const auto all = m_session->index().notes({}, {}).value();
        QCOMPARE(all.size(), 2);
        const auto copy = std::ranges::find_if(
            all, [&](const core::NoteSummary &s) { return s.id != note.meta.id; });
        QVERIFY(copy->title.startsWith(u"Shared (conflict"_s));
        QCOMPARE(m_session->library().readNote(copy->id)->body, u"<p>original local</p>"_s);
    }

    /// A save that finds the file changed outside before the watcher told the editor keeps both
    /// versions in the same way, without overwriting the outside one.
    void saveAfterOutsideChangeKeepsBoth()
    {
        core::Note note = core::makeNote();
        note.meta.title = u"Shared"_s;
        note.body = u"<p>original</p>"_s;
        QVERIFY(m_session->saveNote(note));
        m_editor->openNote(note.meta.id);
        auto *text = m_editor->findChild<editor::TextEdit *>();
        text->moveCursor(QTextCursor::End);
        QTest::keyClicks(text, u" local"_s);

        note.body = u"<p>outside</p>"_s;
        QVERIFY(core::writeFileAtomic(m_session->library().notePath(note.meta.id),
                                      core::serializeNote(note)));
        QVERIFY(m_editor->saveNow());

        QCOMPARE(text->toPlainText(), u"outside"_s);
        QCOMPARE(m_session->library().readNote(note.meta.id)->body, u"<p>outside</p>"_s);
        QCOMPARE(m_session->index().notes({}, {}).value().size(), 2);
    }

    /// Two views share typing, title, formatting and Undo, but retain their own cursor.
    void twoViewsShareBuffer()
    {
        core::Note note = core::makeNote();
        note.body = u"<p>base</p>"_s;
        QVERIFY(m_session->saveNote(note));
        QVERIFY(m_editor->openNote(note.meta.id));
        editor::NoteEditor second;
        QVERIFY(second.setSession(m_session.get()));
        QVERIFY(second.openNote(note.meta.id));
        auto *firstText = m_editor->findChild<editor::TextEdit *>();
        auto *secondText = second.findChild<editor::TextEdit *>();
        QCOMPARE(firstText->document(), secondText->document());
        firstText->moveCursor(QTextCursor::End);
        secondText->moveCursor(QTextCursor::Start);
        QTest::keyClicks(firstText, u" one"_s);
        QCOMPARE(secondText->toPlainText(), u"base one"_s);
        QCOMPARE(secondText->textCursor().position(), 0);
        secondText->undo();
        QCOMPARE(firstText->toPlainText(), u"base"_s);
        secondText->redo();
        QCOMPARE(firstText->toPlainText(), u"base one"_s);
        auto *title = m_editor->findChildren<QLineEdit *>().first();
        QTest::keyClicks(title, u"Shared title"_s);
        QCOMPARE(second.findChildren<QLineEdit *>().first()->text(), u"Shared title"_s);
        QVERIFY(second.closeNote());
        QCOMPARE(firstText->toPlainText(), u"base one"_s);
        QVERIFY(m_editor->saveNow());
        QCOMPARE(m_session->library().readNote(note.meta.id)->body, u"<p>base one</p>"_s);
    }

    /// Loading a note fills a new document: an outside change reaches every view of the note,
    /// each keeping its cursor, leaves nothing to undo, and typing afterwards is still saved by
    /// the autosave timer.
    void reloadReplacesDocument()
    {
        core::Note note = core::makeNote();
        note.body = u"<p>first version</p>"_s;
        QVERIFY(m_session->saveNote(note));
        QVERIFY(m_editor->openNote(note.meta.id));
        editor::NoteEditor second;
        QVERIFY(second.setSession(m_session.get()));
        QVERIFY(second.openNote(note.meta.id));
        auto *firstText = m_editor->findChild<editor::TextEdit *>();
        auto *secondText = second.findChild<editor::TextEdit *>();
        const QTextDocument *before = firstText->document();
        firstText->moveCursor(QTextCursor::End);
        QTextCursor cursor = secondText->textCursor();
        cursor.setPosition(5);
        secondText->setTextCursor(cursor);

        note.body = u"<p>second version</p>"_s;
        QVERIFY(core::writeFileAtomic(m_session->library().notePath(note.meta.id),
                                      core::serializeNote(note)));
        m_editor->checkOutsideChanges();

        QCOMPARE(firstText->toPlainText(), u"second version"_s);
        QVERIFY(firstText->document() != before);
        QCOMPARE(secondText->document(), firstText->document());
        QCOMPARE(firstText->textCursor().position(), 13);
        QCOMPARE(secondText->textCursor().position(), 5);
        QVERIFY(!firstText->document()->isUndoAvailable());
        QVERIFY(!firstText->document()->isModified());

        firstText->moveCursor(QTextCursor::End);
        QTest::keyClicks(firstText, u" typed"_s);
        QTRY_COMPARE(m_session->library().readNote(note.meta.id)->body,
                     u"<p>second version typed</p>"_s);
    }

    /// A note opens with the cursor and the view at its start, also when it holds tables
    /// (regression: such a note opened scrolled to its end).
    void opensAtStart()
    {
        core::Note note = core::makeNote();
        for (int i = 0; i < 50; ++i) {
            note.body += u"<p>Text</p>\n<table>\n<tbody>\n<tr><td><p>cell</p></td></tr>\n</tbody>\n"
                         u"</table>\n"_s;
        }
        QVERIFY(m_session->saveNote(note));
        m_editor->resize(600, 400);
        m_editor->show();
        QVERIFY(m_editor->openNote(note.meta.id));
        auto *text = m_editor->findChild<editor::TextEdit *>();
        QApplication::processEvents();
        QVERIFY(text->verticalScrollBar()->maximum() > 0);
        QCOMPARE(text->textCursor().position(), 0);
        QCOMPARE(text->verticalScrollBar()->value(), 0);
    }

    /// The images of a loaded note find their files in the note's attachment folder.
    void imagesFindTheirFiles()
    {
        core::Note note = core::makeNote();
        const QString folder = m_session->library().attachmentsDir(note.meta.id);
        QVERIFY(QDir().mkpath(folder));
        QImage image(4, 4, QImage::Format_RGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(folder + u"/red.png"_s));
        note.body =
            u"<p><img src=\"%1\"></p>"_s.arg(core::attachmentSource(note.meta.id, u"red.png"_s));
        QVERIFY(m_session->saveNote(note));
        QVERIFY(m_editor->openNote(note.meta.id));

        const QTextDocument *document = m_editor->findChild<editor::TextEdit *>()->document();
        const auto *objects = document->findChild<editor::ObjectHandler *>();
        QVERIFY(objects != nullptr);
        std::optional<QTextFormat> imageFormat;
        for (auto it = document->begin().begin(); !it.atEnd(); ++it) {
            if (it.fragment().charFormat().objectType() == editor::ImageObject) {
                imageFormat = it.fragment().charFormat();
            }
        }
        QVERIFY(imageFormat);
        QVERIFY(QFileInfo(objects->filePath(*imageFormat)).isFile());
    }

    /// Navigation and detachment cannot discard text while its directory is unwritable.
    void failedSaveKeepsBuffer()
    {
        core::Note note = core::makeNote();
        QVERIFY(m_session->saveNote(note));
        QVERIFY(m_editor->openNote(note.meta.id));
        auto *text = m_editor->findChild<editor::TextEdit *>();
        QTest::keyClicks(text, u"retained"_s);
        const QString notes = m_session->library().notesDir();
        QVERIFY(QDir().rename(notes, notes + u"-saved"_s));
        QVERIFY(core::writeFileAtomic(notes, "blocked"));
        QVERIFY(!m_editor->saveNow());
        QVERIFY(!m_editor->openNote(core::newId()));
        QVERIFY(!m_editor->closeNote());
        QVERIFY(!m_editor->setSession(nullptr));
        QCOMPARE(text->toPlainText(), u"retained"_s);
        QCOMPARE(m_editor->noteId(), note.meta.id);
        QVERIFY(core::removePath(notes));
        QVERIFY(QDir().rename(notes + u"-saved"_s, notes));
        QVERIFY(m_editor->saveNow());
        QCOMPARE(m_session->library().readNote(note.meta.id)->body, u"<p>retained</p>"_s);
    }

    /// Failure to preserve a conflict copy must leave the unsaved version in both views. The
    /// copy fails on an attachment that cannot be read, a folder in place of the file; a missing
    /// file would only stay a broken reference.
    void failedConflictCopyKeepsTyping()
    {
        core::Note note = core::makeNote();
        note.body = u"<p>base<img src=\""_s +
                    core::attachmentSource(note.meta.id, u"unreadable.png"_s) + u"\"></p>"_s;
        QVERIFY(core::makeDirectory(m_session->library().attachmentsDir(note.meta.id) +
                                    u"/unreadable.png"_s));
        QVERIFY(m_session->saveNote(note));
        QVERIFY(m_editor->openNote(note.meta.id));
        auto *text = m_editor->findChild<editor::TextEdit *>();
        text->moveCursor(QTextCursor::End);
        QTest::keyClicks(text, u" retained"_s);
        note.body = u"<p>outside</p>"_s;
        QVERIFY(core::writeFileAtomic(m_session->library().notePath(note.meta.id),
                                      core::serializeNote(note)));
        m_editor->checkOutsideChanges();
        QVERIFY(text->toPlainText().endsWith(u" retained"_s));
        QVERIFY(!m_editor->saveNow());
        QCOMPARE(m_session->library().readNote(note.meta.id)->body, u"<p>outside</p>"_s);
    }

private:
    /// Returns the pill of a tag in the strip, or nullptr.
    static QWidget *pill(const editor::TagStrip *strip, const QString &tag)
    {
        for (QWidget *candidate : strip->findChildren<QWidget *>()) {
            if (candidate->property("tagPill").toBool() && candidate->accessibleName() == tag &&
                !candidate->isHidden()) {
                return candidate;
            }
        }
        return nullptr;
    }

    std::optional<ui::Theme> m_theme;
    std::optional<QTemporaryDir> m_dir;
    std::unique_ptr<core::LibrarySession> m_session;
    std::optional<editor::NoteEditor> m_editor;
};

QTEST_MAIN(NoteEditorTest)
#include "tst_noteeditor.moc"
