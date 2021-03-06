/* This file is part of KDevelop
     Copyright 2012 Olivier de Gaalon <olivier.jg@gmail.com>
               2014 David Stevens <dgedstevens@gmail.com>

     This library is free software; you can redistribute it and/or
     modify it under the terms of the GNU Library General Public
     License version 2 as published by the Free Software Foundation.

     This library is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.   See the GNU
     Library General Public License for more details.

     You should have received a copy of the GNU Library General Public License
     along with this library; see the file COPYING.LIB. If not, write to
     the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
     Boston, MA 02110-1301, USA.
*/

#include "test_assistants.h"

#include <QtTest/QtTest>
#include <QTemporaryDir>

#include <ktexteditor/view.h>
#include <ktexteditor/document.h>

#include <tests/autotestshell.h>
#include <tests/testcore.h>

#include <util/foregroundlock.h>

#include <interfaces/idocumentcontroller.h>
#include <interfaces/ilanguagecontroller.h>
#include <interfaces/iplugincontroller.h>
#include <interfaces/isourceformattercontroller.h>

#include <language/assistant/staticassistant.h>
#include <language/assistant/staticassistantsmanager.h>
#include <language/assistant/renameaction.h>
#include <language/backgroundparser/backgroundparser.h>
#include <language/duchain/duchain.h>
#include <language/duchain/duchainutils.h>
#include <language/codegen/coderepresentation.h>

#include <shell/documentcontroller.h>

using namespace KDevelop;
using namespace KTextEditor;

QTEST_MAIN(TestAssistants)

ForegroundLock *globalTestLock = 0;
StaticAssistantsManager *staticAssistantsManager() { return Core::self()->languageController()->staticAssistantsManager(); }

void TestAssistants::initTestCase()
{
    QLoggingCategory::setFilterRules(QStringLiteral(R"(
        *.debug=false
        default.debug=true
        kdevelop.plugins.clang.debug=true
    )"));
    QVERIFY(qputenv("KDEV_DISABLE_PLUGINS", "kdevcppsupport"));
    QVERIFY(qputenv("KDEV_CLANG_DISPLAY_DIAGS", "1"));
    AutoTestShell::init({QStringLiteral("kdevclangsupport")});
    TestCore::initialize();
    DUChain::self()->disablePersistentStorage();
    Core::self()->languageController()->backgroundParser()->setDelay(0);
    Core::self()->sourceFormatterController()->disableSourceFormatting(true);
    CodeRepresentation::setDiskChangesForbidden(true);

    globalTestLock = new ForegroundLock;
}

void TestAssistants::cleanupTestCase()
{
    Core::self()->cleanup();
    delete globalTestLock;
    globalTestLock = 0;
}

static QUrl createFile(const QString& fileContents, QString extension, int id)
{
    static QTemporaryDir tempDirA;
    Q_ASSERT(tempDirA.isValid());
    static QDir dirA(tempDirA.path());
    QFile file(dirA.filePath(QString::number(id) + extension));
    file.open(QIODevice::WriteOnly | QIODevice::Text);
    file.write(fileContents.toUtf8());
    file.close();
    return QUrl::fromLocalFile(file.fileName());
}

class Testbed
{
public:
    enum TestDoc
    {
        HeaderDoc,
        CppDoc
    };

    enum IncludeBehavior
    {
        NoAutoInclude,
        AutoInclude,
    };

    Testbed(const QString& headerContents, const QString& cppContents, IncludeBehavior include = AutoInclude)
        : m_includeBehavior(include)
    {
        static int i = 0;
        int id = i;
        ++i;
        m_headerDocument.url = createFile(headerContents,".h",id);
        m_headerDocument.textDoc = openDocument(m_headerDocument.url);

        QString preamble;
        if (include == AutoInclude)
            preamble = QStringLiteral("#include \"%1\"\n").arg(m_headerDocument.url.toLocalFile());
        m_cppDocument.url = createFile(preamble + cppContents,".cpp",id);
        m_cppDocument.textDoc = openDocument(m_cppDocument.url);
    }
    ~Testbed()
    {
        Core::self()->documentController()->documentForUrl(m_cppDocument.url)->textDocument();
        Core::self()->documentController()->documentForUrl(m_cppDocument.url)->close(KDevelop::IDocument::Discard);
        Core::self()->documentController()->documentForUrl(m_headerDocument.url)->close(KDevelop::IDocument::Discard);

        staticAssistantsManager()->hideAssistant();
    }

    void changeDocument(TestDoc which, Range where, const QString& what, bool waitForUpdate = false)
    {
        TestDocument document;
        if (which == CppDoc)
        {
            document = m_cppDocument;
            if (m_includeBehavior == AutoInclude) {
                where = Range(where.start().line() + 1, where.start().column(),
                              where.end().line() + 1, where.end().column()); //The include adds a line
            }
        }
        else {
            document = m_headerDocument;
        }
        // we must activate the document, otherwise we cannot find the correct active view
        auto kdevdoc = ICore::self()->documentController()->documentForUrl(document.url);
        QVERIFY(kdevdoc);
        ICore::self()->documentController()->activateDocument(kdevdoc);
        auto view = ICore::self()->documentController()->activeTextDocumentView();
        QCOMPARE(view->document(), document.textDoc);

        view->setSelection(where);
        view->removeSelectionText();
        view->setCursorPosition(where.start());
        view->insertText(what);
        QCoreApplication::processEvents();
        if (waitForUpdate) {
            DUChain::self()->waitForUpdate(IndexedString(document.url), KDevelop::TopDUContext::AllDeclarationsAndContexts);
        }
    }

    QString documentText(TestDoc which)
    {
        if (which == CppDoc)
        {
            //The CPP document text shouldn't include the autogenerated include line
            QString text = m_cppDocument.textDoc->text();
            return m_includeBehavior == AutoInclude ? text.mid(text.indexOf("\n") + 1) : text;
        }
        else
            return m_headerDocument.textDoc->text();
    }

    QString includeFileName() const
    {
        return m_headerDocument.url.toLocalFile();
    }

    KTextEditor::Document *document(TestDoc which) const
    {
        return Core::self()->documentController()->documentForUrl(
            which == CppDoc ? m_cppDocument.url : m_headerDocument.url)->textDocument();
    }

private:
    struct TestDocument {
        QUrl url;
        Document *textDoc;
    };

    Document* openDocument(const QUrl& url)
    {
        Core::self()->documentController()->openDocument(url);
        DUChain::self()->waitForUpdate(IndexedString(url), KDevelop::TopDUContext::AllDeclarationsAndContexts);
        return Core::self()->documentController()->documentForUrl(url)->textDocument();
    }

    IncludeBehavior m_includeBehavior;
    TestDocument m_headerDocument;
    TestDocument m_cppDocument;
};


/**
 * A StateChange describes an insertion/deletion/replacement and the expected result
**/
struct StateChange
{
    StateChange(){};
    StateChange(Testbed::TestDoc document, const Range& range, const QString& newText, const QString& result)
        : document(document)
        , range(range)
        , newText(newText)
        , result(result)
    {
    }
    Testbed::TestDoc document;
    Range range;
    QString newText;
    QString result;
};

Q_DECLARE_METATYPE(StateChange)
Q_DECLARE_METATYPE(QList<StateChange>)


void TestAssistants::testRenameAssistant_data()
{
    QTest::addColumn<QString>("fileContents");
    QTest::addColumn<QString>("oldDeclarationName");
    QTest::addColumn<QList<StateChange> >("stateChanges");
    QTest::addColumn<QString>("finalFileContents");

    QTest::newRow("Prepend Text")
        << "int foo(int i)\n { i = 0; return i; }"
        << "i"
        << QList<StateChange>{
            StateChange(Testbed::CppDoc, Range(0,12,0,12), "u", "ui"),
            StateChange(Testbed::CppDoc, Range(0,13,0,13), "z", "uzi"),
        }
        << "int foo(int uzi)\n { uzi = 0; return uzi; }";

    QTest::newRow("Append Text")
        << "int foo(int i)\n { i = 0; return i; }"
        << "i"
        << (QList<StateChange>() << StateChange(Testbed::CppDoc, Range(0,13,0,13), "d", "id"))
        << "int foo(int id)\n { id = 0; return id; }";

    QTest::newRow("Replace Text")
        << "int foo(int i)\n { i = 0; return i; }"
        << "i"
        << (QList<StateChange>() << StateChange(Testbed::CppDoc, Range(0,12,0,13), "u", "u"))
        << "int foo(int u)\n { u = 0; return u; }";

    QTest::newRow("Letter-by-Letter")
        << "int foo(int i)\n { i = 0; return i; }"
        << "i"
        << (QList<StateChange>()
            << StateChange(Testbed::CppDoc, Range(0,12,0,13), "", "")
            << StateChange(Testbed::CppDoc, Range(0,12,0,12), "a", "a")
            << StateChange(Testbed::CppDoc, Range(0,13,0,13), "b", "ab")
            << StateChange(Testbed::CppDoc, Range(0,14,0,14), "c", "abc")
        )
        << "int foo(int abc)\n { abc = 0; return abc; }";

    QTest::newRow("Paste Replace")
        << "int foo(int abg)\n { abg = 0; return abg; }"
        << "abg"
        << (QList<StateChange>() << StateChange(Testbed::CppDoc, Range(0,12,0,15), "abcdefg", "abcdefg"))
        << "int foo(int abcdefg)\n { abcdefg = 0; return abcdefg; }";

    QTest::newRow("Paste Insert")
        << "int foo(int abg)\n { abg = 0; return abg; }"
        << "abg"
        << (QList<StateChange>() << StateChange(Testbed::CppDoc, Range(0,14,0,14), "cdef", "abcdefg"))
        << "int foo(int abcdefg)\n { abcdefg = 0; return abcdefg; }";

    QTest::newRow("Letter-by-Letter Insert")
        << "int foo(int abg)\n { abg = 0; return abg; }"
        << "abg"
        << (QList<StateChange>()
            << StateChange(Testbed::CppDoc, Range(0,14,0,14), "c", "abcg")
            << StateChange(Testbed::CppDoc, Range(0,15,0,15), "d", "abcdg")
            << StateChange(Testbed::CppDoc, Range(0,16,0,16), "e", "abcdeg")
            << StateChange(Testbed::CppDoc, Range(0,17,0,17), "f", "abcdefg")
        )
        << "int foo(int abcdefg)\n { abcdefg = 0; return abcdefg; }";
}

void TestAssistants::testRenameAssistant()
{
    QFETCH(QString, fileContents);
    Testbed testbed("", fileContents);

    QFETCH(QString, oldDeclarationName);
    QFETCH(QList<StateChange>, stateChanges);
    foreach(StateChange stateChange, stateChanges)
    {
        testbed.changeDocument(Testbed::CppDoc, stateChange.range, stateChange.newText);
        if (stateChange.result.isEmpty()) {
            QVERIFY(!staticAssistantsManager()->activeAssistant() || !staticAssistantsManager()->activeAssistant()->actions().size());
        } else {
            QVERIFY(staticAssistantsManager()->activeAssistant() && staticAssistantsManager()->activeAssistant()->actions().size());
            RenameAction *r = qobject_cast<RenameAction*>(staticAssistantsManager()->activeAssistant()->actions().first().data());
            QCOMPARE(r->oldDeclarationName(), oldDeclarationName);
            QCOMPARE(r->newDeclarationName(), stateChange.result);
        }
    }
    if (staticAssistantsManager()->activeAssistant() && staticAssistantsManager()->activeAssistant()->actions().size()) {
            staticAssistantsManager()->activeAssistant()->actions().first()->execute();
    }
    QFETCH(QString, finalFileContents);
    QCOMPARE(testbed.documentText(Testbed::CppDoc), finalFileContents);
}

void TestAssistants::testRenameAssistantUndoRename()
{
    Testbed testbed("", "int foo(int i)\n { i = 0; return i; }");
    testbed.changeDocument(Testbed::CppDoc, Range(0,13,0,13), "d");
    QVERIFY(staticAssistantsManager()->activeAssistant());
    QVERIFY(staticAssistantsManager()->activeAssistant()->actions().size() > 0);
    RenameAction *r = qobject_cast<RenameAction*>(staticAssistantsManager()->activeAssistant()->actions().first().data());
    QVERIFY(r);

    // now rename the variable back to its original identifier
    testbed.changeDocument(Testbed::CppDoc, Range(0,13,0,14), "");
    // there should be no assistant anymore
    QVERIFY(!staticAssistantsManager()->activeAssistant());
}

const QString SHOULD_ASSIST = "SHOULD_ASSIST"; //An assistant will be visible
const QString NO_ASSIST = "NO_ASSIST";               //No assistant visible

void TestAssistants::testSignatureAssistant_data()
{
    QTest::addColumn<QString>("headerContents");
    QTest::addColumn<QString>("cppContents");
    QTest::addColumn<QList<StateChange> >("stateChanges");
    QTest::addColumn<QString>("finalHeaderContents");
    QTest::addColumn<QString>("finalCppContents");

    QTest::newRow("change_argument_type")
      << "class Foo {\nint bar(int a, char* b, int c = 10); \n};"
      << "int Foo::bar(int a, char* b, int c)\n{ a = c; b = new char; return a + *b; }"
      << (QList<StateChange>() << StateChange(Testbed::HeaderDoc, Range(1,8,1,11), "char", SHOULD_ASSIST))
      << "class Foo {\nint bar(char a, char* b, int c = 10); \n};"
      << "int Foo::bar(char a, char* b, int c)\n{ a = c; b = new char; return a + *b; }";

    QTest::newRow("prepend_arg_header")
      << "class Foo { void bar(int i); };"
      << "void Foo::bar(int i)\n{}"
      << (QList<StateChange>() << StateChange(Testbed::HeaderDoc, Range(0, 21, 0, 21), "char c, ", SHOULD_ASSIST))
      << "class Foo { void bar(char c, int i); };"
      << "void Foo::bar(char c, int i)\n{}";

    QTest::newRow("prepend_arg_cpp")
      << "class Foo { void bar(int i); };"
      << "void Foo::bar(int i)\n{}"
      << (QList<StateChange>() << StateChange(Testbed::CppDoc, Range(0, 14, 0, 14), "char c, ", SHOULD_ASSIST))
      << "class Foo { void bar(char c, int i); };"
      << "void Foo::bar(char c, int i)\n{}";

    QTest::newRow("change_default_parameter")
        << "class Foo {\nint bar(int a, char* b, int c = 10); \n};"
        << "int Foo::bar(int a, char* b, int c)\n{ a = c; b = new char; return a + *b; }"
        << (QList<StateChange>() << StateChange(Testbed::HeaderDoc, Range(1,29,1,34), "", NO_ASSIST))
        << "class Foo {\nint bar(int a, char* b, int c); \n};"
        << "int Foo::bar(int a, char* b, int c)\n{ a = c; b = new char; return a + *b; }";

    QTest::newRow("change_function_type")
        << "class Foo {\nint bar(int a, char* b, int c = 10); \n};"
        << "int Foo::bar(int a, char* b, int c)\n{ a = c; b = new char; return a + *b; }"
        << (QList<StateChange>() << StateChange(Testbed::CppDoc, Range(0,0,0,3), "char", SHOULD_ASSIST))
        << "class Foo {\nchar bar(int a, char* b, int c = 10); \n};"
        << "char Foo::bar(int a, char* b, int c)\n{ a = c; b = new char; return a + *b; }";

    QTest::newRow("swap_args_definition_side")
        << "class Foo {\nint bar(int a, char* b, int c = 10); \n};"
        << "int Foo::bar(int a, char* b, int c)\n{ a = c; b = new char; return a + *b; }"
        << (QList<StateChange>() << StateChange(Testbed::CppDoc, Range(0,13,0,28), "char* b, int a,", SHOULD_ASSIST))
        << "class Foo {\nint bar(char* b, int a, int c = 10); \n};"
        << "int Foo::bar(char* b, int a, int c)\n{ a = c; b = new char; return a + *b; }";

    // see https://bugs.kde.org/show_bug.cgi?id=299393
    // actually related to the whitespaces in the header...
    QTest::newRow("change_function_constness")
        << "class Foo {\nvoid bar(const Foo&) const;\n};"
        << "void Foo::bar(const Foo&) const\n{}"
        << (QList<StateChange>() << StateChange(Testbed::CppDoc, Range(0,25,0,31), "", SHOULD_ASSIST))
        << "class Foo {\nvoid bar(const Foo&);\n};"
        << "void Foo::bar(const Foo&)\n{}";

    // see https://bugs.kde.org/show_bug.cgi?id=356179
    QTest::newRow("keep_static_cpp")
        << "class Foo { static void bar(int i); };"
        << "void Foo::bar(int i)\n{}"
        << (QList<StateChange>() << StateChange(Testbed::CppDoc, Range(0, 19, 0, 19), ", char c", SHOULD_ASSIST))
        << "class Foo { static void bar(int i, char c); };"
        << "void Foo::bar(int i, char c)\n{}";
    QTest::newRow("keep_static_header")
        << "class Foo { static void bar(int i); };"
        << "void Foo::bar(int i)\n{}"
        << (QList<StateChange>() << StateChange(Testbed::HeaderDoc, Range(0, 33, 0, 33), ", char c", SHOULD_ASSIST))
        << "class Foo { static void bar(int i, char c); };"
        << "void Foo::bar(int i, char c)\n{}";

    // see https://bugs.kde.org/show_bug.cgi?id=356178
    QTest::newRow("keep_default_args_cpp_before")
        << "class Foo { void bar(bool b, int i = 0); };"
        << "void Foo::bar(bool b, int i)\n{}"
        << (QList<StateChange>() << StateChange(Testbed::CppDoc, Range(0, 14, 0, 14), "char c, ", SHOULD_ASSIST))
        << "class Foo { void bar(char c, bool b, int i = 0); };"
        << "void Foo::bar(char c, bool b, int i)\n{}";
    QTest::newRow("keep_default_args_cpp_after")
        << "class Foo { void bar(bool b, int i = 0); };"
        << "void Foo::bar(bool b, int i)\n{}"
        << (QList<StateChange>() << StateChange(Testbed::CppDoc, Range(0, 27, 0, 27), ", char c", SHOULD_ASSIST))
        << "class Foo { void bar(bool b, int i = 0, char c = {} /* TODO */); };"
        << "void Foo::bar(bool b, int i, char c)\n{}";
    QTest::newRow("keep_default_args_header_before")
        << "class Foo { void bar(bool b, int i = 0); };"
        << "void Foo::bar(bool b, int i)\n{}"
        << (QList<StateChange>() << StateChange(Testbed::HeaderDoc, Range(0, 29, 0, 29), "char c = 'A', ", SHOULD_ASSIST))
        << "class Foo { void bar(bool b, char c = 'A', int i = 0); };"
        << "void Foo::bar(bool b, char c, int i)\n{}";
    QTest::newRow("keep_default_args_header_after")
        << "class Foo { void bar(bool b, int i = 0); };"
        << "void Foo::bar(bool b, int i)\n{}"
        << (QList<StateChange>() << StateChange(Testbed::HeaderDoc, Range(0, 38, 0, 38), ", char c = 'A'", SHOULD_ASSIST))
        << "class Foo { void bar(bool b, int i = 0, char c = 'A'); };"
        << "void Foo::bar(bool b, int i, char c)\n{}";

    // see https://bugs.kde.org/show_bug.cgi?id=355356
    QTest::newRow("no_retval_on_ctor")
        << "class Foo { Foo(); };"
        << "Foo::Foo()\n{}"
        << (QList<StateChange>() << StateChange(Testbed::HeaderDoc, Range(0, 16, 0, 16), "char c", SHOULD_ASSIST))
        << "class Foo { Foo(char c); };"
        << "Foo::Foo(char c)\n{}";
}

void TestAssistants::testSignatureAssistant()
{
    QFETCH(QString, headerContents);
    QFETCH(QString, cppContents);
    Testbed testbed(headerContents, cppContents);

    QFETCH(QList<StateChange>, stateChanges);
    foreach (StateChange stateChange, stateChanges)
    {
        testbed.changeDocument(stateChange.document, stateChange.range, stateChange.newText, true);

        if (stateChange.result == SHOULD_ASSIST) {
            QEXPECT_FAIL("change_function_type", "Clang sees that return type of out-of-line definition differs from that in the declaration and won't parse the code...", Abort);
            QVERIFY(staticAssistantsManager()->activeAssistant() && !staticAssistantsManager()->activeAssistant()->actions().isEmpty());
        } else {
            QVERIFY(!staticAssistantsManager()->activeAssistant() || staticAssistantsManager()->activeAssistant()->actions().isEmpty());
        }
    }
    if (staticAssistantsManager()->activeAssistant() && !staticAssistantsManager()->activeAssistant()->actions().isEmpty())
        staticAssistantsManager()->activeAssistant()->actions().first()->execute();

    QFETCH(QString, finalHeaderContents);
    QFETCH(QString, finalCppContents);
    QCOMPARE(testbed.documentText(Testbed::HeaderDoc), finalHeaderContents);
    QCOMPARE(testbed.documentText(Testbed::CppDoc), finalCppContents);
}

enum UnknownDeclarationActions
{
    NoUnknownDeclaration = 0x0,
    ForwardDecls = 0x1,
    MissingInclude = 0x2
};

Q_DECLARE_METATYPE(UnknownDeclarationActions)

void TestAssistants::testUnknownDeclarationAssistant_data()
{
    QTest::addColumn<QString>("headerContents");
    QTest::addColumn<QString>("globalText");
    QTest::addColumn<QString>("functionText");
    QTest::addColumn<UnknownDeclarationActions>("actions");

    QTest::newRow("unincluded_struct") << "struct test{};" << "" << "test"
        << static_cast<UnknownDeclarationActions>(ForwardDecls | MissingInclude);
    QTest::newRow("forward_declared_struct") << "struct test{};" << "struct test;" << "test *f; f->"
        << static_cast<UnknownDeclarationActions>(MissingInclude);
    QTest::newRow("unknown_struct") << "" << "" << "test"
        << static_cast<UnknownDeclarationActions>(ForwardDecls);
}

void TestAssistants::testUnknownDeclarationAssistant()
{
    QFETCH(QString, headerContents);
    QFETCH(QString, globalText);
    QFETCH(QString, functionText);
    QFETCH(UnknownDeclarationActions, actions);

    static const auto cppContents = QStringLiteral("%1\nvoid f_u_n_c_t_i_o_n() {\n}");
    Testbed testbed(headerContents, cppContents.arg(globalText), Testbed::NoAutoInclude);
    const int line = testbed.document(Testbed::CppDoc)->lines() - 1;
    testbed.changeDocument(Testbed::CppDoc, Range(line, 0, line, 0), functionText, true);

    if (actions == NoUnknownDeclaration) {
        QVERIFY(!staticAssistantsManager()->activeAssistant());
        return;
    }

    QVERIFY(staticAssistantsManager()->activeAssistant());
    const auto assistantActions = staticAssistantsManager()->activeAssistant()->actions();
    QStringList actionDescriptions;
    for (auto action: assistantActions) {
        actionDescriptions << action->description();
    }

    {
        const bool hasForwardDecls =
            actionDescriptions.contains(QObject::tr("Forward declare as 'struct'")) &&
            actionDescriptions.contains(QObject::tr("Forward declare as 'class'"));
        QCOMPARE(hasForwardDecls, static_cast<bool>(actions & ForwardDecls));
    }

    {
        auto fileName = testbed.includeFileName();
        fileName = fileName.mid(fileName.lastIndexOf('/') + 1);
        const auto description = QObject::tr("Insert \'%1\'")
            .arg(QStringLiteral("#include \"%1\"").arg(fileName));
        const bool hasMissingInclude = actionDescriptions.contains(description);
        QCOMPARE(hasMissingInclude, static_cast<bool>(actions & MissingInclude));
    }
}
