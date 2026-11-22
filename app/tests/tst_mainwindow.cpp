#include "app/CaptureWindow.h"
#include "app/MainWindow.h"
#include "app/NoteList.h"
#include "app/Settings.h"
#include "app/Sidebar.h"
#include "core/Files.h"
#include "core/LibrarySession.h"
#include "editor/NoteEditor.h"
#include "editor/TextEdit.h"
#include "ui/Popups.h"
#include "ui/Theme.h"

#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMimeData>
#include <QPushButton>
#include <QShortcut>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeView>

using namespace Qt::StringLiterals;

/// Checks note actions of the main window against a real library: creating notes in the
/// current collection, discarding a new note left empty, and the trash with Undo.
class MainWindowTest : public QObject
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
        m_settings.emplace();
        m_window.emplace(*m_settings);
        m_window->setSession(m_session.get());
    }

    void cleanup()
    {
        m_window.reset();
        m_settings.reset();
        m_session.reset();
        m_dir.reset();
    }

    /// A note created in a collection view belongs to that collection; left empty, it is
    /// deleted when another note opens.
    void newNoteInCollection()
    {
        core::Collection work{.id = core::newId(),
                              .parent = std::nullopt,
                              .name = u"Work"_s,
                              .icon = u"Folder"_s,
                              .color = std::nullopt,
                              .created = core::now(),
                              .updated = core::now(),
                              .extra = {}};
        QVERIFY(m_session->library().writeCollections({work}));
        m_session->refresh();
        QTRY_COMPARE(m_session->collections().size(), 1);
        m_window->findChild<Sidebar *>()->selectView(
            {.kind = View::Kind::Collection, .id = work.id});
        Q_EMIT m_window->findChild<Sidebar *>()->viewSelected(
            {.kind = View::Kind::Collection, .id = work.id});

        core::Note other = core::makeNote();
        other.meta.title = u"Other"_s;
        QVERIFY(m_session->saveNote(other));

        newNoteButton()->click();
        auto *editor = m_window->findChild<editor::NoteEditor *>();
        const QString created = editor->noteId();
        QVERIFY(!created.isEmpty() && created != other.meta.id);
        QCOMPARE(m_session->library().readNote(created)->meta.collections, QStringList{work.id});

        editor->openNote(other.meta.id);
        QVERIFY(!m_session->index().note(created).value().has_value());
        QVERIFY(m_session->index().note(other.meta.id).value().has_value());
    }

    /// Moving a note to the trash offers Undo, which brings it back.
    void trashAndUndo()
    {
        core::Note note = core::makeNote();
        note.meta.title = u"Draft"_s;
        QVERIFY(m_session->saveNote(note));
        Q_EMIT m_window->findChild<NoteList *>()->actionRequested(editor::NoteAction::Trash,
                                                                  {note.meta.id});
        QTRY_VERIFY(m_session->library().readNote(note.meta.id)->meta.trashed.has_value());

        auto *toast = m_window->findChild<ui::Toast *>();
        // The toast follows the operation's completion, which reaches the window after the
        // worker has written the file.
        QTRY_VERIFY(toast->isVisibleTo(m_window.operator->()));
        toast->findChild<QPushButton *>()->click();
        QTRY_VERIFY(!m_session->library().readNote(note.meta.id)->meta.trashed.has_value());
    }

    /// Dragging a collection onto another nests it; dropping notes on a collection files them;
    /// cancelling the name of a new collection removes it again.
    void sidebarCollections()
    {
        const auto work = m_session->createCollection(u"Work"_s, std::nullopt).value();
        const auto plans = m_session->createCollection(u"Plans"_s, std::nullopt).value();
        core::Note note = core::makeNote();
        QVERIFY(m_session->saveNote(note));

        auto *tree = m_window->findChild<Sidebar *>()->findChild<QTreeView *>();
        QAbstractItemModel *model = tree->model();
        const auto collectionIndex = [&](const QString &name) {
            const QModelIndexList found = model->match(model->index(0, 0), Qt::DisplayRole, name,
                                                       -1, Qt::MatchExactly | Qt::MatchRecursive);
            // The tree entry, not the favourites one: its parent is the Collections header.
            return found.isEmpty() ? QModelIndex() : found.last();
        };

        QMimeData collection;
        collection.setData(u"application/x-jotter-collection"_s, plans.id.toUtf8());
        model->dropMimeData(&collection, Qt::MoveAction, -1, 0, collectionIndex(u"Work"_s));
        QCOMPARE(m_session->collections().last().parent, std::optional<QString>(work.id));

        QMimeData notes;
        notes.setData(u"application/x-jotter-notes"_s, note.meta.id.toUtf8());
        model->dropMimeData(&notes, Qt::CopyAction, -1, 0, collectionIndex(u"Work"_s));
        QTRY_COMPARE(m_session->library().readNote(note.meta.id)->meta.collections,
                     QStringList{work.id});

        m_window->findChild<Sidebar *>()->createCollection(std::nullopt);
        QCOMPARE(m_session->collections().size(), 3);
        Q_EMIT tree->itemDelegate()->closeEditor(nullptr, QAbstractItemDelegate::RevertModelCache);
        QTRY_COMPARE(m_session->collections().size(), 2);
    }

    /// A tag renamed in the tree replaces the last part of its path in every note, and a typed
    /// slash nests it deeper.
    void renameTagInPlace()
    {
        core::Note note = core::makeNote();
        note.meta.tags = {u"work/meetings"_s};
        QVERIFY(m_session->saveNote(note));
        QAbstractItemModel *model =
            m_window->findChild<Sidebar *>()->findChild<QTreeView *>()->model();
        const auto tagIndex = [&](const QString &name) {
            return model
                ->match(model->index(0, 0), Qt::DisplayRole, name, 1,
                        Qt::MatchExactly | Qt::MatchRecursive)
                .value(0);
        };
        QVERIFY(model->setData(tagIndex(u"meetings"_s), u" #standups "_s));
        QTRY_COMPARE(m_session->library().readNote(note.meta.id)->meta.tags,
                     QStringList{u"work/standups"_s});
        QVERIFY(model->setData(tagIndex(u"standups"_s), u"daily/early"_s));
        QTRY_COMPARE(m_session->library().readNote(note.meta.id)->meta.tags,
                     QStringList{u"work/daily/early"_s});
    }

    /// A favourite dragged above another moves before it in the favourites.
    void reorderFavorites()
    {
        core::Note first = core::makeNote();
        first.meta.title = u"First"_s;
        core::Note second = core::makeNote();
        second.meta.title = u"Second"_s;
        QVERIFY(m_session->saveNote(first));
        QVERIFY(m_session->saveNote(second));
        QVERIFY(m_session->updateInfo([&](core::LibraryInfo &info) {
            info.favorites = {{.kind = core::FavoriteRef::Kind::Note, .id = first.meta.id},
                              {.kind = core::FavoriteRef::Kind::Note, .id = second.meta.id}};
        }));

        QAbstractItemModel *model =
            m_window->findChild<Sidebar *>()->findChild<QTreeView *>()->model();
        const QModelIndex header =
            model->match(model->index(0, 0), Qt::DisplayRole, u"Favourites"_s, 1, Qt::MatchExactly)
                .value(0);
        QVERIFY(header.isValid());
        QMimeData favorite;
        favorite.setData(u"application/x-jotter-favorite"_s,
                         (u"favorite:"_s + second.meta.id).toUtf8());
        model->dropMimeData(&favorite, Qt::MoveAction, 0, 0, header);
        QCOMPARE(m_session->library().info().favorites.first().id, second.meta.id);
    }

    /// Leaving a note with unsaved typing for a view without it saves the text once; saving
    /// refreshes the list, which must not close and save the note again and again.
    void leaveEditedNote()
    {
        core::Note note = core::makeNote();
        note.meta.title = u"Edited"_s;
        QVERIFY(m_session->saveNote(note));
        auto *editor = m_window->findChild<editor::NoteEditor *>();
        editor->openNote(note.meta.id);
        auto *text = editor->findChild<editor::TextEdit *>();
        QTest::keyClicks(text, u"unsaved"_s);

        Q_EMIT m_window->findChild<Sidebar *>()->viewSelected(
            {.kind = View::Kind::Archive, .id = {}});
        QVERIFY(editor->noteId().isEmpty());
        QCOMPARE(m_session->library().readNote(note.meta.id)->body, u"<p>unsaved</p>"_s);
    }

    /// A note created in the Unfiled notes view is an ordinary note and stays in that view.
    void newUnfiledNote()
    {
        Q_EMIT m_window->findChild<Sidebar *>()->viewSelected(
            {.kind = View::Kind::Unfiled, .id = {}});
        newNoteButton()->click();
        const QString created = m_window->findChild<editor::NoteEditor *>()->noteId();
        QVERIFY(!created.isEmpty());
        QVERIFY(m_session->library().readNote(created)->meta.extra.isEmpty());
        QCOMPARE(m_window->findChild<NoteList *>()->currentNote(), created);
    }

    /// The search field narrows the list after a pause in typing and shows No matches when
    /// nothing is found; a new note clears the search, so it stays in the list.
    void searchNarrowsTheList()
    {
        for (const QString &title : {u"Apples"_s, u"Pears"_s}) {
            core::Note note = core::makeNote();
            note.meta.title = title;
            note.body = u"<p>green %1</p>"_s.arg(title.toLower());
            QVERIFY(m_session->saveNote(note));
        }
        auto *list = m_window->findChild<NoteList *>();
        const QAbstractItemModel *rows = list->findChild<QListView *>()->model();
        QTRY_COMPARE(rows->rowCount(), 2);
        QLineEdit *field = nullptr;
        for (QLineEdit *edit : list->findChildren<QLineEdit *>()) {
            if (edit->accessibleName() == u"Search notes"_s) {
                field = edit;
            }
        }
        QVERIFY(field != nullptr);

        QTest::keyClicks(field, u"apple"_s);
        QTRY_COMPARE(rows->rowCount(), 1);
        field->setText(u"plums"_s);
        QTRY_COMPARE(rows->rowCount(), 0);
        const QList<QLabel *> labels =
            list->findChild<QStackedWidget *>()->currentWidget()->findChildren<QLabel *>();
        QVERIFY(std::ranges::any_of(
            labels, [](const QLabel *label) { return label->text() == u"No matches"_s; }));

        newNoteButton()->click();
        QVERIFY(field->text().isEmpty());
        QCOMPARE(rows->rowCount(), 3);
        QCOMPARE(list->currentNote(), m_window->findChild<editor::NoteEditor *>()->noteId());
    }

    /// A note file the index cannot read is announced once, with Show leading to the list of
    /// problems; a later scan without new problems stays quiet.
    void unreadableNotesAreAnnounced()
    {
        const QString broken = m_session->library().notePath(core::newId());
        QVERIFY(core::writeFileAtomic(broken, "not a note"));
        auto *toast = m_window->findChild<ui::Toast *>();
        QSignalSpy shown(m_window.operator->(), &MainWindow::problemsRequested);
        m_session->refresh();
        QTRY_VERIFY(toast->isVisibleTo(m_window.operator->()));
        QVERIFY(std::ranges::any_of(toast->findChildren<QLabel *>(), [](const QLabel *label) {
            // Without the English translation the plural form keeps its "(s)".
            return label->text().startsWith(u"1 note file"_s) &&
                   label->text().endsWith(u"could not be read."_s);
        }));
        toast->findChild<QPushButton *>()->click();
        QCOMPARE(shown.size(), 1);

        toast->hide();
        core::Note note = core::makeNote();
        note.meta.title = u"Fine"_s;
        QVERIFY(core::writeFileAtomic(m_session->library().notePath(note.meta.id),
                                      core::serializeNote(note)));
        QSignalSpy scanned(m_session.get(), &core::LibrarySession::notesChanged);
        m_session->refresh();
        QVERIFY(scanned.wait());
        QVERIFY(!toast->isVisibleTo(m_window.operator->()));
    }

    /// A rule that files a note announces it with Undo, which gives the note back its
    /// collections.
    void ruleUndo()
    {
        core::Collection work{.id = core::newId(),
                              .parent = std::nullopt,
                              .name = u"Work"_s,
                              .icon = u"Folder"_s,
                              .color = std::nullopt,
                              .created = core::now(),
                              .updated = core::now(),
                              .extra = {}};
        QVERIFY(m_session->library().writeCollections({work}));
        m_session->refresh();
        QTRY_COMPARE(m_session->collections().size(), 1);
        core::Rule rule;
        rule.id = core::newId();
        rule.name = u"Work"_s;
        rule.conditions = {{.kind = core::RuleCondition::Kind::TagEquals, .value = u"work"_s}};
        rule.action.collections = {work.id};
        QVERIFY(m_session->saveRules({rule}));

        core::Note note = core::makeNote();
        QVERIFY(m_session->saveNote(note));
        QVERIFY(m_session->changeTags({note.meta.id}, {u"work"_s}, {}));
        QCOMPARE(m_session->library().readNote(note.meta.id)->meta.collections,
                 QStringList{work.id});
        auto *undo = m_window->findChild<ui::Toast *>()->findChild<QPushButton *>();
        QCOMPARE(undo->text(), u"Undo"_s);
        undo->click();
        QTRY_VERIFY(m_session->library().readNote(note.meta.id)->meta.collections.isEmpty());
    }

    /// Focus mode shows only the editor; Escape, or the note closing, brings the panes back.
    void focusMode()
    {
        m_window->resize(1200, 700);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(&*m_window));
        newNoteButton()->click();
        auto *sidebar = m_window->findChild<Sidebar *>();
        auto *list = m_window->findChild<NoteList *>();
        pressShortcut(&*m_window, QKeySequence(u"Ctrl+Shift+D"_s));
        QVERIFY(!sidebar->isVisible() && !list->isVisible());
        QTest::keyClick(m_window->findChild<editor::TextEdit *>(), Qt::Key_Escape);
        QVERIFY(sidebar->isVisible() && list->isVisible());

        // Without its note focus mode ends, so another note can be chosen.
        pressShortcut(&*m_window, QKeySequence(u"Ctrl+Shift+D"_s));
        QVERIFY(!list->isVisible());
        m_window->findChild<editor::NoteEditor *>()->closeNote();
        QVERIFY(sidebar->isVisible() && list->isVisible());
    }

    /// Narrowed to one pane, the window keeps the open note in view, and after Back a click on
    /// that same note, which leaves the selection unchanged, shows it again (regression).
    void narrowLayoutShowsOpenNote()
    {
        m_window->resize(1200, 700);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(&*m_window));
        newNoteButton()->click();
        auto *list = m_window->findChild<NoteList *>();
        auto *editor = m_window->findChild<editor::NoteEditor *>();
        QTRY_VERIFY(!list->currentNote().isEmpty());

        m_window->resize(500, 700);
        QTRY_VERIFY(editor->isVisible());
        QVERIFY(!list->isVisible());

        QToolButton *back = nullptr;
        for (auto *button : editor->findChildren<QToolButton *>()) {
            if (button->accessibleName() == u"Back to the list"_s) {
                back = button;
            }
        }
        QVERIFY(back != nullptr);
        back->click();
        QVERIFY(list->isVisible() && !editor->isVisible());

        auto *view = list->findChild<QListView *>();
        const QModelIndex current = view->currentIndex();
        QVERIFY(current.isValid());
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, view->visualRect(current).center());
        QVERIFY(editor->isVisible() && !list->isVisible());
    }

    /// A list refreshed while Qt still reports Ctrl as held, as it does after Ctrl+C until the
    /// next input, keeps the open note selected and open (regression).
    void refreshWithCtrlHeld()
    {
        core::Note note = core::makeNote();
        note.meta.title = u"Kept"_s;
        QVERIFY(m_session->saveNote(note));
        auto *list = m_window->findChild<NoteList *>();
        auto *view = list->findChild<QListView *>();
        QTRY_COMPARE(view->model()->rowCount(), 1);
        view->setCurrentIndex(view->model()->index(0, 0));
        auto *editor = m_window->findChild<editor::NoteEditor *>();
        QCOMPARE(editor->noteId(), note.meta.id);
        auto *text = editor->findChild<editor::TextEdit *>();
        QTest::keyClicks(text, u"typed"_s);
        QTest::keyPress(text, Qt::Key_Control, Qt::ControlModifier);
        editor->saveNow();
        QTest::keyRelease(text, Qt::Key_Control);
        QCOMPARE(editor->noteId(), note.meta.id);
        QCOMPARE(list->currentNote(), note.meta.id);
    }

    /// The new note window creates an unfiled note, keeps it when written and discards it when
    /// left empty; Ctrl+Enter closes it.
    void captureWindow()
    {
        const auto unfiledNotes = [this] {
            return m_session->index()
                .count({.view = core::NoteFilter::View::Unfiled, .ids = {}, .tag = {}})
                .value_or(-1);
        };
        {
            CaptureWindow window(*m_session);
            QVERIFY(window.start());
            QCOMPARE(unfiledNotes(), 1);
            window.reject();
        }
        QCOMPARE(unfiledNotes(), 0);

        CaptureWindow window(*m_session);
        window.show();
        QVERIFY(window.start());
        QLineEdit *title = nullptr;
        for (QLineEdit *edit : window.findChildren<QLineEdit *>()) {
            if (edit->property("titleField").toBool()) {
                title = edit;
            }
        }
        QVERIFY(title != nullptr);
        QTest::keyClicks(title, u"Idea"_s);
        pressShortcut(&window, QKeySequence(u"Ctrl+Return"_s));
        QVERIFY(!window.isVisible());
        QCOMPARE(unfiledNotes(), 1);
    }

    /// Following note links opens their targets, a trashed one in the trash, and Alt+Left
    /// returns along the way.
    void followLinksAndBack()
    {
        core::Note first = core::makeNote();
        first.meta.title = u"First"_s;
        core::Note second = core::makeNote();
        second.meta.title = u"Second"_s;
        core::Note trashed = core::makeNote();
        trashed.meta.title = u"Gone"_s;
        trashed.meta.trashed = core::now();
        for (core::Note *note : {&first, &second, &trashed}) {
            QVERIFY(m_session->saveNote(*note));
        }
        auto *list = m_window->findChild<NoteList *>();
        auto *editor = m_window->findChild<editor::NoteEditor *>();
        QTRY_VERIFY(list->findChild<QListView *>()->model()->rowCount() == 2);
        list->selectNote(first.meta.id);
        Q_EMIT list->noteSelected(first.meta.id);
        QCOMPARE(editor->noteId(), first.meta.id);

        Q_EMIT editor->noteLinkActivated(second.meta.id);
        QCOMPARE(editor->noteId(), second.meta.id);
        Q_EMIT editor->noteLinkActivated(trashed.meta.id);
        QCOMPARE(editor->noteId(), trashed.meta.id);
        QCOMPARE(list->currentNote(), trashed.meta.id);

        pressShortcut(&*m_window, QKeySequence(Qt::ALT | Qt::Key_Left));
        QCOMPARE(editor->noteId(), second.meta.id);
        pressShortcut(&*m_window, QKeySequence(Qt::ALT | Qt::Key_Left));
        QCOMPARE(editor->noteId(), first.meta.id);
    }

private:
    /// Triggers the shortcut of a window bound to @p keys. Pressing the keys would reach only
    /// the active window, which the offscreen platform does not always grant in parallel test
    /// runs.
    static void pressShortcut(QWidget *window, const QKeySequence &keys)
    {
        for (QShortcut *shortcut : window->findChildren<QShortcut *>()) {
            if (shortcut->key() == keys) {
                Q_EMIT shortcut->activated();
                return;
            }
        }
        QFAIL("No such shortcut");
    }

    QToolButton *newNoteButton() const
    {
        for (QToolButton *button :
             m_window->findChild<NoteList *>()->findChildren<QToolButton *>()) {
            if (button->accessibleName() == u"New note"_s) {
                return button;
            }
        }
        return nullptr;
    }

    std::optional<ui::Theme> m_theme;
    std::optional<QTemporaryDir> m_dir;
    std::unique_ptr<core::LibrarySession> m_session;
    std::optional<Settings> m_settings;
    std::optional<MainWindow> m_window;
};

QTEST_MAIN(MainWindowTest)
#include "tst_mainwindow.moc"
