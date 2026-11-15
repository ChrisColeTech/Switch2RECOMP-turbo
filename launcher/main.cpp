// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
#include "window.h"
#include "theme.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QPainter>
#include <QImageReader>
#include <QStandardPaths>
#include <QTimer>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("MZLabs");
    QApplication::setApplicationName("SwitchRecompLauncher");
    QApplication::setApplicationVersion("0.1.0");
    QApplication::setStyle("Fusion");
    Theme::apply(app);
    QFont font("Segoe UI", 9); app.setFont(font);
    QApplication::setWindowIcon(Theme::appIcon());
    QCommandLineParser parser; parser.setApplicationDescription("SwitchRecomp by MZ Labs — native game library and build launcher");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"root", "SwitchRecomp toolkit directory", "directory"});
    parser.addOption({"library", "Use a specific library file", "file"});
    parser.addOption({"import-build", "Import an existing native game executable", "exe"});
    parser.addOption({"demo", "Show a non-persistent preview library"});
    parser.addOption({"list-view", "Start in the details-list view"});
    parser.addOption({"show-console", "Show the build-output dock"});
    parser.addOption({"show-properties", "Open the selected game's Properties dialog"});
    parser.addOption({"show-filters", "Open the library search and filter popout"});
    parser.addOption({"preview-theme", "Preview dark or light mode without saving", "mode"});
    parser.addOption({"preview-accent", "Preview an accent without saving", "color"});
    parser.addOption({"select-title", "Select a library game by display title", "title"});
    parser.addOption({"check-image", "Validate an artwork image using deployed Qt plugins", "file"});
    parser.addOption({"show-appearance", "Open appearance settings"});
    parser.addOption({"screenshot", "Save a launcher preview and exit", "png"});
    parser.addOption({"smoke-build", "Build a synthetic test input and exit (diagnostics)", "input"});
    parser.addOption({"build-output", "Output folder for --smoke-build", "directory"});
    parser.process(app);
    if(parser.isSet("check-image")){QImageReader reader(parser.value("check-image"));const auto image=reader.read();if(image.isNull()){qCritical("Artwork decode failed: %s",qPrintable(reader.errorString()));return 1;}qInfo("Artwork decoded: %d x %d",image.width(),image.height());return 0;}
    QString libraryPath=parser.value("library");
    if(libraryPath.isEmpty()) libraryPath=QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("library.json");
    Window window(libraryPath,parser.value("root"),parser.isSet("demo"));
    window.show();
    if(parser.isSet("import-build")) window.importBuild(parser.value("import-build"));
    if(parser.isSet("select-title"))window.selectTitle(parser.value("select-title"));
    if(parser.isSet("preview-theme")||parser.isSet("preview-accent"))window.setAppearance(parser.value("preview-theme").isEmpty()?"dark":parser.value("preview-theme"),parser.value("preview-accent").isEmpty()?"lime":parser.value("preview-accent"));
    if(parser.isSet("list-view")) window.setViewMode(1);
    if(parser.isSet("show-console")) window.showBuildOutput();
    if(parser.isSet("show-properties")) QTimer::singleShot(0,&window,[&window]{window.showProperties();});
    if(parser.isSet("show-filters")) QTimer::singleShot(0,&window,[&window]{window.showFilters();});
    if(parser.isSet("show-appearance")) QTimer::singleShot(0,&window,[&window]{window.showAppearance();});
    if(parser.isSet("smoke-build")) {
        if(parser.value("build-output").isEmpty() || parser.value("library").isEmpty()) return 2;
        QObject::connect(&window,&Window::smokeFinished,&app,[](bool okay){QApplication::exit(okay?0:1);});
        QTimer::singleShot(0,&window,[&] {
            if(!window.importSource(parser.value("smoke-build"),false,parser.value("build-output"))) { QApplication::exit(2); return; }
            window.startBuild();
        });
    }
    if(parser.isSet("screenshot")) {
        QTimer::singleShot(1100,&window,[&] {
            const QString path=QFileInfo(parser.value("screenshot")).absoluteFilePath();
            QDir().mkpath(QFileInfo(path).absolutePath());
            QWidget* target=QApplication::activeModalWidget();if(!target)target=QApplication::activePopupWidget();if(!target)target=&window;
            auto image=target->grab().toImage();
            if(image.width()>1600)image=image.scaledToWidth(1600,Qt::SmoothTransformation); image.setDevicePixelRatio(1);
            QApplication::exit(image.save(path)?0:1);
        });
    }
    return app.exec();
}
