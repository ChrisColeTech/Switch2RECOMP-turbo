// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
// Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
// documentation fixture: actual launcher widgets, disposable data, no game compilation or execution
#include "window.h"
#include "theme.h"
#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUuid>
#include <QtTest>
#include <cstdio>

static void check(bool okay,const char* what){if(!okay)qFatal("Capture check failed: %s",what);}
static void write(const QString& path,const QByteArray& bytes){
    check(QDir().mkpath(QFileInfo(path).absolutePath()),"fixture directory");
    QFile file(path);check(file.open(QIODevice::WriteOnly),"fixture file");check(file.write(bytes)==bytes.size(),"fixture bytes");
}
static QAction* action(Window& window,const QString& text){
    for(auto* a:window.findChildren<QAction*>())if(a->text()==text)return a;
    qFatal("Action not found: %s",qPrintable(text));return nullptr;
}
static QDialog* modal(){auto* d=qobject_cast<QDialog*>(QApplication::activeModalWidget());check(d,"modal dialog");return d;}
static void shot(QWidget* widget,const QString& output,const QString& name){
    QTest::qWait(180);auto image=widget->grab();
    // omit the file chooser's absolute address row; all remaining pixels are unmodified Qt output
    if(qobject_cast<QFileDialog*>(widget))image=image.copy(0,50,image.width(),image.height()-50);
    check(image.save(QDir(output).filePath(name+".png")),"PNG capture");
}
static void artwork(const QString& path,const QColor& color,const QString& title){
    QImage image(600,400,QImage::Format_RGB32);image.fill(QColor("#15202b"));
    QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);p.setPen(Qt::NoPen);p.setBrush(color);
    p.drawEllipse(QRectF(270,-60,300,300));p.setBrush(QColor("#263749"));p.drawRoundedRect(QRectF(40,80,280,180),26,26);
    p.setPen(Qt::white);p.setFont(QFont("Segoe UI",26,QFont::Bold));p.drawText(QRect(40,255,530,70),Qt::AlignLeft|Qt::TextWordWrap,title);
    p.setFont(QFont("Segoe UI",24));p.drawText(40,355,"DEMO ART");p.end();
    check(QDir().mkpath(QFileInfo(path).absolutePath()),"art directory");check(image.save(path),"demo artwork");
}
static QFileDialog* fileDialog(const QString& directory){
    auto* d=qobject_cast<QFileDialog*>(modal());check(d,"actual QFileDialog");
    d->setSidebarUrls({QUrl::fromLocalFile(directory)});d->setHistory({});d->setDirectory(directory);
    d->resize(850,480);
    if(auto* tree=d->findChild<QTreeView*>("treeView")){
        tree->header()->resizeSection(0,300);tree->header()->resizeSection(1,85);
        tree->header()->resizeSection(2,140);tree->header()->resizeSection(3,170);
    }
    return d;
}
static void chooseFile(QFileDialog* dialog,const QString& name){
    auto* edit=dialog->findChild<QLineEdit*>("fileNameEdit");check(edit,"Qt file-name field");edit->setText(name);
}

int main(int argc,char** argv){
    qInstallMessageHandler([](QtMsgType,const QMessageLogContext&,const QString& text){fprintf(stderr,"%s\n",qPrintable(text));fflush(stderr);});
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc,argv);
    check(argc==3,"usage: launcher-capture TOOLKIT OUTPUT_DIRECTORY");
    const QString root=QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
    const QString output=QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
    check(QDir().mkpath(output),"output directory");
    QTemporaryDir temporary;check(temporary.isValid(),"temporary fixture");
    const QString previous=QDir::currentPath();check(QDir::setCurrent(temporary.path()),"fixture working directory");
    // isolate Qt's file-dialog history and the key installer from the real application identity
    QApplication::setOrganizationName("SwitchRecompDocFixture-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    QApplication::setApplicationName("Capture");QStandardPaths::setTestModeEnabled(true);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temporary.filePath("settings"));
    const QString privateData=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    check(!QFileInfo::exists(privateData),"unique fixture application data");
    QApplication::setStyle("Fusion");
#ifdef Q_OS_WIN
    for(const auto* file:{"segoeui.ttf","segoeuib.ttf"})check(QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR")+"/Fonts/"+file)>=0,"system font for offscreen Qt");
#endif
    app.setFont(QFont("Segoe UI",10));Theme::apply(app);

    write("Sources/Demo application.nsp","PFS0"+QByteArray(16,'\0'));
    write("Sources/Demo package.nup",QByteArray("NUP\1",4));
    write("Sources/Demo dump/exefs/main","NSO0");
    write("Keys/prod.keys","# documentation fixture, not console keys\nheader_key = demonstration-only-not-a-key\n");
    const auto tools=Toolchain::find(root);check(tools.canBuild(),"real toolkit detected");
    Library library(temporary.filePath("library.json"));library.toolRoot=root;library.coverWidth=230;library.outputRoot="Builds";
    const QStringList titles{"Demo application","Demo package","Demo dump","Demo native"};
    const QStringList sources{"Sources/Demo application.nsp","Sources/Demo package.nup","Sources/Demo dump",{}};
    const QList<QColor> colors{QColor("#84cf5b"),QColor("#69c4d1"),QColor("#b69be8"),QColor("#e9ab76")};
    for(int i=0;i<titles.size();++i){
        Game game;game.id=QString::number(i);game.title=titles[i];game.source=sources[i];game.outputName=titles[i];game.buildDir="Builds/"+titles[i];
        game.cover="Artwork/"+titles[i]+".png";artwork(game.cover,colors[i],titles[i]);
        if(i==3){write(game.executable(),"demonstration only, never execute");write(game.codeModule(),"demonstration only");check(QDir().mkpath(game.dataDir()),"native data directory");}
        check(library.add(game),"demo library entry");
    }
    QString error;check(library.save(&error),"save disposable library");
    {
        Window window(library.path,root);window.resize(1080,600);window.show();window.selectTitle(titles[0]);
        QTest::qWait(350);shot(&window,output,"01-library-build-play");
        window.showBuildOutput();shot(&window,output,"06-build-output");window.findChild<QDockWidget*>("BuildOutput")->hide();
        auto* add=window.findChild<QToolButton*>("AddGameButton");check(add&&add->menu(),"add dropdown");
        add->menu()->popup(add->mapToGlobal(QPoint(0,add->height())));shot(add->menu(),output,"02-add-choices");add->menu()->hide();

        // drive the production Add game action, check its filter, and accept an NSP into its real Properties dialog
        QTimer::singleShot(100,&window,[&]{
            auto* d=fileDialog(temporary.filePath("Sources"));
            check(d->nameFilters().join(' ').contains("*.nsp"),"NSP offered in file filter");
            chooseFile(d,"Demo application.nsp");shot(d,output,"03-add-nsp-package");
            // a second source avoids the existing-library short circuit
            write("Sources/Demo import.nsp","PFS0"+QByteArray(16,'\0'));chooseFile(d,"Demo import.nsp");
            QTimer::singleShot(100,&window,[&]{
                auto* properties=modal();bool direct=false;
                for(auto* label:properties->findChildren<QLabel*>())direct|=label->text()=="DIRECT NSP";
                check(direct,"NSP classified in actual import dialog");
                properties->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
            });
            QMetaObject::invokeMethod(d,"accept",Qt::DirectConnection);
        });action(window,"Add game")->trigger();
        Library imported(library.path);check(imported.load(&error)&&imported.games.size()==5,"NSP persisted through UI import");
        window.selectTitle(titles[0]);
        for(const auto& pair:QList<QPair<int,QString>>{{0,"04-source-properties"},{1,"05-build-options"},{3,"08-artwork-properties"}}){
            QTimer::singleShot(100,&window,[&]{auto* d=modal();d->resize(780,430);shot(d,output,pair.second);d->reject();});
            window.showProperties(pair.first);
        }

        QTimer::singleShot(100,&window,[&]{auto* d=fileDialog(temporary.filePath("Builds/Demo native"));chooseFile(d,"Demo native"+QString(executableSuffix));shot(d,output,"07-import-native-build");d->reject();});
        action(window,QString::fromUtf8("Import build…"))->trigger();

        // use only the nonfunctional fixture key file; the installer checks presence of a header-key name, not cryptography
        QTimer::singleShot(100,&window,[&]{
            auto* d=fileDialog(temporary.filePath("Keys"));chooseFile(d,"prod.keys");shot(d,output,"09-install-keys");
            QTimer::singleShot(100,&window,[&]{
                auto* message=qobject_cast<QMessageBox*>(modal());check(message&&message->windowTitle()=="Keys installed","key install confirmation");
                shot(message,output,"10-keys-installed");message->accept();
            });QMetaObject::invokeMethod(d,"accept",Qt::DirectConnection);
        });action(window,QString::fromUtf8("Install keys…"))->trigger();
        Library installed(library.path);check(installed.load(&error),"load installed key path");
        check(installed.keysPath==QDir(privateData).filePath("keys/prod.keys"),"private key destination");
        QFile copied(installed.keysPath);check(copied.open(QIODevice::ReadOnly),"installed fixture key file");
        check(copied.readAll().contains("demonstration-only-not-a-key"),"fixture key copied");copied.close();

        // check real BuildManager guards and QProcess dispatch without invoking GCC or compiling a game
        BuildManager manager;Game game=gameFromSource(temporary.filePath("Sources/Demo import.nsp"));
        game.buildDir=temporary.filePath("dispatch");Toolchain dispatch=tools;dispatch.keys.clear();
        check(!manager.start(game,dispatch,2,&error)&&error.contains("Install keys"),"NSP missing-key guard");
        dispatch.root=temporary.filePath("toolkit");dispatch.keys=installed.keysPath;
        write(QDir(dispatch.root).filePath("switchrecomp.py"),
            "import os, sys\nfrom pathlib import Path\n"
            "assert Path(sys.argv[1]).name == 'Demo import.nsp'\n"
            "assert '--runtime' in sys.argv and '--cc' in sys.argv\n"
            "assert 'demonstration-only-not-a-key' in Path(os.environ['SWR_PROD_KEYS']).read_text()\n"
            "print('NSP_DISPATCH_VERIFIED', flush=True)\nsys.exit(23)\n");
        bool verified=false;QObject::connect(&manager,&BuildManager::output,&app,[&](const QString& text){verified|=text=="NSP_DISPATCH_VERIFIED";});
        QSignalSpy done(&manager,&BuildManager::finished);check(manager.start(game,dispatch,2,&error),"NSP process starts with installed keys");
        check(done.wait(10000)&&verified&&!done.first()[1].toBool(),"NSP args/key environment and failed-build state");
        window.close();
    }
    // remove only the UUID-named data owned by this invocation, never the user's launcher data
    check(QDir(privateData).removeRecursively(),"remove fixture key copy");
    QDir().rmdir(QFileInfo(privateData).absolutePath());check(QDir::setCurrent(previous),"restore working directory");
    qInfo("PASS: 10 actual Qt captures; NSP picker/import, key install, missing-key guard and build dispatch. No game compiled or launched.");
    return 0;
}
