#include "core/Attachments.h"
#include "core/Files.h"
#include "editor/Commands.h"
#include "editor/Formats.h"
#include "editor/HtmlCodec.h"
#include "editor/Markdown.h"
#include "editor/Objects.h"
#include "editor/Presenter.h"
#include "editor/Syntax.h"
#include "editor/TextEdit.h"
#include "ui/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextTable>
#include <QTimer>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace {

/// Opens the clipboard functions of the editor to the tests.
class ClipboardEdit : public editor::TextEdit
{
public:
    using editor::TextEdit::contextMenuEvent;
    using editor::TextEdit::createMimeDataFromSelection;
    using editor::TextEdit::insertFromMimeData;
};

/// Reads a body into a fresh document and writes it back.
QString roundTrip(const QString &html)
{
    QTextDocument document;
    editor::readBody(document, html);
    return editor::writeBody(document);
}

} // namespace

/// Checks the editor's document model: the HTML codec reads and writes the note vocabulary
/// without loss, and list commands give the nesting the codec writes.
class EditorTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    /// Every form of the vocabulary survives a round trip unchanged.
    void roundTrip_data()
    {
        QTest::addColumn<QString>("html");
        QTest::newRow("blocks")
            << u"<h1>Title</h1>\n<p>Text</p>\n<h2>Sub</h2>\n<h3>Small</h3>\n<p></p>"_s;
        QTest::newRow("inline") << u"<p>a <strong>bold</strong> <em>it</em> <u>u</u> <s>s</s> "
                                   u"x<sup>2</sup> H<sub>2</sub>O <code>code</code></p>"_s;
        QTest::newRow("nested inline") << u"<p><strong><em>both</em></strong> <em>one</em></p>"_s;
        QTest::newRow("colours and font")
            << u"<p><span style=\"font-family: 'Noto Serif'\"><span data-color=\"red\">"
               u"<mark data-color=\"yellow\">x</mark></span></span></p>"_s;
        QTest::newRow("custom colours")
            << u"<p><span data-color=\"#ff8800\" style=\"color: #ff8800\"><mark "
               u"data-color=\"#0000ff\" style=\"background: #b3b3ff\">x</mark></span></p>"_s;
        QTest::newRow("link") << u"<p>see <a href=\"https://example.com/?a=1&amp;b=2\">site</a>"
                                 u"</p>"_s;
        QTest::newRow("note link")
            << u"<p>see <a class=\"note-link\" href=\"0192f3a4-5b6c-7d8e-9f01-23456789abcd.html\">"
               "Plan</a></p>"_s;
        QTest::newRow("line break") << u"<p>one<br>two</p>"_s;
        QTest::newRow("escaping") << u"<p>a &lt; b &amp; c &gt; d \"q\"</p>"_s;
        QTest::newRow("lists") << u"<ul>\n<li>one\n<ol start=\"3\">\n<li>three</li>\n<li>four\n"
                                  u"<ul class=\"checklist\">\n<li data-checked=\"true\">done</li>\n"
                                  u"<li data-checked=\"false\">todo</li>\n</ul>\n</li>\n</ol>\n"
                                  u"</li>\n<li>two</li>\n</ul>\n<p>after</p>"_s;
        QTest::newRow("adjacent lists") << u"<ul>\n<li>a</li>\n</ul>\n<ol>\n<li>b</li>\n</ol>"_s;
        QTest::newRow("quote") << u"<blockquote>\n<p>q1</p>\n<p>q2</p>\n</blockquote>\n<p>x</p>"_s;
        QTest::newRow("divider") << u"<p>a</p>\n<hr>\n<p>b</p>"_s;
        QTest::newRow("code") << u"<p>a</p>\n<pre data-language=\"go\" data-title=\"main &amp; "
                                 u"&quot;x&quot;\"><code>func main() {\n    x := 1 &lt; 2\n\n}"
                                 u"</code></pre>\n<h2>b</h2>"_s;
        QTest::newRow("code alone") << u"<pre><code>x</code></pre>"_s;
        QTest::newRow("empty code") << u"<p>a</p>\n<pre><code></code></pre>"_s;
        QTest::newRow("code between lists")
            << u"<ul>\n<li>a</li>\n</ul>\n<pre><code>x</code></pre>\n<ol>\n<li>b</li>\n</ol>"_s;
        QTest::newRow("numbering style")
            << u"<ol data-style=\"outline\" start=\"3\">\n<li>a\n<ol data-style=\"0192f3a4-5b6c-"
               u"7d8e-9f01-23456789abcd\">\n<li>b</li>\n</ol>\n</li>\n</ol>"_s;
        QTest::newRow("table")
            << u"<p>a</p>\n<table>\n<thead>\n<tr><th><p>H1</p></th><th><p><em>H2</em></p></th></"
               u"tr>\n"
               u"</thead>\n<tbody>\n<tr><td><p>x</p>\n<p>y</p></td><td><ul>\n<li>i</li>\n</ul></td>"
               u"</tr>\n<tr><td><p></p></td><td><p>z</p></td></tr>\n</tbody>\n</table>\n<p>b</p>"_s;
        QTest::newRow("table alone") << u"<table>\n<tbody>\n<tr><td><p>x</p></td></tr>\n</tbody>\n"
                                        u"</table>"_s;
        QTest::newRow("objects")
            << u"<p>see <img src=\"../attachments/n1/a%20b.png\" data-width=\"50\"><img "
               u"src=\"../attachments/n1/a%20b.png\"> and <a class=\"attachment\" "
               u"href=\"../attachments/n1/r.pdf\" data-size=\"1234\">Report &amp; notes.pdf</a>"
               u"</p>"_s;
        QTest::newRow("image in a cell")
            << u"<table>\n<tbody>\n<tr><td><p><img src=\"../attachments/n1/x.png\"></p></td>"
               u"</tr>\n</tbody>\n</table>"_s;
        QTest::newRow("column widths")
            << u"<table>\n<colgroup><col style=\"width: 30%\"><col style=\"width: "
               u"70%\"></colgroup>\n"
               u"<tbody>\n<tr><td><p>a</p></td><td><p>b</p></td></tr>\n</tbody>\n</table>"_s;
        QTest::newRow("image width")
            << u"<p><img src=\"../attachments/n1/x.png\" data-width=\"37\"></p>"_s;
        QTest::newRow("image wrapping")
            << u"<p><img src=\"../attachments/n1/x.png\" data-width=\"40\" data-wrap=\"square\">"
               u"text <img src=\"../attachments/n1/y.png\" data-wrap=\"top-bottom\"></p>"_s;
        QTest::newRow("table width")
            << u"<table data-width=\"60\">\n<tbody>\n<tr><td><p>a</p></td></tr>\n</tbody>\n"
               u"</table>"_s;
        QTest::newRow("code after quote and empty paragraph")
            << u"<blockquote>\n<p>q</p>\n</blockquote>\n<pre><code>x</code></pre>\n<p></p>\n"
               u"<pre><code>y</code></pre>"_s;
    }

    void roundTrip()
    {
        QFETCH(QString, html);
        QCOMPARE(::roundTrip(html), html);
    }

    /// Other spellings and unknown elements read into the canonical form; text is never lost.
    void normalises_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("output");
        QTest::newRow("b and i") << u"<p><b>x</b><i>y</i></p>"_s
                                 << u"<p><strong>x</strong><em>y</em></p>"_s;
        QTest::newRow("unknown element") << u"<p><font>x</font> y</p>"_s << u"<p>x y</p>"_s;
        QTest::newRow("heading 5") << u"<h5>x</h5>"_s << u"<h3>x</h3>"_s;
        QTest::newRow("bare text") << u"just text"_s << u"<p>just text</p>"_s;
        QTest::newRow("empty") << QString() << u"<p></p>"_s;
        QTest::newRow("bare cells")
            << u"<table><tr><td>a<td>b</table>"_s
            << u"<table>\n<tbody>\n<tr><td><p>a</p></td><td><p>b</p></td></tr>"
               u"\n</tbody>\n</table>"_s;
        QTest::newRow("heading in a cell")
            << u"<table><tr><td><h2>a</h2><pre><code>b</code></pre></td></tr></table>"_s
            << u"<table>\n<tbody>\n<tr><td><p>a</p>\n<p><code>b</code></p></td></tr>\n</tbody>\n"
               u"</table>"_s;
        QTest::newRow("default style")
            << u"<ol data-style=\"decimal\"><li>a</li></ol>"_s << u"<ol>\n<li>a</li>\n</ol>"_s;
        QTest::newRow("foreign image")
            << u"<p><img src=\"https://example.com/a.png\">x</p>"_s << u"<p>x</p>"_s;
        QTest::newRow("equal widths")
            << u"<table><colgroup><col style=\"width:50%\"><col style=\"width:50%\"></colgroup>"
               u"<tr><td>a</td><td>b</td></tr></table>"_s
            << u"<table>\n<tbody>\n<tr><td><p>a</p></td><td><p>b</p></td></tr>\n</tbody>\n"
               u"</table>"_s;
        QTest::newRow("widths in another scale")
            << u"<table><colgroup><col style=\"width: 10%\"><col style=\"width: 30%\"></colgroup>"
               u"<tr><td>a</td><td>b</td></tr></table>"_s
            << u"<table>\n<colgroup><col style=\"width: 25%\"><col style=\"width: 75%\">"
               u"</colgroup>\n<tbody>\n<tr><td><p>a</p></td><td><p>b</p></td></tr>\n</tbody>\n"
               u"</table>"_s;
        QTest::newRow("unsafe links")
            << u"<p><a href=\"javascript:x()\">a</a> <a href=\"file:///x.desktop\">b</a> "
               u"<a href=\"https://a.org\">c</a> <a href=\"mailto:me@a.org\">d</a></p>"_s
            << u"<p>a b <a href=\"https://a.org\">c</a> <a href=\"mailto:me@a.org\">d</a></p>"_s;
        QTest::newRow("image too wide") << u"<p><img src=\"../attachments/n/x.png\" "
                                           u"data-width=\"150\"></p>"_s
                                        << u"<p><img src=\"../attachments/n/x.png\"></p>"_s;
    }

    /// Pasted HTML keeps structure and basic formatting, also from style attributes, and drops
    /// fonts, colours, foreign links and what is not content.
    void pastes_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("output");
        QTest::newRow("google docs")
            << u"<meta charset=\"utf-8\"><b style=\"font-weight:normal;\" id=\"x\"><p><span "
               u"style=\"font-size:11pt;font-family:Arial;color:#ff0000;font-weight:700\">bold"
               u"</span><span style=\"font-style:italic\"> it</span></p></b>"_s
            << u"<p><strong>bold</strong><em> it</em></p>"_s;
        QTest::newRow("fonts and colours")
            << u"<p><span style=\"font-family: 'Comic'\" data-color=\"red\">a</span><mark "
               u"data-color=\"yellow\">b</mark><font color=\"red\">c</font></p>"_s
            << u"<p>abc</p>"_s;
        QTest::newRow("links")
            << u"<p><a href=\"https://a.org\">a</a> <a href=\"page.html\">b</a> <a "
               u"href=\"javascript:x()\">c</a></p>"_s
            << u"<p><a href=\"https://a.org\">a</a> b c</p>"_s;
        QTest::newRow("white space") << u"<html><head><style>p{x:1}</style><title>t</title></head>"
                                        u"<body><p>\n  one\n  two  </p></body></html>"_s
                                     << u"<p>one two </p>"_s;
        QTest::newRow("header cells")
            << u"<table><tr><th>A</th><th>B</th></tr><tr><td>1</td><td>2</td></tr></table>"_s
            << u"<table>\n<thead>\n<tr><th><p>A</p></th><th><p>B</p></th></tr>\n</thead>\n<tbody>\n"
               u"<tr><td><p>1</p></td><td><p>2</p></td></tr>\n</tbody>\n</table>"_s;
        QTest::newRow("web image")
            << u"<p><img src=\"https://a.org/x.png\" alt=\"A cat\"></p>"_s << u"<p>A cat</p>"_s;
        QTest::newRow("numbering style")
            << u"<ol data-style=\"outline\"><li>a</li></ol>"_s << u"<ol>\n<li>a</li>\n</ol>"_s;
    }

    void pastes()
    {
        QFETCH(QString, input);
        QFETCH(QString, output);
        QTextDocument document;
        editor::readBody(document, input, editor::ReadMode::Paste);
        QCOMPARE(editor::writeBody(document), output);
    }

    /// Pasted pictures become pending objects for the editor to store; a chip read from a
    /// note keeps its file's name apart from the name it shows.
    void objects()
    {
        QTextDocument document;
        editor::readBody(document, u"<p><img src=\"data:image/png;base64,AAAA\"></p>"_s,
                         editor::ReadMode::Paste);
        QTextCursor cursor(&document);
        cursor.movePosition(QTextCursor::NextCharacter);
        QCOMPARE(cursor.charFormat().objectType(), int(editor::ImageObject));
        QCOMPARE(cursor.charFormat().stringProperty(editor::PendingSource),
                 u"data:image/png;base64,AAAA"_s);

        editor::readBody(document, u"<p><a class=\"attachment\" href=\"../attachments/n/f%23.zip\" "
                                   u"data-size=\"7\">Shown</a></p>"_s);
        cursor = QTextCursor(&document);
        cursor.movePosition(QTextCursor::NextCharacter);
        QCOMPARE(cursor.charFormat().stringProperty(editor::ObjectFile), u"f#.zip"_s);
        QCOMPARE(cursor.charFormat().stringProperty(editor::AttachmentName), u"Shown"_s);
    }

    void normalises_code()
    {
        QCOMPARE(::roundTrip(u"<pre data-language=\"Go\">\n<code><b>x</b>\r\ny</code></pre>"_s),
                 u"<pre data-language=\"go\"><code>x\ny</code></pre>"_s);
        QCOMPARE(::roundTrip(u"<pre><code>a</code></pre><pre><code>b</code></pre>"_s),
                 u"<pre><code>a</code></pre>\n<p></p>\n<pre><code>b</code></pre>"_s);
    }

    void normalises()
    {
        QFETCH(QString, input);
        QFETCH(QString, output);
        QCOMPARE(::roundTrip(input), output);
    }

    /// Tab nests a list item under the previous one; Shift+Tab on a first-level item takes it
    /// out of the list; Enter-style list toggling joins neighbouring items.
    void listLevels()
    {
        QTextDocument document;
        editor::readBody(document, u"<p>a</p>\n<p>b</p>\n<p>c</p>"_s);
        QTextCursor cursor(&document);
        cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        editor::toggleList(cursor, editor::ListKind::Bulleted);
        QCOMPARE(editor::writeBody(document), u"<ul>\n<li>a</li>\n<li>b</li>\n<li>c</li>\n</ul>"_s);

        QTextCursor second(document.findBlockByNumber(1));
        QVERIFY(editor::changeListLevel(second, 1));
        QCOMPARE(editor::writeBody(document),
                 u"<ul>\n<li>a\n<ul>\n<li>b</li>\n</ul>\n</li>\n<li>c</li>\n</ul>"_s);

        QTextCursor third(document.findBlockByNumber(2));
        QVERIFY(editor::changeListLevel(third, -1));
        QCOMPARE(editor::writeBody(document),
                 u"<ul>\n<li>a\n<ul>\n<li>b</li>\n</ul>\n</li>\n</ul>\n<p>c</p>"_s);
    }

    /// A nested list changes kind on its own, and a checklist item can be checked.
    void nestedKindAndChecklist()
    {
        QTextDocument document;
        editor::readBody(document, u"<ul>\n<li>a\n<ul>\n<li>b</li>\n</ul>\n</li>\n</ul>"_s);
        QTextCursor nested(document.findBlockByNumber(1));
        editor::toggleList(nested, editor::ListKind::Checklist);
        QVERIFY(editor::toggleChecked(nested));
        QCOMPARE(editor::writeBody(document),
                 u"<ul>\n<li>a\n<ul class=\"checklist\">\n<li data-checked=\"true\">b</li>\n</ul>\n"
                 u"</li>\n</ul>"_s);
    }

    /// Markdown shortcuts typed at the start of a paragraph change its kind.
    void markdownShortcuts_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("html");
        QTest::newRow("dash") << u"- a"_s << u"<ul>\n<li>a</li>\n</ul>"_s;
        QTest::newRow("star") << u"* a"_s << u"<ul>\n<li>a</li>\n</ul>"_s;
        QTest::newRow("one") << u"1. a"_s << u"<ol>\n<li>a</li>\n</ol>"_s;
        QTest::newRow("seven") << u"7. a\nb"_s
                               << u"<ol start=\"7\">\n<li>a</li>\n<li>b</li>\n</ol>"_s;
        QTest::newRow("checklist") << u"[ ] a"_s
                                   << u"<ul class=\"checklist\">\n<li data-checked=\"false\">a</li>"
                                      u"\n</ul>"_s;
        QTest::newRow("heading") << u"## a"_s << u"<h2>a</h2>"_s;
        QTest::newRow("quote") << u"> a"_s << u"<blockquote>\n<p>a</p>\n</blockquote>"_s;
        QTest::newRow("divider") << u"---\na"_s << u"<hr>\n<p>a</p>"_s;
        QTest::newRow("long divider") << u"-----\na"_s << u"<hr>\n<p>a</p>"_s;
        QTest::newRow("two dashes") << u"--\na"_s << u"<p>--</p>\n<p>a</p>"_s;
        QTest::newRow("mid-line") << u"a - b 1. c"_s << u"<p>a - b 1. c</p>"_s;
        QTest::newRow("same list kind") << u"- - a"_s << u"<ul>\n<li>- a</li>\n</ul>"_s;
        QTest::newRow("nested kind")
            << u"- a\n\t1. b"_s << u"<ul>\n<li>a\n<ol>\n<li>b</li>\n</ol>\n</li>\n</ul>"_s;
    }

    void markdownShortcuts()
    {
        QFETCH(QString, typed);
        QFETCH(QString, html);
        editor::TextEdit edit;
        for (const QChar c : typed) {
            if (c == u'\n') {
                QTest::keyClick(&edit, Qt::Key_Return);
            } else if (c == u'\t') {
                QTest::keyClick(&edit, Qt::Key_Tab);
            } else if (c == u' ') {
                QTest::keyClick(&edit, Qt::Key_Space, Qt::NoModifier);
            } else {
                QTest::keyClicks(&edit, QString(c));
            }
        }
        QCOMPARE(editor::writeBody(*edit.document()), html);
    }

    /// A code block takes an empty paragraph's place or follows the cursor's paragraph, keeps
    /// the format of the paragraph after it, and is left and removed with the keyboard.
    void codeBlocks()
    {
        QTextDocument document;
        editor::readBody(document, u"<p>a</p>\n<p></p>\n<h2>b</h2>"_s);
        QTextCursor cursor(document.findBlockByNumber(1));
        QVERIFY(editor::insertCodeBlock(cursor, u"go"_s) != nullptr);
        cursor.insertText(u"x"_s);
        QCOMPARE(editor::writeBody(document),
                 u"<p>a</p>\n<pre data-language=\"go\"><code>x</code></pre>\n<h2>b</h2>"_s);
        QVERIFY(editor::insertCodeBlock(cursor) == nullptr);

        QVERIFY(editor::leaveCodeBlock(cursor));
        cursor.insertText(u"c"_s);
        QCOMPARE(editor::writeBody(document),
                 u"<p>a</p>\n<pre data-language=\"go\"><code>x</code></pre>\n<p>c</p>\n"
                 u"<h2>b</h2>"_s);

        editor::readBody(document, u"<p>a</p>\n<pre><code></code></pre>\n<h2>b</h2>"_s);
        QTextCursor empty(document.findBlockByNumber(1));
        QVERIFY(editor::removeEmptyCodeBlock(empty));
        empty.insertText(u"c"_s);
        QCOMPARE(editor::writeBody(document), u"<p>a</p>\n<p>c</p>\n<h2>b</h2>"_s);

        editor::readBody(document, u"<p>one</p>\n<ul>\n<li>two</li>\n</ul>"_s);
        QTextCursor selection(&document);
        selection.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        editor::insertCodeBlock(selection);
        QCOMPARE(editor::writeBody(document), u"<pre><code>one\ntwo</code></pre>"_s);
    }

    /// A code block takes selected characters only, leaving edge line breaks and unselected
    /// text outside, including soft line breaks; an existing frame cannot be converted.
    void codeBlockFromSelection()
    {
        QTextDocument document;
        const auto select = [&document](int from, int to) {
            QTextCursor cursor(&document);
            cursor.setPosition(from);
            cursor.setPosition(to, QTextCursor::KeepAnchor);
            return cursor;
        };
        const auto blockAt = [&document](int number) { return document.findBlockByNumber(number); };

        editor::readBody(document, u"<p>above</p>\n<p>one</p>\n<p>two</p>\n<p>below</p>"_s);
        QTextCursor edges = select(blockAt(0).position() + 5, blockAt(3).position());
        QVERIFY(editor::insertCodeBlock(edges) != nullptr);
        QCOMPARE(editor::writeBody(document),
                 u"<p>above</p>\n<pre><code>one\ntwo</code></pre>\n<p>below</p>"_s);

        editor::readBody(document, u"<p>alpha<br>beta<br><br>one<br>two</p>"_s);
        const int one =
            blockAt(0).position() + static_cast<int>(blockAt(0).text().indexOf(u"one"_s));
        QTextCursor line = select(one + 1, one + 2);
        QVERIFY(editor::insertCodeBlock(line) != nullptr);
        QCOMPARE(editor::writeBody(document),
                 u"<p>alpha<br>beta<br><br>o</p>\n<pre><code>n</code></pre>\n<p>e<br>two</p>"_s);

        const QString formatted = u"<p><strong>before code after</strong></p>"_s;
        editor::readBody(document, formatted);
        QTextCursor fragment = select(11, 7); // A backward selection inside one rich paragraph.
        QVERIFY(editor::insertCodeBlock(fragment) != nullptr);
        QCOMPARE(editor::writeBody(document),
                 u"<p><strong>before </strong></p>\n<pre><code>code</code></pre>\n"
                 u"<p><strong> after</strong></p>"_s);
        document.undo();
        QCOMPARE(editor::writeBody(document), formatted);

        // A selection reaching into a code block takes all of it (Qt widens it to the frame).
        const QString afterCode = u"<pre><code>a</code></pre>\n<p>b</p>"_s;
        editor::readBody(document, afterCode);
        QTextCursor overCode = select(blockAt(1).position(), document.characterCount() - 1);
        QVERIFY(!editor::canInsertCodeBlock(overCode));
        QVERIFY(editor::insertCodeBlock(overCode) == nullptr);
        QCOMPARE(editor::writeBody(document), afterCode);

        // Paragraph styles follow the same rule: a triple-clicked line keeps the next one out.
        editor::readBody(document, u"<p>one</p>\n<p>two</p>"_s);
        editor::setBlockStyle(select(0, blockAt(1).position()), editor::BlockStyle::Heading2);
        QCOMPARE(editor::writeBody(document), u"<h2>one</h2>\n<p>two</p>"_s);
    }

    /// The toolbar code command distinguishes logical lines from inline fragments and wrapping.
    void codeFormatting_data()
    {
        QTest::addColumn<QString>("body");
        QTest::addColumn<int>("start");
        QTest::addColumn<int>("end");
        QTest::addColumn<QString>("expected");
        QTest::newRow("fragment") << u"<p>before  code  after</p>"_s << 8 << 12
                                  << u"<p>before  <code>code</code>  after</p>"_s;
        QTest::newRow("start fragment")
            << u"<p>code after</p>"_s << 0 << 4 << u"<p><code>code</code> after</p>"_s;
        QTest::newRow("end fragment")
            << u"<p>before code</p>"_s << 7 << 11 << u"<p>before <code>code</code></p>"_s;
        QTest::newRow("preserve emphasis")
            << u"<p><strong>before code after</strong></p>"_s << 7 << 11
            << u"<p><strong>before </strong><strong><code>code</code></strong><strong> after</strong></p>"_s;
        QTest::newRow("whole line")
            << u"<p>code</p>"_s << 0 << 4 << u"<pre><code>code</code></pre>"_s;
        QTest::newRow("triple click") << u"<p>code</p>\n<p>after</p>"_s << 0 << 5
                                      << u"<pre><code>code</code></pre>\n<p>after</p>"_s;
        QTest::newRow("multiple lines")
            << u"<p>one</p>\n<p>two</p>"_s << 0 << 7 << u"<pre><code>one\ntwo</code></pre>"_s;
        QTest::newRow("partial multiple lines")
            << u"<p>abc</p>\n<p>def</p>"_s << 1 << 6
            << u"<p>a</p>\n<pre><code>bc\nde</code></pre>\n<p>f</p>"_s;
        QTest::newRow("soft line fragment") << u"<p>abc<br>def<br>ghi</p>"_s << 5 << 6
                                            << u"<p>abc<br>d<code>e</code>f<br>ghi</p>"_s;
        QTest::newRow("whole soft line")
            << u"<p>abc<br>def<br>ghi</p>"_s << 4 << 7
            << u"<p>abc</p>\n<pre><code>def</code></pre>\n<p>ghi</p>"_s;
        QTest::newRow("soft line edge breaks")
            << u"<p>abc<br>def<br>ghi</p>"_s << 3 << 8
            << u"<p>abc</p>\n<pre><code>def</code></pre>\n<p>ghi</p>"_s;
        QTest::newRow("soft multiple lines")
            << u"<p>abc<br>def<br>ghi</p>"_s << 1 << 6
            << u"<p>a</p>\n<pre><code>bc\nde</code></pre>\n<p>f<br>ghi</p>"_s;
        QTest::newRow("no selection")
            << u"<p>text</p>"_s << 2 << 2 << u"<p>text</p>\n<pre><code></code></pre>"_s;
    }

    void codeFormatting()
    {
        QFETCH(QString, body);
        QFETCH(int, start);
        QFETCH(int, end);
        QFETCH(QString, expected);
        for (const bool backward : {false, true}) {
            QTextDocument document;
            editor::readBody(document, body);
            QTextCursor cursor(&document);
            cursor.setPosition(backward ? end : start);
            cursor.setPosition(backward ? start : end, QTextCursor::KeepAnchor);
            QVERIFY(editor::formatCode(cursor));
            QCOMPARE(editor::writeBody(document), expected);
            QCOMPARE(::roundTrip(expected), expected);
            document.undo();
            QCOMPARE(editor::writeBody(document), body);
            document.redo();
            QCOMPARE(editor::writeBody(document), expected);
        }
    }

    /// Selecting a visually wrapped line is still an inline fragment of its logical line.
    void codeFormattingWithWrapping()
    {
        QTextDocument document;
        const QString plain = u"word "_s.repeated(20).trimmed();
        document.setPlainText(plain);
        document.setTextWidth(100);
        document.documentLayout()->documentSize();
        const QTextLayout *layout = document.begin().layout();
        QVERIFY(layout->lineCount() > 1);
        const int end = layout->lineAt(0).textLength();
        QTextCursor cursor(&document);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        QVERIFY(editor::formatCode(cursor));
        QCOMPARE(document.toPlainText(), plain);
        QVERIFY(document.rootFrame()->childFrames().isEmpty());
        QVERIFY(cursor.charFormat().boolProperty(editor::InlineCode));
        document.undo();
        QCOMPARE(editor::writeBody(document), u"<p>"_s + plain + u"</p>"_s);
    }

    /// A list takes a numbering style as a whole, and Start at… on a later item splits the list
    /// there, so the numbers before stay. Markers count as the file
    /// nests the lists.
    void numbering()
    {
        QTextDocument document;
        editor::readBody(document, u"<ol>\n<li>a</li>\n<li>b\n<ul>\n<li>x</li>\n</ul>\n</li>\n"
                                   u"<li>c</li>\n</ol>"_s);
        editor::setNumberingStyle(QTextCursor(document.findBlockByNumber(0)), u"multilevel"_s);
        QVERIFY(editor::setListStart(QTextCursor(document.findBlockByNumber(3)), 5));
        QCOMPARE(editor::writeBody(document),
                 u"<ol data-style=\"multilevel\">\n<li>a</li>\n<li>b\n<ul>\n<li>x</li>\n</ul>\n"
                 u"</li>\n</ol>\n<ol data-style=\"multilevel\" start=\"5\">\n<li>c</li>\n</ol>"_s);
        QVERIFY(!editor::setListStart(QTextCursor(document.findBlockByNumber(2)), 1));

        editor::readBody(document, u"<ol data-style=\"multilevel\" start=\"2\">\n<li>a\n<ol "
                                   u"data-style=\"multilevel\">\n<li>b</li>\n<li>c</li>\n</ol>\n"
                                   u"</li>\n<li>d</li>\n</ol>\n<ol>\n<li>e</li>\n</ol>"_s);
        const QHash<int, QString> markers = editor::listMarkers(document);
        QCOMPARE(markers.value(0), u"2."_s);
        QCOMPARE(markers.value(1), u"2.1."_s);
        QCOMPARE(markers.value(2), u"2.2."_s);
        QCOMPARE(markers.value(3), u"3."_s);
        QCOMPARE(markers.value(4), u"1."_s);
    }

    /// A code block or a table inserted from the second of two empty lines takes its place
    /// there, not inside the text above (regression).
    void frameAfterEmptyLines()
    {
        const QString start = u"<p>text</p>\n<p></p>\n<pre><code>a</code></pre>\n<p></p>"_s;
        QTextDocument document;
        editor::readBody(document, start + u"\n<p></p>"_s);
        QTextCursor code(document.lastBlock());
        QVERIFY(editor::insertCodeBlock(code) != nullptr);
        QCOMPARE(editor::writeBody(document), start + u"\n<pre><code></code></pre>"_s);

        editor::readBody(document, start + u"\n<p></p>"_s);
        QTextCursor table(document.lastBlock());
        QVERIFY(editor::insertTable(table, 1, 1, false) != nullptr);
        QVERIFY(editor::writeBody(document).startsWith(start + u"\n<table>"_s));
    }

    /// A table takes an empty paragraph's place, Tab walks its cells and adds a row at the end,
    /// and its rows, columns and header change as one undo step each.
    void tables()
    {
        QTextDocument document;
        editor::readBody(document, u"<p>a</p>\n<p></p>\n<p>b</p>"_s);
        QTextCursor cursor(document.findBlockByNumber(1));
        QVERIFY(editor::insertTable(cursor, 1, 2, true) != nullptr);
        cursor.insertText(u"h"_s);
        QVERIFY(editor::insertTable(cursor, 1, 1, false) == nullptr);
        QCOMPARE(editor::writeBody(document),
                 u"<p>a</p>\n<table>\n<thead>\n<tr><th><p>h</p></th><th><p></p></th></tr>\n"
                 u"</thead>\n</table>\n<p>b</p>"_s);

        QVERIFY(editor::moveToCell(cursor, 1));
        QVERIFY(editor::moveToCell(cursor, 1));
        cursor.insertText(u"x"_s);
        QVERIFY(editor::changeTable(cursor, editor::TableChange::ColumnRight));
        cursor.insertText(u"y"_s);
        QCOMPARE(editor::writeBody(document),
                 u"<p>a</p>\n<table>\n<thead>\n<tr><th><p>h</p></th><th><p></p></th><th><p></p>"
                 u"</th></tr>\n</thead>\n<tbody>\n<tr><td><p>x</p></td><td><p>y</p></td><td><p></p>"
                 u"</td></tr>\n</tbody>\n</table>\n<p>b</p>"_s);
        QCOMPARE(cursor.currentTable()->format().columnWidthConstraints().size(), 3);

        QVERIFY(editor::changeTable(cursor, editor::TableChange::ToggleHeader));
        QVERIFY(editor::changeTable(cursor, editor::TableChange::DeleteColumn));
        QVERIFY(editor::changeTable(cursor, editor::TableChange::DeleteRow));
        QCOMPARE(editor::writeBody(document),
                 u"<p>a</p>\n<table>\n<tbody>\n<tr><td><p>h</p></td><td><p></p></td></tr>\n"
                 u"</tbody>\n</table>\n<p>b</p>"_s);
        document.undo();
        QVERIFY(editor::writeBody(document).contains(u"<td><p>x</p></td>"));

        QTextCursor last(document.findBlockByNumber(1));
        QVERIFY(editor::changeTable(last, editor::TableChange::DeleteTable));
        QCOMPARE(editor::writeBody(document), u"<p>a</p>\n<p></p>\n<p>b</p>"_s);
    }

    /// Column widths keep whole percents adding up to 100 with a minimum per column; one column
    /// set by Column width… takes its share from the others in their proportions.
    void columnWidths()
    {
        QTextTableFormat format;
        editor::setColumnWidths(format, 3, {1, 1, 98});
        QCOMPARE(editor::columnWidths(format, 3), (QList<qreal>{5, 5, 90}));
        QCOMPARE(editor::withColumnWidth({20, 30, 50}, 0, 60), (QList<qreal>{60, 15, 25}));
        QVERIFY(editor::hasEqualColumns(editor::tableFormat(3, 0), 3));
    }

    /// An empty paragraph right before a table or a code block keeps its room above it, so its
    /// cursor is not drawn over the frame.
    void emptyParagraphBeforeFrames()
    {
        for (const QString &frame : {u"<table>\n<tbody>\n<tr><td><p>a</p></td></tr>\n</tbody>\n"
                                     u"</table>"_s,
                                     u"<pre><code>a</code></pre>"_s}) {
            QTextDocument document;
            document.setDocumentMargin(0);
            document.setTextWidth(400);
            editor::readBody(document, u"<p>x</p>\n<p></p>\n"_s + frame);
            const QAbstractTextDocumentLayout *layout = document.documentLayout();
            const QRectF empty = layout->blockBoundingRect(document.findBlockByNumber(1));
            const QRectF inside = layout->blockBoundingRect(document.findBlockByNumber(2));
            QVERIFY2(inside.top() >= empty.bottom(), qPrintable(frame));
        }
    }

    /// Dragging the line between two columns changes their widths as one undo step.
    void dragColumnLine()
    {
        editor::TextEdit edit;
        edit.document()->setDocumentMargin(0);
        edit.resize(600, 300);
        edit.show();
        editor::readBody(*edit.document(), u"<table>\n<tbody>\n<tr><td><p>a</p></td><td><p>b</p>"
                                           u"</td></tr>\n</tbody>\n</table>"_s);
        auto *table =
            qobject_cast<QTextTable *>(edit.document()->rootFrame()->childFrames().first());
        const QAbstractTextDocumentLayout *layout = edit.document()->documentLayout();
        const QRectF left =
            layout->blockBoundingRect(table->cellAt(0, 0).firstCursorPosition().block());
        const QRectF right =
            layout->blockBoundingRect(table->cellAt(0, 1).firstCursorPosition().block());
        const qreal padding = table->format().cellPadding();
        const QPoint line(static_cast<int>(std::round(right.left() - padding)),
                          static_cast<int>(left.center().y()));
        const qreal tableLeft = left.left() - padding;
        const qreal width = 2 * (line.x() - tableLeft);
        const QPoint target(static_cast<int>(std::round(tableLeft + (width * 0.3))), line.y());
        QTest::mousePress(edit.viewport(), Qt::LeftButton, {}, line);
        QTest::mouseMove(edit.viewport(), target);
        QTest::mouseRelease(edit.viewport(), Qt::LeftButton, {}, target);
        QCOMPARE(editor::columnWidths(table->format(), 2), (QList<qreal>{30, 70}));
        edit.undo();
        QVERIFY(editor::hasEqualColumns(table->format(), 2));
    }

    /// Clicking an image selects it, and dragging a corner handle sets its width, taking the
    /// menu's widths when near them.
    void resizeImage()
    {
        QTemporaryDir library;
        QImage picture(400, 200, QImage::Format_RGB32);
        picture.fill(Qt::blue);
        QVERIFY(QDir().mkpath(library.filePath(u"attachments/n"_s)));
        QVERIFY(picture.save(library.filePath(u"attachments/n/p.png"_s)));
        editor::TextEdit edit;
        edit.document()->setDocumentMargin(0);
        edit.setAttachmentContext(library.path(), u"n"_s);
        edit.resize(800, 400);
        edit.show();
        editor::readBody(*edit.document(), u"<p><img src=\"../attachments/n/p.png\"></p>"_s);
        QTest::mouseClick(edit.viewport(), Qt::LeftButton, {}, QPoint(200, 100));
        QCOMPARE(edit.textCursor().selectionEnd() - edit.textCursor().selectionStart(), 1);
        const int quarter = (edit.viewport()->width() / 4) + 4;
        QTest::mousePress(edit.viewport(), Qt::LeftButton, {}, QPoint(400, 200));
        QTest::mouseMove(edit.viewport(), QPoint(quarter, 200));
        QTest::mouseRelease(edit.viewport(), Qt::LeftButton, {}, QPoint(quarter, 200));
        QCOMPARE(editor::writeBody(*edit.document()),
                 u"<p><img src=\"../attachments/n/p.png\" data-width=\"25\"></p>"_s);
    }

    /// An image wrapped square floats at the left of the text column with text beside it, and
    /// one wrapped top and bottom takes a line of its own. Changing the wrapping is one undo
    /// step, and square wrapping makes a full-width image Medium.
    void wrapImages()
    {
        const ui::Theme theme;
        QTextDocument document;
        document.setDocumentMargin(0);
        document.setTextWidth(400);
        editor::ObjectHandler objects(&document);
        objects.setLibraryRoot(u"/nonexistent"_s);
        const QString words = u"word "_s.repeated(60);
        // A missing file is drawn as a frame of its width; the image is the first character.
        editor::readBody(document, u"<p><img src=\"../attachments/n/x.png\" data-width=\"50\" "
                                   u"data-wrap=\"square\">"_s +
                                       words + u"</p>"_s);
        QVERIFY(editor::floatingFrame(document, 0) != nullptr);
        document.documentLayout()->documentSize();
        const QTextLayout *layout = document.firstBlock().layout();
        QVERIFY(layout->lineAt(0).x() >= 200);
        QCOMPARE(layout->lineAt(layout->lineCount() - 1).x(), 0.0);

        QTextCursor image(&document);
        image.setPosition(1, QTextCursor::KeepAnchor);
        editor::setImageWrap(image, editor::ImageWrap::TopAndBottom);
        document.documentLayout()->documentSize();
        // The image takes a line of its own, and the text that follows goes under it.
        QVERIFY(editor::floatingFrame(document, 0) == nullptr);
        QCOMPARE(layout->lineAt(0).textLength(), 1);
        QCOMPARE(layout->lineAt(1).x(), 0.0);
        QVERIFY(layout->lineAt(1).y() >= layout->lineAt(0).ascent() - 1);

        editor::setImageWrap(image, editor::ImageWrap::Inline);
        QVERIFY(editor::floatingFrame(document, 0) == nullptr);
        QVERIFY(!editor::writeBody(document).contains(u"data-wrap"));
        document.undo();
        QVERIFY(editor::writeBody(document).contains(u"data-wrap=\"top-bottom\""));

        editor::readBody(document, u"<p><img src=\"../attachments/n/x.png\"></p>"_s);
        image = QTextCursor(&document);
        image.setPosition(1, QTextCursor::KeepAnchor);
        editor::setImageWrap(image, editor::ImageWrap::Square);
        QCOMPARE(editor::writeBody(document),
                 u"<p><img src=\"../attachments/n/x.png\" data-width=\"50\" data-wrap=\"square\">"
                 u"</p>"_s);
    }

    /// A paragraph after one whose line went below an image wrapped square starts below that
    /// line, also when Qt moves the paragraph without laying it out again.
    void paragraphsAfterFloats()
    {
        const ui::Theme theme;
        QTextDocument document;
        document.setDocumentMargin(0);
        document.setTextWidth(400);
        editor::ObjectHandler objects(&document);
        objects.setLibraryRoot(u"/nonexistent"_s);
        editor::readBody(document, u"<p><img src=\"../attachments/n/x.png\" data-width=\"40\" "
                                   u"data-wrap=\"square\">a</p>\n<p>b <img src=\"../attachments/"
                                   u"n/y.png\" data-width=\"30\" data-wrap=\"top-bottom\"> c</p>\n"
                                   u"<p>d</p>\n<p>e</p>"_s);
        QTextCursor(document.lastBlock()).insertText(u"typed "_s);
        const QAbstractTextDocumentLayout *layout = document.documentLayout();
        for (QTextBlock block = document.begin(); block.next().isValid(); block = block.next()) {
            QVERIFY(layout->blockBoundingRect(block.next()).top() >=
                    layout->blockBoundingRect(block).bottom() - 0.5);
        }
    }

    /// Text already beside an image flows around it once the image is wrapped square, resized
    /// or no longer wrapped, as Qt lays out again only the image's own paragraph.
    void wrapExistingText()
    {
        const ui::Theme theme;
        QTextDocument document;
        document.setDocumentMargin(0);
        document.setTextWidth(400);
        editor::ObjectHandler objects(&document);
        objects.setLibraryRoot(u"/nonexistent"_s);
        editor::readBody(document, u"<p>top</p>\n<p><img src=\"../attachments/n/x.png\" "
                                   u"data-width=\"50\">a</p>\n<p>b</p>\n<p>c</p>"_s);
        const QAbstractTextDocumentLayout *layout = document.documentLayout();
        QTextCursor image(&document);
        image.setPosition(4);
        image.setPosition(5, QTextCursor::KeepAnchor);
        const auto linesBeside = [&] {
            QTextFrame *frame = editor::floatingFrame(document, image.selectionStart());
            const QRectF area = frame != nullptr ? layout->frameBoundingRect(frame) : QRectF();
            for (QTextBlock block = image.block().next(); block.isValid(); block = block.next()) {
                const QRectF rect = layout->blockBoundingRect(block);
                const QTextLine line = block.layout()->lineAt(0);
                const bool besideImage = area.isValid() && rect.top() + line.y() < area.bottom();
                if (besideImage != (rect.left() + line.x() >= area.right() - 0.5) &&
                    area.isValid()) {
                    return false;
                }
                if (!area.isValid() && line.x() > 0) {
                    return false;
                }
            }
            return true;
        };
        editor::setImageWrap(image, editor::ImageWrap::Square);
        QVERIFY(linesBeside());
        image.setCharFormat(editor::imageFormat(u"n"_s, u"x.png"_s, 25, editor::ImageWrap::Square));
        QVERIFY(linesBeside());
        // Text growing above moves the image along with its paragraph.
        QTextCursor(document.firstBlock()).insertText(u"word "_s.repeated(40));
        QCOMPARE(layout->frameBoundingRect(editor::floatingFrame(document, image.selectionStart()))
                     .top(),
                 layout->blockBoundingRect(image.block()).top());
        QVERIFY(linesBeside());
        editor::setImageWrap(image, editor::ImageWrap::Inline);
        QVERIFY(linesBeside());
    }

    /// A table's width is a share of the text column from MinWidth to full width, set as one
    /// undo step, also by dragging its right edge.
    void tableWidth()
    {
        editor::TextEdit edit;
        edit.document()->setDocumentMargin(0);
        edit.resize(600, 300);
        edit.show();
        editor::readBody(*edit.document(), u"<table>\n<tbody>\n<tr><td><p>a</p></td><td><p>b</p>"
                                           u"</td></tr>\n</tbody>\n</table>"_s);
        auto *table =
            qobject_cast<QTextTable *>(edit.document()->rootFrame()->childFrames().first());
        editor::setTableWidth(*table, 5);
        QCOMPARE(editor::tableWidth(table->format()), editor::MinWidth);
        editor::setTableWidth(*table, 100);
        QCOMPARE(editor::tableWidth(table->format()), 0);

        const QAbstractTextDocumentLayout *layout = edit.document()->documentLayout();
        const QRectF cell =
            layout->blockBoundingRect(table->cellAt(0, 1).firstCursorPosition().block());
        const qreal column = edit.document()->textWidth();
        const QPoint edge(static_cast<int>(std::round(column)) - 1,
                          static_cast<int>(cell.center().y()));
        const QPoint half(static_cast<int>(std::round(column * 0.49)), edge.y());
        QTest::mousePress(edit.viewport(), Qt::LeftButton, {}, edge);
        QTest::mouseMove(edit.viewport(), half);
        QTest::mouseRelease(edit.viewport(), Qt::LeftButton, {}, half);
        QVERIFY(editor::writeBody(*edit.document()).startsWith(u"<table data-width=\"50\">"));
        QCOMPARE(editor::columnWidths(table->format(), 2), (QList<qreal>{50, 50}));
        edit.undo();
        QCOMPARE(editor::tableWidth(table->format()), 0);
    }

    /// A table moves past the paragraph or the frame next to it as one undo step; the
    /// paragraphs it leaves stay apart, and an empty paragraph that only kept it apart from a
    /// frame goes with it.
    void moveTables()
    {
        const QString table =
            u"<table>\n<tbody>\n<tr><td><p>t</p></td></tr>\n</tbody>\n</table>\n"_s;
        const QString code = u"<pre><code>x</code></pre>\n"_s;
        QTextDocument document;
        const auto move = [&](int direction) {
            auto *frame =
                qobject_cast<QTextTable *>(editor::topFrameOf(document.find(u"t"_s).block()));
            const int place = editor::tableMovePlace(*frame, direction);
            return place >= 0 && editor::moveTable(*frame, place) != nullptr;
        };
        const QString start = u"<p>a</p>\n"_s + table + u"<p>b</p>\n<p>c</p>"_s;
        editor::readBody(document, start);
        QVERIFY(move(1));
        QCOMPARE(editor::writeBody(document), u"<p>a</p>\n<p>b</p>\n"_s + table + u"<p>c</p>"_s);
        document.undo();
        QCOMPARE(editor::writeBody(document), start);
        QVERIFY(move(-1));
        QCOMPARE(editor::writeBody(document), table + u"<p>a</p>\n<p>b</p>\n<p>c</p>"_s);
        QVERIFY(!move(-1));

        // Two frames keep an empty paragraph between them.
        const QString between = u"<p></p>\n"_s;
        editor::readBody(document, u"<p>a</p>\n"_s + code + between + table + u"<p>b</p>"_s);
        QVERIFY(move(-1));
        QCOMPARE(editor::writeBody(document),
                 u"<p>a</p>\n"_s + table + between + code + u"<p>b</p>"_s);
        QVERIFY(move(1));
        QCOMPARE(editor::writeBody(document),
                 u"<p>a</p>\n"_s + code + between + table + u"<p>b</p>"_s);
    }

    /// Changing a code block's title or language is an edit: the note is saved and the change
    /// can be undone.
    void codeBlockProperties()
    {
        QTextDocument document;
        editor::readBody(document, u"<pre><code>x</code></pre>"_s);
        document.setModified(false);
        const QSignalSpy changed(&document, &QTextDocument::contentsChanged);
        QTextFrame *code = editor::codeBlockOf(document.findBlockByNumber(1));
        QVERIFY(code != nullptr);
        editor::setCodeBlockProperty(*code, editor::CodeTitle, u"a.go"_s);
        editor::setCodeBlockProperty(*code, editor::CodeLanguage, u"go"_s);
        QVERIFY(document.isModified());
        QVERIFY(!changed.isEmpty());
        QCOMPARE(editor::writeBody(document),
                 u"<pre data-language=\"go\" data-title=\"a.go\"><code>x</code></pre>"_s);
        document.undo();
        QCOMPARE(editor::writeBody(document), u"<pre data-title=\"a.go\"><code>x</code></pre>"_s);
    }

    /// A new language highlights the code at once, without waiting for an edit.
    void codeLanguageRehighlights()
    {
        ui::Theme theme;
        theme.apply();
        QTextDocument document;
        const editor::Presenter presenter(&document);
        editor::readBody(document, u"<pre><code>func main() { return 1 }</code></pre>"_s);
        QTextFrame *code = editor::codeBlockOf(document.findBlockByNumber(1));
        QVERIFY(code != nullptr);
        const auto runs = [&] {
            return code->firstCursorPosition().block().layout()->formats().size();
        };
        const qsizetype plain = runs();
        editor::setCodeBlockProperty(*code, editor::CodeLanguage, u"go"_s);
        QVERIFY(runs() > plain);
    }

    /// Text typed right after an attachment chip is ordinary text, not part of the chip.
    void typingAfterObject()
    {
        editor::TextEdit edit;
        editor::readBody(*edit.document(), u"<p><a class=\"attachment\" href=\"../attachments/n/"
                                           u"a.zip\" data-size=\"1\">a.zip</a></p>"_s);
        QTextCursor cursor(edit.document());
        cursor.movePosition(QTextCursor::End);
        edit.setTextCursor(cursor);
        QTest::keyClicks(&edit, u"x"_s);
        cursor.movePosition(QTextCursor::End);
        QVERIFY(!cursor.charFormat().hasProperty(editor::ObjectFile));
        QCOMPARE(cursor.charFormat().verticalAlignment(), QTextCharFormat::AlignNormal);
    }

    /// The language list offers common languages only; any other language a note holds keeps
    /// its highlighting and its name.
    void codeLanguages()
    {
        const QList<editor::SyntaxLanguage> languages = editor::codeLanguages();
        QCOMPARE(languages.size(), 50);
        const auto offers = [&](const QString &key) {
            return std::ranges::find(languages, key, &editor::SyntaxLanguage::key) !=
                   languages.end();
        };
        QVERIFY(offers(u"go"_s));
        QVERIFY(!offers(u".desktop"_s));
        QCOMPARE(editor::languageName(u"ms-dos batch"_s), u"Batch"_s);
        QCOMPARE(editor::languageKey(u"php"_s), u"php/php"_s);
        QCOMPARE(editor::languageKey(u"py"_s), u"python"_s);
        QCOMPARE(editor::languageName(u"awk"_s), u"AWK"_s);
        QCOMPARE(editor::languageName(u"no-such-language"_s), u"no-such-language"_s);
        QCOMPARE(::roundTrip(u"<pre data-language=\"awk\"><code>x</code></pre>"_s),
                 u"<pre data-language=\"awk\"><code>x</code></pre>"_s);
    }

    /// Inside a code block Tab indents, Return keeps the indentation, Ctrl+Return leaves the
    /// block and ``` typed on a line of its own starts one.
    void codeBlockKeys()
    {
        editor::TextEdit edit;
        QTest::keyClicks(&edit, u"```py"_s);
        QTest::keyClick(&edit, Qt::Key_Return);
        QTest::keyClicks(&edit, u"if x:"_s);
        QTest::keyClick(&edit, Qt::Key_Return);
        QTest::keyClick(&edit, Qt::Key_Tab);
        QTest::keyClicks(&edit, u"- y"_s);
        QTest::keyClick(&edit, Qt::Key_Space);
        QTest::keyClick(&edit, Qt::Key_Return);
        QTest::keyClicks(&edit, u"z"_s);
        QTest::keyClick(&edit, Qt::Key_Backtab);
        QTest::keyClick(&edit, Qt::Key_Return, Qt::ControlModifier);
        QTest::keyClicks(&edit, u"after"_s);
        QCOMPARE(editor::writeBody(*edit.document()),
                 u"<pre data-language=\"python\"><code>if x:\n    - y \nz</code></pre>\n"
                 u"<p>after</p>"_s);
        edit.undo();
        QCOMPARE(editor::writeBody(*edit.document()),
                 u"<pre data-language=\"python\"><code>if x:\n    - y \nz</code></pre>"_s);
    }

    /// Undo right after a shortcut brings back the typed characters.
    void markdownShortcutUndo()
    {
        editor::TextEdit edit;
        QTest::keyClicks(&edit, u"1."_s);
        QTest::keyClick(&edit, Qt::Key_Space);
        QCOMPARE(editor::writeBody(*edit.document()), u"<ol>\n<li></li>\n</ol>"_s);
        edit.undo();
        QCOMPARE(editor::writeBody(*edit.document()), u"<p>1. </p>"_s);
    }

    /// Backspace right after an automatic change undoes it and keeps the typed characters.
    void backspaceUndoesShortcut_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("html");
        QTest::newRow("bulleted list") << u"- "_s << u"<p>- x</p>"_s;
        QTest::newRow("numbered list") << u"1. "_s << u"<p>1. x</p>"_s;
        QTest::newRow("checklist") << u"[ ] "_s << u"<p>[ ] x</p>"_s;
        QTest::newRow("heading") << u"## "_s << u"<p>## x</p>"_s;
        QTest::newRow("quote") << u"> "_s << u"<p>&gt; x</p>"_s;
        QTest::newRow("divider") << u"---\n"_s << u"<p>---x</p>"_s;
        QTest::newRow("code block") << u"```\n"_s << u"<p>```x</p>"_s;
        QTest::newRow("inline code") << u"`a`"_s << u"<p>`a`x</p>"_s;
        QTest::newRow("link") << u"https://example.com "_s << u"<p>https://example.com x</p>"_s;
    }

    void backspaceUndoesShortcut()
    {
        QFETCH(QString, typed);
        QFETCH(QString, html);
        editor::TextEdit edit;
        for (const QChar c : typed) {
            if (c == u'\n') {
                QTest::keyClick(&edit, Qt::Key_Return);
            } else if (c == u' ') {
                QTest::keyClick(&edit, Qt::Key_Space, Qt::NoModifier);
            } else {
                QTest::keyClicks(&edit, QString(c));
            }
        }
        const QString applied = editor::writeBody(*edit.document());
        QCoreApplication::processEvents();
        QTest::keyClick(&edit, Qt::Key_Backspace);
        QTest::keyClicks(&edit, u"x"_s);
        QCOMPARE(editor::writeBody(*edit.document()), html);
        QVERIFY(applied != html);
    }

    /// Once something is typed or the cursor moves, Backspace no longer undoes the change.
    void backspaceAfterShortcutDeletes()
    {
        editor::TextEdit edit;
        QTest::keyClicks(&edit, u"##"_s);
        QTest::keyClick(&edit, Qt::Key_Space, Qt::NoModifier);
        QTest::keyClicks(&edit, u"a"_s);
        QTest::keyClick(&edit, Qt::Key_Backspace);
        QCOMPARE(editor::writeBody(*edit.document()), u"<h2></h2>"_s);

        edit.clear();
        QTest::keyClicks(&edit, u"abc"_s);
        QTest::keyClick(&edit, Qt::Key_Return);
        QTest::keyClicks(&edit, u"-"_s);
        QTest::keyClick(&edit, Qt::Key_Space, Qt::NoModifier);
        QTest::keyClick(&edit, Qt::Key_Left);
        QTest::keyClick(&edit, Qt::Key_Right);
        QTest::keyClick(&edit, Qt::Key_Backspace);
        QVERIFY(!edit.toPlainText().contains(u'-'));
    }

    /// Pasted Markdown uses the note model; unformatted text keeps its exact whitespace.
    void markdownPaste_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("html");
        QTest::newRow("inline")
            << u"a **bold** *italic* ~~gone~~ `code` z"_s
            << u"<p>a <strong>bold</strong> <em>italic</em> <s>gone</s> <code>code</code> z</p>"_s;
        QTest::newRow("headings") << u"# One\n\n###### Six"_s << u"<h1>One</h1>\n<h3>Six</h3>"_s;
        QTest::newRow("fence")
            << u"before\n\n```go\nx\n    y\n```\n\nafter"_s
            << u"<p>before</p>\n<pre data-language=\"go\"><code>x\n    y</code></pre>\n<p>after</p>"_s;
        QTest::newRow("fence blanks") << u"```\n\nx\n\n```"_s << u"<pre><code>\nx\n</code></pre>"_s;
        QTest::newRow("empty fence") << u"```\n```"_s << u"<pre><code></code></pre>"_s;
        // Copied text indents lists by a tab or four spaces; only fences make code (regression).
        QTest::newRow("indented list") << u"text\n\n    - x\n    - y"_s
                                       << u"<p>text</p>\n<ul>\n<li>x</li>\n<li>y</li>\n</ul>"_s;
        QTest::newRow("tab-indented list")
            << u"text\n\n\t1. x\n\t2. y"_s << u"<p>text</p>\n<ol>\n<li>x</li>\n<li>y</li>\n</ol>"_s;
        QTest::newRow("indented text")
            << u"**text**\n\n    x\n    y"_s << u"<p><strong>text</strong></p>\n<p>x<br>y</p>"_s;
        QTest::newRow("tasks")
            << u"- [x] done\n- [ ] todo"_s
            << u"<ul class=\"checklist\">\n<li data-checked=\"true\">done</li>\n<li data-checked=\"false\">todo</li>\n</ul>"_s;
        QTest::newRow("lists")
            << u"- one\n- two\n\n3. three\n4. four"_s
            << u"<ul>\n<li>one</li>\n<li>two</li>\n</ul>\n<ol start=\"3\">\n<li>three</li>\n<li>four</li>\n</ol>"_s;
        QTest::newRow("quote") << u"> quoted"_s << u"<blockquote>\n<p>quoted</p>\n</blockquote>"_s;
        QTest::newRow("link") << u"[site](https://example.com)"_s
                              << u"<p><a href=\"https://example.com\">site</a></p>"_s;
        QTest::newRow("plain") << u" one  two\nthree\n\n"_s
                               << u"<p> one  two</p>\n<p>three</p>\n<p></p>\n<p></p>"_s;
        QTest::newRow("literal html")
            << u"<b>literal</b>"_s << u"<p>&lt;b&gt;literal&lt;/b&gt;</p>"_s;
        QTest::newRow("remote image")
            << u"![description](https://example.com/a.png) **end**"_s
            << u"<p><a href=\"https://example.com/a.png\">description</a> <strong>end</strong></p>"_s;
        QTest::newRow("remote image without description")
            << u"![](https://example.com/a.png) **end**"_s
            << u"<p><a href=\"https://example.com/a.png\">https://example.com/a.png</a> "
               u"<strong>end</strong></p>"_s;
        QTest::newRow("remote image in a link")
            << u"[![description](https://example.com/a.png)](https://example.org) **end**"_s
            << u"<p><a href=\"https://example.org\">description</a> <strong>end</strong></p>"_s;
        QTest::newRow("table")
            << u"| A | B |\n|---|---|\n|x|y|"_s
            << u"<table>\n<thead>\n<tr><th><p>A</p></th><th><p>B</p></th></tr>\n</thead>\n<tbody>\n<tr><td><p>x</p></td><td><p>y</p></td></tr>\n</tbody>\n</table>"_s;
        QTest::newRow("formatted code")
            << u"**`bold`** *`italic`* ~~`gone`~~ [`linked`](https://example.com)"_s
            << u"<p><strong><code>bold</code></strong> <em><code>italic</code></em> "
               u"<s><code>gone</code></s> <a href=\"https://example.com\"><code>linked</code></a></p>"_s;
        QTest::newRow("line breaks")
            << u"one\\\ntwo  \nthree\n\n- a\\\n  b"_s
            << u"<p>one<br>two<br>three</p>\n<ul>\n<li>a<br>b</li>\n</ul>"_s;
        QTest::newRow("item paragraphs")
            << u"- one\n\n  more\n- two"_s << u"<ul>\n<li>one<br>more</li>\n<li>two</li>\n</ul>"_s;
        QTest::newRow("table pipe in code")
            << u"| `a\\|b` |\n|---|\n| c\\|d |"_s
            << u"<table>\n<thead>\n<tr><th><p><code>a|b</code></p></th></tr>\n</thead>\n<tbody>\n<tr><td><p>c|d</p></td></tr>\n</tbody>\n</table>"_s;
        QTest::newRow("entity") << u"**&copy; &#8212;**"_s << u"<p><strong>© —</strong></p>"_s;
        QTest::newRow("plain dividers")
            << u"day one\n-----\nfirst\nsecond\n\n-----\nempty\n=====\n# Head\n---\n> *quote*\n> ---"_s
            << u"<p>day one</p>\n<hr>\n<p>first<br>second</p>\n<hr>\n<p>empty</p>\n<hr>\n"
               u"<h1>Head</h1>\n<hr>\n<blockquote>\n<p><em>quote</em></p>\n</blockquote>\n<hr>"_s;
        QTest::newRow("divider first") << u"---\n\n- x"_s << u"<hr>\n<ul>\n<li>x</li>\n</ul>"_s;
    }

    void markdownPaste()
    {
        QFETCH(QString, text);
        QFETCH(QString, html);
        ClipboardEdit edit;
        QMimeData mime;
        mime.setText(text);
        edit.insertFromMimeData(&mime);
        QCOMPARE(editor::writeBody(*edit.document()), html);
        QCOMPARE(::roundTrip(html), html);
        edit.undo();
        QVERIFY(edit.toPlainText().isEmpty());
    }

    /// A divider that starts pasted content splits the line it is pasted into.
    void dividerPastedInLine()
    {
        ClipboardEdit edit;
        edit.setPlainText(u"abcdef"_s);
        QTextCursor cursor = edit.textCursor();
        cursor.setPosition(3);
        edit.setTextCursor(cursor);
        QMimeData mime;
        mime.setText(u"---\nx"_s);
        edit.insertFromMimeData(&mime);
        QCOMPARE(editor::writeBody(*edit.document()), u"<p>abc</p>\n<hr>\n<p>xdef</p>"_s);
        edit.undo();
        QCOMPARE(edit.toPlainText(), u"abcdef"_s);
    }

    /// Ctrl+C from a text editor offers HTML too; Ctrl+V must still recognise Markdown.
    void markdownFromCopiedText()
    {
        QTextEdit source;
        source.setPlainText(u"# Title\n\n**bold** and `code`"_s);
        source.selectAll();
        QTest::keyClick(&source, Qt::Key_C, Qt::ControlModifier);
        QVERIFY(QGuiApplication::clipboard()->mimeData()->hasHtml());
        ClipboardEdit target;
        QTest::keyClick(&target, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(editor::writeBody(*target.document()),
                 u"<h1>Title</h1>\n<p><strong>bold</strong> and <code>code</code></p>"_s);
        target.undo();
        QVERIFY(target.toPlainText().isEmpty());
    }

    /// Explicit Markdown overrides HTML wrappers; rich prose and bare URLs keep HTML.
    void markdownFromHtml_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("html");
        QTest::addColumn<QString>("expected");
        QTest::newRow("fenced code") << u"```go\nx\n```"_s << u"<pre>```go\nx\n```</pre>"_s
                                     << u"<pre data-language=\"go\"><code>x</code></pre>"_s;
        QTest::newRow("list") << u"- first\n- second"_s << u"<p>- first</p><p>- second</p>"_s
                              << u"<ul>\n<li>first</li>\n<li>second</li>\n</ul>"_s;
        QTest::newRow("indented list")
            << u"Steps:\n\n    - first\n    - second"_s
            << u"<p>Steps:</p><ul><li>first</li><li>second</li></ul>"_s
            << u"<p>Steps:</p>\n<ul>\n<li>first</li>\n<li>second</li>\n</ul>"_s;
        QTest::newRow("link") << u"[site](https://example.com)"_s
                              << u"<p>[site](https://example.com)</p>"_s
                              << u"<p><a href=\"https://example.com\">site</a></p>"_s;
        QTest::newRow("angle link")
            << u"<https://example.com>"_s << u"<p>&lt;https://example.com&gt;</p>"_s
            << u"<p><a href=\"https://example.com\">https://example.com</a></p>"_s;
        QTest::newRow("rich prose")
            << u"rich text"_s << u"<p><strong>rich</strong> <em>text</em></p>"_s
            << u"<p><strong>rich</strong> <em>text</em></p>"_s;
        QTest::newRow("rich bare URL")
            << u"rich https://example.com\nsecond line"_s
            << u"<p><strong>rich</strong> <a href=\"https://example.com\">https://example.com</a></p><p>second line</p>"_s
            << u"<p><strong>rich</strong> <a href=\"https://example.com\">https://example.com</a></p>\n<p>second line</p>"_s;
        QTest::newRow("brackets and bare URL")
            << u"[literal] https://example.com"_s
            << u"<p><em>[literal]</em> https://example.com</p>"_s
            << u"<p><em>[literal]</em> https://example.com</p>"_s;
        QTest::newRow("HTML only")
            << QString() << u"<p><em>rich</em></p>"_s << u"<p><em>rich</em></p>"_s;
    }

    void markdownFromHtml()
    {
        QFETCH(QString, text);
        QFETCH(QString, html);
        QFETCH(QString, expected);
        ClipboardEdit target;
        QMimeData mime;
        if (!text.isNull()) {
            mime.setText(text);
        }
        mime.setHtml(html);
        target.insertFromMimeData(&mime);
        QCOMPARE(editor::writeBody(*target.document()), expected);
    }

    /// Private note content stays literal even when its copied text resembles Markdown.
    void copiedMarkdownLiteral()
    {
        ClipboardEdit source;
        const QString body = u"<p><em>**literal**</em></p>"_s;
        editor::readBody(*source.document(), body);
        source.selectAll();
        const std::unique_ptr<QMimeData> copied(source.createMimeDataFromSelection());
        ClipboardEdit target;
        target.insertFromMimeData(copied.get());
        QCOMPARE(editor::writeBody(*target.document()), body);
    }

    /// The popup pastes literal text and respects read-only notes and an empty clipboard.
    void plainPasteContextMenu()
    {
        ClipboardEdit target;
        auto *mime = new QMimeData;
        mime->setText(u"# Title\n\n**literal**"_s);
        mime->setHtml(u"<h1>Title</h1><p><strong>literal</strong></p>"_s);
        QGuiApplication::clipboard()->setMimeData(mime);
        std::optional<bool> enabled;
        QString shortcut;
        // Drive the popup and action to cover menu wiring along with paste behaviour.
        const auto openMenu = [&] {
            enabled.reset();
            QTimer::singleShot(0, &target, [&] {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (menu == nullptr) {
                    return;
                }
                if (auto *plain = menu->findChild<QAction *>(u"pastePlainText"_s)) {
                    enabled = plain->isEnabled();
                    shortcut = plain->shortcut().toString();
                    if (plain->isEnabled()) {
                        plain->trigger();
                    }
                }
                menu->close();
            });
            QContextMenuEvent event(QContextMenuEvent::Mouse, QPoint(), QPoint());
            target.contextMenuEvent(&event);
        };
        openMenu();
        QVERIFY(enabled.has_value());
        QVERIFY(*enabled);
        QCOMPARE(shortcut, u"Ctrl+Shift+V"_s);
        QCOMPARE(editor::writeBody(*target.document()),
                 u"<p># Title</p>\n<p></p>\n<p>**literal**</p>"_s);
        target.undo();
        QVERIFY(target.toPlainText().isEmpty());
        target.setReadOnly(true);
        openMenu();
        QVERIFY(!enabled.has_value());
        QVERIFY(target.toPlainText().isEmpty());
        target.setReadOnly(false);
        QGuiApplication::clipboard()->clear();
        openMenu();
        QVERIFY(enabled.has_value());
        QVERIFY(!*enabled);
        QVERIFY(target.toPlainText().isEmpty());
    }

    /// Copy offers rich HTML and literal text by default; Markdown copy offers only text.
    void markdownCopy()
    {
        ClipboardEdit edit;
        const QString html = u"<h1>Title</h1>\n<p>a <strong>bold</strong> <code>code</code> z</p>\n"
                             u"<pre data-language=\"go\"><code>x\ny</code></pre>\n<p>after</p>"_s;
        editor::readBody(*edit.document(), html);
        edit.selectAll();
        const std::unique_ptr<QMimeData> rich(edit.createMimeDataFromSelection());
        QVERIFY(rich->hasHtml());
        QVERIFY(rich->hasFormat(editor::TextEdit::BodyMimeType));
        QVERIFY(rich->text().contains(u"a bold code z"_s));
        QVERIFY(!rich->text().contains(u"**"_s));
        ClipboardEdit target;
        target.insertFromMimeData(rich.get());
        QCOMPARE(editor::writeBody(*target.document()), html);
        QTest::keyClick(&edit, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        const QMimeData *markdown = QGuiApplication::clipboard()->mimeData();
        QVERIFY(markdown->hasText());
        QVERIFY(!markdown->hasHtml());
        QVERIFY(!markdown->hasFormat(editor::TextEdit::BodyMimeType));
        QVERIFY(markdown->text().contains(u"**bold**"_s));
        QVERIFY(markdown->text().contains(u"`code`"_s));
        QVERIFY2(markdown->text().contains(u"```go\nx\ny\n```"_s), qPrintable(markdown->text()));
        QTextDocument parsed;
        QVERIFY(editor::readMarkdown(parsed, markdown->text()));
        QCOMPARE(editor::writeBody(parsed), html);
    }

    /// Markdown export preserves supported structure, including lists and code boundaries.
    void markdownStructure_data()
    {
        QTest::addColumn<QString>("html");
        QTest::newRow("lists") << u"<ol start=\"3\">\n<li>three</li>\n<li>four</li>\n</ol>"_s;
        QTest::newRow("nested") << u"<ul>\n<li>one\n<ol>\n<li>two</li>\n</ol>\n</li>\n</ul>"_s;
        QTest::newRow("tasks")
            << u"<ul class=\"checklist\">\n<li data-checked=\"true\">done</li>\n<li data-checked=\"false\">todo</li>\n</ul>"_s;
        QTest::newRow("quote") << u"<blockquote>\n<p>quote</p>\n</blockquote>"_s;
        QTest::newRow("table")
            << u"<table>\n<thead>\n<tr><th><p>A</p></th><th><p>B</p></th></tr>\n</thead>\n<tbody>\n<tr><td><p>x</p></td><td><p>y</p></td></tr>\n</tbody>\n</table>"_s;
        QTest::newRow("code first") << u"<pre><code>x\ny</code></pre>\n<h2>after</h2>"_s;
        QTest::newRow("code last")
            << u"<p>before</p>\n<pre data-language=\"go\"><code>x\ny</code></pre>"_s;
        QTest::newRow("code between lists")
            << u"<ul>\n<li>before</li>\n</ul>\n<pre><code>x</code></pre>\n<ol>\n<li>after</li>\n</ol>"_s;
        QTest::newRow("empty code") << u"<pre><code></code></pre>"_s;
        QTest::newRow("code blank lines") << u"<pre><code>\nx\n</code></pre>"_s;
        QTest::newRow("inline backtick") << u"<p>a <code>b`c</code> d</p>"_s;
        QTest::newRow("inline code spaces") << u"<p>a <code> b </code> c</p>"_s;
        QTest::newRow("inline code edges") << u"<p>a <code>`b`</code> c</p>"_s;
        QTest::newRow("fence in code") << u"<pre><code>```\nx\n```</code></pre>"_s;
        QTest::newRow("line break") << u"<ul>\n<li>one<br>- two</li>\n</ul>"_s;
        QTest::newRow("bold code")
            << u"<p><strong><code>b</code></strong> <em><code>c</code></em></p>"_s;
        QTest::newRow("quote paragraphs")
            << u"<blockquote>\n<p>one</p>\n<p>two</p>\n</blockquote>"_s;
        QTest::newRow("adjacent lists")
            << u"<ul>\n<li>a</li>\n</ul>\n<ul class=\"checklist\">\n<li data-checked=\"false\">b</li>\n</ul>"_s;
        QTest::newRow("escapes") << u"<p><strong>x</strong></p>\n<p># a *b* [c] a_b _d_ &lt;e&gt; "
                                    u"&amp;amp; ~f~ \\g</p>\n"
                                    u"<p>1. not a list</p>"_s;
        QTest::newRow("table pipe")
            << u"<table>\n<thead>\n<tr><th><p>a|b</p></th></tr>\n</thead>\n<tbody>\n<tr><td><p><code>c|d</code></p></td></tr>\n</tbody>\n</table>"_s;
    }

    /// Markdown copy keeps every paragraph and list item on one line, whatever its length, and
    /// keeps spaces outside emphasis so it stays valid.
    void markdownLines()
    {
        const QString words = u"words that make this line much longer than eighty columns"_s;
        QTextDocument source;
        editor::readBody(
            source,
            u"<h2>Title #</h2>\n<p>Run (<code>git reset --hard</code>, <code>git clean</code>) "
            u"with <strong>bold </strong>and "_s +
                words + u"</p>\n<ol start=\"9\">\n<li>nine</li>\n<li>ten with "_s + words +
                u" <code>a</code>/<code>b</code>\n<ul>\n<li>child</li>\n</ul>\n</li>\n</ol>\n"
                u"<blockquote>\n<p>one<br>- two</p>\n<p>three</p>\n</blockquote>"_s);
        QCOMPARE(editor::writeMarkdown(source),
                 u"## Title \\#\n\nRun (`git reset --hard`, `git clean`) with **bold** and "_s +
                     words + u"\n\n9. nine\n10. ten with "_s + words +
                     u" `a`/`b`\n    - child\n\n> one\\\n> \\- two\n>\n> three\n"_s);
    }

    void markdownStructure()
    {
        QFETCH(QString, html);
        QTextDocument source;
        editor::readBody(source, html);
        const QString markdown = editor::writeMarkdown(source);
        QTextDocument target;
        QVERIFY2(editor::readMarkdown(target, markdown), qPrintable(markdown));
        QCOMPARE(editor::writeBody(target), html);
        QCOMPARE(editor::writeBody(source), html);
    }

    /// Explicit plain paste and code blocks bypass Markdown; ordinary rich HTML stays formatted.
    void markdownPasteModes()
    {
        ClipboardEdit edit;
        QMimeData mime;
        mime.setText(u"rich"_s);
        mime.setHtml(u"<p><em>rich</em></p>"_s);
        edit.insertFromMimeData(&mime);
        QCOMPARE(editor::writeBody(*edit.document()), u"<p><em>rich</em></p>"_s);
        edit.clear();
        edit.setCurrentCharFormat(QTextCharFormat());
        QGuiApplication::clipboard()->setText(u"**literal**"_s);
        QTest::keyClick(&edit, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(editor::writeBody(*edit.document()), u"<p>**literal**</p>"_s);
        edit.clear();
        QTextCursor cursor(edit.document());
        QVERIFY(editor::insertCodeBlock(cursor) != nullptr);
        edit.setTextCursor(cursor);
        mime.setText(u"# literal\n**also literal**"_s);
        edit.insertFromMimeData(&mime);
        QCOMPARE(editor::writeBody(*edit.document()),
                 u"<pre><code># literal\n**also literal**</code></pre>"_s);
    }

    /// Trailing inline code does not continue on Return; spaces survive formatting and reload.
    void inlineCodeBoundaries()
    {
        ClipboardEdit edit;
        const QString plain = u"before  code  after"_s;
        edit.setPlainText(plain);
        QTextCursor selected(edit.document());
        selected.setPosition(8);
        selected.setPosition(12, QTextCursor::KeepAnchor);
        edit.setTextCursor(selected);
        QTest::keyClicks(&edit, u"`"_s);
        QCOMPARE(edit.toPlainText(), plain);
        const QString body = editor::writeBody(*edit.document());
        QCOMPARE(body, u"<p>before  <code>code</code>  after</p>"_s);
        editor::readBody(*edit.document(), body);
        QCOMPARE(edit.toPlainText(), plain);
        edit.selectAll();
        const std::unique_ptr<QMimeData> copy(edit.createMimeDataFromSelection());
        QCOMPARE(copy->text(), plain);
        editor::readBody(*edit.document(), u"<p>before <code>code</code></p>"_s);
        edit.moveCursor(QTextCursor::End);
        QTest::keyClick(&edit, Qt::Key_Return);
        edit.undo();
        QCOMPARE(editor::writeBody(*edit.document()), u"<p>before <code>code</code></p>"_s);
        QTest::keyClick(&edit, Qt::Key_Return);
        QTest::keyClicks(&edit, u"after"_s);
        QCOMPARE(editor::writeBody(*edit.document()),
                 u"<p>before <code>code</code></p>\n<p>after</p>"_s);
    }

    /// Inline-code backgrounds leave neighbouring spaces visible at the actual text layout.
    void inlineCodeSpacing()
    {
        ui::Theme theme;
        theme.apply();
        QTextDocument document;
        document.setDefaultFont(theme.font(ui::TextRole::Body));
        document.setTextWidth(500);
        editor::readBody(document, u"<p>a <code>code</code> b</p>"_s);
        const editor::Presenter presenter(&document);
        const QRectF rect = document.documentLayout()->blockBoundingRect(document.begin());
        const QTextLine line = document.begin().layout()->lineAt(0);
        QImage image(500, 100, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            editor::paintInlineCode(painter, document.begin(), theme.roles(), {}, image.height());
        }
        const int y = static_cast<int>(rect.top() + line.y() + line.ascent() / 2);
        // Sample the outer quarter of each space, which must remain outside the background.
        for (const auto &[from, to] : {std::pair{1, 2}, std::pair{6, 7}}) {
            const qreal left = rect.left() + line.cursorToX(from);
            const qreal right = rect.left() + line.cursorToX(to);
            const qreal x = from == 1 ? left + (right - left) / 4 : right - (right - left) / 4;
            QCOMPARE(image.pixelColor(static_cast<int>(std::round(x)), y).alpha(), 0);
        }
    }

    /// Text typed between two backticks becomes inline code, and the text typed after it is
    /// plain again.
    void inlineCodeShortcut_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("html");
        QTest::newRow("code") << u"a `b c` d"_s << u"<p>a <code>b c</code> d</p>"_s;
        QTest::newRow("twice") << u"`a` `b`"_s << u"<p><code>a</code> <code>b</code></p>"_s;
        QTest::newRow("empty") << u"``"_s << u"<p>``</p>"_s;
        QTest::newRow("blank") << u"` `"_s << u"<p>` `</p>"_s;
        QTest::newRow("double backtick") << u"``a`"_s << u"<p>``a`</p>"_s;
        QTest::newRow("fence") << u"```"_s << u"<p>```</p>"_s;
        QTest::newRow("code block") << u"```\n`a`"_s << u"<pre><code>`a`</code></pre>"_s;
    }

    void inlineCodeShortcut()
    {
        QFETCH(QString, typed);
        QFETCH(QString, html);
        editor::TextEdit edit;
        for (const QChar c : typed) {
            if (c == u'\n') {
                QTest::keyClick(&edit, Qt::Key_Return);
            } else {
                QTest::keyClicks(&edit, QString(c));
            }
        }
        QCOMPARE(editor::writeBody(*edit.document()), html);
    }

    /// Undo right after the inline code shortcut brings back the backticks.
    void inlineCodeShortcutUndo()
    {
        editor::TextEdit edit;
        QTest::keyClicks(&edit, u"`a`"_s);
        QCOMPARE(editor::writeBody(*edit.document()), u"<p><code>a</code></p>"_s);
        edit.undo();
        QCOMPARE(editor::writeBody(*edit.document()), u"<p>`a`</p>"_s);
    }

    /// Right arrow at the right edge of inline code leaves it without moving, at the end of a
    /// paragraph and before plain text; a second press moves on.
    void leaveInlineCode()
    {
        editor::TextEdit edit;
        editor::readBody(*edit.document(), u"<p><code>a</code></p>\n<p><code>b</code>c</p>"_s);
        QTextCursor cursor(edit.document()->findBlockByNumber(0));
        cursor.movePosition(QTextCursor::EndOfBlock);
        edit.setTextCursor(cursor);
        QTest::keyClick(&edit, Qt::Key_Right);
        QTest::keyClicks(&edit, u"x"_s);
        cursor.setPosition(edit.document()->findBlockByNumber(1).position() + 1);
        edit.setTextCursor(cursor);
        QTest::keyClick(&edit, Qt::Key_Right);
        QTest::keyClicks(&edit, u"y"_s);
        // From the start of the paragraph: into the code, out of it, then on past y.
        cursor.setPosition(edit.document()->findBlockByNumber(1).position());
        edit.setTextCursor(cursor);
        for (int i = 0; i < 3; ++i) {
            QTest::keyClick(&edit, Qt::Key_Right);
        }
        QTest::keyClicks(&edit, u"z"_s);
        QCOMPARE(editor::writeBody(*edit.document()),
                 u"<p><code>a</code>x</p>\n<p><code>b</code>yzc</p>"_s);
    }

    /// A typed bracket or quote encloses the selection as one undo step and keeps the inner
    /// text selected; a backtick makes it inline code.
    void wrapSelection()
    {
        editor::TextEdit edit;
        editor::readBody(*edit.document(), u"<p>a b c</p>"_s);
        QTextCursor cursor(edit.document());
        cursor.setPosition(2);
        cursor.setPosition(3, QTextCursor::KeepAnchor);
        edit.setTextCursor(cursor);
        QTest::keyClicks(&edit, u"("_s);
        QTest::keyClicks(&edit, u"«"_s);
        QCOMPARE(editor::writeBody(*edit.document()), u"<p>a («b») c</p>"_s);
        QCOMPARE(edit.textCursor().selectedText(), u"b"_s);
        edit.undo();
        QCOMPARE(editor::writeBody(*edit.document()), u"<p>a (b) c</p>"_s);
        cursor.setPosition(3);
        cursor.setPosition(4, QTextCursor::KeepAnchor);
        edit.setTextCursor(cursor);
        QTest::keyClicks(&edit, u"`"_s);
        QCOMPARE(editor::writeBody(*edit.document()), u"<p>a (<code>b</code>) c</p>"_s);
    }

    /// Copied content pastes back without loss, and its files are copied into the folder of
    /// the note it is pasted into; dropped files become images or chips, and pasted HTML brings
    /// only images.
    void copyAndPasteFiles()
    {
        QTemporaryDir library;
        const QString folderA = library.filePath(u"attachments/a"_s);
        QVERIFY(core::storeData(folderA, u"pic.png"_s, "\x89PNG\r\n\x1a\n"));
        ClipboardEdit source;
        source.setAttachmentContext(library.path(), u"a"_s);
        editor::readBody(*source.document(),
                         u"<p>x <img src=\"../attachments/a/pic.png\" data-width=\"25\"> "
                         u"<strong>y</strong></p>"_s);
        source.selectAll();
        const std::unique_ptr<QMimeData> copied(source.createMimeDataFromSelection());

        ClipboardEdit target;
        target.setAttachmentContext(library.path(), u"b"_s);
        target.insertFromMimeData(copied.get());
        QTRY_COMPARE(editor::writeBody(*target.document()),
                     u"<p>x <img src=\"../attachments/b/pic.png\" data-width=\"25\"> "
                     u"<strong>y</strong></p>"_s);
        QVERIFY(QFile::exists(library.filePath(u"attachments/b/pic.png"_s)));

        QFile document(library.filePath(u"report.txt"_s));
        QVERIFY(document.open(QIODevice::WriteOnly));
        document.write("abc");
        document.close();
        QMimeData dropped;
        dropped.setUrls({QUrl::fromLocalFile(document.fileName())});
        target.insertFromMimeData(&dropped);
        QTRY_VERIFY(editor::writeBody(*target.document())
                        .contains(u"<a class=\"attachment\" href=\"../attachments/b/report.txt\" "
                                  u"data-size=\"3\">report.txt</a>"));

        // Pasted HTML can name any local file, so only an image is taken from it.
        QMimeData pasted;
        pasted.setHtml(u"<p>pasted <img src=\"%1\"> <img src=\"%2\"></p>"_s.arg(
            QUrl::fromLocalFile(document.fileName()).toString(),
            QUrl::fromLocalFile(folderA + u"/pic.png"_s).toString()));
        target.insertFromMimeData(&pasted);
        QTRY_VERIFY(editor::writeBody(*target.document()).contains(u"pasted"));
        QVERIFY(QFile::exists(library.filePath(u"attachments/b/pic (2).png"_s)));
        QVERIFY(!QFile::exists(library.filePath(u"attachments/b/report (2).txt"_s)));
    }
    /// Replacing an image at the same path updates already open views instead of stale pixels.
    void imageCacheTracksFileVersion()
    {
        ui::Theme theme;
        theme.apply();
        const QTemporaryDir dir;
        const QString id = core::newId();
        const QString path = core::attachmentsDir(dir.path(), id) + u"/image.png"_s;
        const auto bytes = [](const QColor &color) {
            QImage image(16, 16, QImage::Format_RGB32);
            image.fill(color);
            QByteArray encoded;
            QBuffer buffer(&encoded);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            return encoded;
        };
        QVERIFY(core::writeFileAtomic(path, bytes(Qt::red)));
        QTextDocument document;
        document.setDocumentMargin(0);
        document.setTextWidth(100);
        editor::ObjectHandler objects(&document);
        objects.setLibraryRoot(dir.path());
        const auto format = editor::imageFormat(id, u"image.png"_s, 0);
        QTextCursor(&document).insertText(QString(QChar::ObjectReplacementCharacter), format);
        QSignalSpy decoded(&objects, &editor::ObjectHandler::imageLoaded);
        const auto draw = [&] {
            QImage image(16, 16, QImage::Format_RGB32);
            image.fill(Qt::white);
            QPainter painter(&image);
            objects.drawObject(&painter, QRectF(0, 0, 16, 16), &document, 0, format);
            painter.end();
            return image;
        };
        draw();
        QTRY_VERIFY(!decoded.isEmpty());
        QCOMPARE(draw().pixelColor(8, 8), QColor(Qt::red));
        decoded.clear();
        QVERIFY(core::writeFileAtomic(path, bytes(Qt::blue)));
        draw();
        QTRY_VERIFY(!decoded.isEmpty());
        QCOMPARE(draw().pixelColor(8, 8), QColor(Qt::blue));
    }
};

QTEST_MAIN(EditorTest)
#include "tst_editor.moc"
