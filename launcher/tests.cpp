// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
#include "buildmanager.h"
#include "theme.h"
#include "processwatch.h"
#include <QColor>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

class LauncherTests : public QObject {
    Q_OBJECT
private slots:
    void pythonPrecedenceAndEnvironment() {
        const auto oldPython=qgetenv("SWR_PYTHON"),oldPath=qgetenv("PATH");
        const bool hadPython=qEnvironmentVariableIsSet("SWR_PYTHON");
        const auto restore=qScopeGuard([&]{if(hadPython)qputenv("SWR_PYTHON",oldPython);else qunsetenv("SWR_PYTHON");qputenv("PATH",oldPath);});
        qunsetenv("SWR_PYTHON");
        QTemporaryDir tmp(QDir::tempPath()+"/swr python spaces-XXXXXX");QVERIFY(tmp.isValid());
#ifdef Q_OS_WIN
        const QString suffix="Scripts/python.exe",bundle="python/python.exe",fallback="path/python.exe";
#else
        const QString suffix="bin/python3",bundle="python/bin/python3",fallback="path/python3";
#endif
        const auto stage=[&](const QString& relative){const auto path=tmp.filePath(relative);QDir().mkpath(QFileInfo(path).absolutePath());return QFile::copy(QCoreApplication::applicationFilePath(),path);};
        QVERIFY(stage(fallback));QVERIFY(stage(bundle));QVERIFY(stage("venv/"+suffix));QVERIFY(stage(".venv/"+suffix));
        qputenv("PATH",(QDir::toNativeSeparators(tmp.filePath("path"))+QDir::listSeparator()).toUtf8()+oldPath);
        auto tools=Toolchain::find(tmp.path());QCOMPARE(tools.python,tmp.filePath(".venv/"+suffix));
        QCOMPARE(tools.environment().value("SWR_PYTHON"),tools.python);
        qputenv("SWR_PYTHON",tmp.filePath(bundle).toUtf8());QCOMPARE(Toolchain::find(tmp.path()).python,tmp.filePath(bundle));
        qputenv("SWR_PYTHON",tmp.filePath("missing.exe").toUtf8());QVERIFY(Toolchain::find(tmp.path()).python.isEmpty());
        qunsetenv("SWR_PYTHON");QVERIFY(QFile::remove(tmp.filePath(".venv/"+suffix)));
        QCOMPARE(Toolchain::find(tmp.path()).python,tmp.filePath("venv/"+suffix));
        QVERIFY(QFile::remove(tmp.filePath("venv/"+suffix)));QCOMPARE(Toolchain::find(tmp.path()).python,tmp.filePath(bundle));
        QVERIFY(QFile::remove(tmp.filePath(bundle)));QCOMPARE(Toolchain::find(tmp.path()).python,tmp.filePath(fallback));
    }
    void discoverNspBuildAndPreserveLibrary() {
        QTemporaryDir tmp;const auto build=tmp.filePath("MKW NSP build");QVERIFY(QDir().mkpath(build+"/Mario Kart World 180_data"));
        const auto source=tmp.filePath("Mario Kart World 1.8.0.nsp");
        for(const auto& path:{source,build+"/Mario Kart World 180"+executableSuffix,build+"/Mario Kart World 180"+moduleSuffix}){
            QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write("fixture");
        }
        {QFile marker(build+"/Mario Kart World 180_data/romfs.nsp");QVERIFY(marker.open(QIODevice::WriteOnly));marker.write("SWRNSP1\n"+QDir::toNativeSeparators(source).toUtf8()+"\n100\n200\n");}
        {QFile ini(build+"/Mario Kart World 180.ini");QVERIFY(ini.open(QIODevice::WriteOnly));ini.write("[Graphics]\nrenderer=vulkan\n");}
        Library library(tmp.filePath("library.json"));library.jobs=3;library.colorMode="light";
        auto old=gameFromSource(tmp.filePath("Older game.nup"));old.cover="my cover.png";QVERIFY(library.add(old));
        QCOMPARE(library.discoverBuilds(tmp.path()),1);QCOMPARE(library.games.size(),2);
        const auto game=library.games.back();QVERIFY(game.hasBuild());QCOMPARE(game.format(),QString("DIRECT NSP"));
        QCOMPARE(game.renderer,QString("vulkan"));QVERIFY(samePath(game.source,source));QVERIFY(game.sourceAvailable());
        QVERIFY(cardState(game,false,false,false,false).actionEnabled);
        QCOMPARE(library.discoverBuilds(tmp.path()),0);QCOMPARE(library.importBuild(game),game.id);
        QCOMPARE(library.games.front().json(),old.json());QCOMPARE(library.jobs,3);QCOMPARE(library.colorMode,QString("light"));
        Library pending(tmp.filePath("pending.json"));auto unbuilt=gameFromSource(source);unbuilt.cover="chosen.png";unbuilt.title="My MKW";unbuilt.workers=5;pending.add(unbuilt);
        QCOMPARE(pending.importBuild(game),unbuilt.id);QCOMPARE(pending.games.size(),1);QVERIFY(pending.games.front().hasBuild());
        QCOMPARE(pending.games.front().cover,unbuilt.cover);QCOMPARE(pending.games.front().title,unbuilt.title);QCOMPARE(pending.games.front().workers,5);
        QVERIFY(QFile::remove(source));QCOMPARE(cardState(library.games.back(),false,false,false,true).status,QString("Source offline"));
        QVERIFY(QFile::remove(game.codeModule()));QVERIFY(!library.games.back().hasBuild());
    }
    void themeTextContrast() {
        const auto luminance=[](const QColor& color){
            const auto linear=[](double v){return v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4);};
            return .2126*linear(color.redF())+.7152*linear(color.greenF())+.0722*linear(color.blueF());
        };
        const auto contrast=[&](QColor a,QColor b){double x=luminance(a),y=luminance(b);return (qMax(x,y)+.05)/(qMin(x,y)+.05);};
        for(const auto& mode:{"light","dark"})for(const auto& accent:Theme::accents()){
            const auto colors=Theme::colorsFor(mode,accent.id);
            for(const auto& background:{colors.canvas,colors.surface,colors.raised,colors.selected}) {
                QVERIFY(contrast(colors.text,background)>=4.5);
                QVERIFY(contrast(colors.muted,background)>=4.5);
            }
            QVERIFY(contrast(colors.accentText,colors.accent)>=4.5);
        }
    }
    void searchAndFiltersCombine() {
        Game game;game.title="Donkey Kong Bananza";game.source="P:/Bananza.nup";
        LibraryFilter filter{"BANAN",2,1};QVERIFY(filter.matches(game));QCOMPARE(filter.activeCount(),3);
        filter.source=2;QVERIFY(!filter.matches(game));filter.source=1;
        filter.state=3;QVERIFY(!filter.matches(game));QVERIFY(filter.matches(game,true));
        filter.state=2;QVERIFY(!filter.matches(game,false,true));
        filter.state=1;QVERIFY(!filter.matches(game));
        filter={};QVERIFY(filter.matches(game));QCOMPARE(filter.activeCount(),0);
    }
    void rendererPreservesSettings() {
        QTemporaryDir tmp; const QString path=tmp.filePath("Game.ini");
        const QByteArray original="; keep this comment\r\n[Graphics]\r\nrenderer=opengl\r\nresolution=0.75\r\n[Audio]\r\naudio_slowdown=0\r\n";
        {QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(original);}
        QString error; QVERIFY2(writeRenderer(path,"vulkan",&error),qPrintable(error));
        QFile result(path); QVERIFY(result.open(QIODevice::ReadOnly)); const auto text=result.readAll();
        QVERIFY(text.contains("renderer=vulkan\r\n")); QVERIFY(text.contains("resolution=0.75"));
        QVERIFY(text.contains("; keep this comment")); QVERIFY(text.contains("audio_slowdown=0"));
    }
    void libraryRoundTripAndCorruption() {
        QTemporaryDir tmp; const QString file=tmp.filePath("nested/library.json");
        Library lib(file); Game game=gameFromSource(tmp.filePath("My Game [v1].nup"));
        game.cover="art.png"; game.background="wallpaper.png"; game.renderer="vulkan"; game.workers=3; game.optimization=1;
        lib.add(game); lib.jobs=4; lib.viewMode=1; lib.coverWidth=220; lib.showBackground=false;lib.colorMode="light";lib.accentColor="violet";lib.keysPath="D:/Keys/prod.keys";
        QString error; QVERIFY2(lib.save(&error),qPrintable(error));
        Library loaded(file); QVERIFY(loaded.load(&error)); QCOMPARE(loaded.games.size(),1); QCOMPARE(loaded.jobs,4);
        QCOMPARE(loaded.games.front().source,game.source); QCOMPARE(loaded.games.front().title,QString("My Game"));
        QCOMPARE(loaded.games.front().background,QString("wallpaper.png")); QCOMPARE(loaded.games.front().workers,3);
        QCOMPARE(loaded.viewMode,1); QCOMPARE(loaded.coverWidth,220); QVERIFY(!loaded.showBackground);
        QCOMPARE(loaded.colorMode,QString("light"));QCOMPARE(loaded.accentColor,QString("violet"));
        QCOMPARE(loaded.keysPath,QString("D:/Keys/prod.keys"));
        QVERIFY(!loaded.add(game));
        {QFile f(file); QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::Truncate)); f.write("bad json");}
        Library broken(file); QVERIFY(!broken.load(&error)); QVERIFY(!broken.save(&error));
        QFile f(file); QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(),QByteArray("bad json"));
    }
    void argumentsPreserveLiteralPaths() {
        Game game; game.source="P:/Games/Name [v1] & friends.nup"; game.buildDir="P:/Build [x]"; game.outputName="Name";
        Toolchain t; t.root="D:/Toolkit path"; t.compiler="D:/GCC path/gcc.exe"; t.runtime="D:/Toolkit path/bin/runtime.exe";
        const auto args=BuildManager::arguments(game,t,8);
        QVERIFY(args.contains(game.source)); QVERIFY(args.contains(game.buildDir)); QVERIFY(args.contains(t.compiler));
        QCOMPARE(args[args.indexOf("--jobs")+1],QString("8"));
        game.workers=3;game.optimization=1;const auto perGame=BuildManager::arguments(game,t,8);
        QCOMPARE(perGame[perGame.indexOf("--jobs")+1],QString("3"));
        QCOMPARE(perGame[perGame.indexOf("-O")+1],QString("1"));
    }
    void importChecksAllArtifacts() {
        QTemporaryDir tmp; const QString exe=tmp.filePath(QString("My Game")+executableSuffix); QString error; Game g;
        {QFile f(exe); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("test");}
        QVERIFY(!gameFromExecutable(exe,&g,&error));
        {QFile f(tmp.filePath(QString("My Game")+moduleSuffix)); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("test");}
        QVERIFY(QDir().mkpath(tmp.filePath("My Game_data")));
        QVERIFY(gameFromExecutable(exe,&g,&error)); QVERIFY(g.hasBuild());
        const auto ready=cardState(g,false,false,true,false);QCOMPARE(ready.action,QString("Play"));QVERIFY(ready.actionEnabled);QVERIFY(!ready.needsAttention);
        g.buildIncomplete=true; QVERIFY(!g.hasBuild()); g.buildIncomplete=false;
        g.source=tmp.filePath("Missing.nup");g.builtSource=QDir::toNativeSeparators(g.source);QVERIFY(g.hasBuild());
        const auto offline=cardState(g,false,false,false,true);QVERIFY(!offline.actionEnabled);QVERIFY(offline.needsAttention);QCOMPARE(offline.status,QString("Source offline"));
        g.source="another.nup"; QVERIFY(!g.hasBuild());
    }
    void perCardActionTargetsCorrectOperation(){
        QTemporaryDir tmp;Game g=gameFromSource(tmp.filePath("Game.nup"));
        {QFile f(g.source);QVERIFY(f.open(QIODevice::WriteOnly));f.write("NUP\1");}
        auto state=cardState(g,false,false,false,true);QCOMPARE(state.action,QString("Build"));QVERIFY(state.needsAttention);QVERIFY(state.actionEnabled);
        QVERIFY(!cardState(g,false,false,true,true).actionEnabled);
        state=cardState(g,true,false,true,true);QCOMPARE(state.action,QString("Stop"));QVERIFY(state.actionEnabled);
        state=cardState(g,false,true,true,true);QCOMPARE(state.action,QString("Cancel build"));QVERIFY(state.actionEnabled);
    }
    void matchesRunningExecutableByFullPath(){
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
        Game self;self.id="self";QFileInfo executable(QCoreApplication::applicationFilePath());self.outputName=executable.completeBaseName();self.buildDir=executable.absolutePath();
        QTemporaryDir tmp;Game different=self;different.id="different";different.buildDir=tmp.path();
        const auto running=findRunningGames({self,different});QCOMPARE(running.value(self.id),QCoreApplication::applicationPid());QVERIFY(!running.contains(different.id));
        QVERIFY(!requestGameClose(QCoreApplication::applicationPid(),different.executable()));
#endif
    }
    void pathFormattingDoesNotInvalidateBuild(){
#ifdef Q_OS_WIN
        QVERIFY(samePath("P:\\Games\\Title.nup","P:/Games/Title.nup"));
#endif
    }
    void nspSourceClassification(){
        QTemporaryDir tmp;const QString path=tmp.filePath("Game.nsp");
        {QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write("PFS0");file.write(QByteArray(16,'\0'));}
        QString error;QVERIFY2(validSource(path,&error),qPrintable(error));
        const auto game=gameFromSource(path);QCOMPARE(game.format(),QString("DIRECT NSP"));QVERIFY(inPlacePackage(path));
        LibraryFilter filter;filter.source=1;QVERIFY(filter.matches(game));
    }
    void toolEnvironmentCarriesKeys(){
        Toolchain tools;tools.keys="D:/Keys/prod.keys";
        QCOMPARE(tools.environment().value("SWR_PROD_KEYS"),tools.keys);
    }
};
int main(int argc,char** argv) {
    // copied into fixture tool directories to exercise executable probes without another Python installation
    if(argc>1&&QByteArray(argv[1])=="-c")return 0;
    QCoreApplication app(argc,argv);LauncherTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "tests.moc"
