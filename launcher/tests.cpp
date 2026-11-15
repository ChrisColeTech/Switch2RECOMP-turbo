// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
#include "buildmanager.h"
#include "theme.h"
#include "processwatch.h"
#include <QColor>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

class LauncherTests : public QObject {
    Q_OBJECT
private slots:
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
        lib.add(game); lib.jobs=4; lib.viewMode=1; lib.coverWidth=220; lib.showBackground=false;lib.colorMode="light";lib.accentColor="violet";
        QString error; QVERIFY2(lib.save(&error),qPrintable(error));
        Library loaded(file); QVERIFY(loaded.load(&error)); QCOMPARE(loaded.games.size(),1); QCOMPARE(loaded.jobs,4);
        QCOMPARE(loaded.games.front().source,game.source); QCOMPARE(loaded.games.front().title,QString("My Game"));
        QCOMPARE(loaded.games.front().background,QString("wallpaper.png")); QCOMPARE(loaded.games.front().workers,3);
        QCOMPARE(loaded.viewMode,1); QCOMPARE(loaded.coverWidth,220); QVERIFY(!loaded.showBackground);
        QCOMPARE(loaded.colorMode,QString("light"));QCOMPARE(loaded.accentColor,QString("violet"));
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
};
QTEST_GUILESS_MAIN(LauncherTests)
#include "tests.moc"
