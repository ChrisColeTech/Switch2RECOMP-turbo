// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
#include "window.h"
#include "artwork.h"
#include "filterpopup.h"
#include "theme.h"
#include "processwatch.h"
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
QLabel* textLabel(const QString& text, const char* name = "") {
    auto* label = new QLabel(text); label->setTextFormat(Qt::PlainText); label->setObjectName(name); return label;
}
QIcon commandIcon(const QString& kind) {
    QPixmap image(128,128); image.fill(Qt::transparent);
    QPainter p(&image); p.setRenderHint(QPainter::Antialiasing); p.scale(4,4);
    const auto& theme=Theme::colors();
    p.setPen(QPen(theme.text,1.65,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    if(kind=="add"||kind=="folder") {
        QPainterPath path; path.moveTo(3,10);path.lineTo(3,7);path.quadTo(3,5,5,5);path.lineTo(12,5);path.lineTo(15,8);path.lineTo(26,8);path.quadTo(28,8,28,10);path.lineTo(28,25);path.lineTo(3,25);path.closeSubpath();
        p.fillPath(path,theme.raised);p.setPen(QPen(theme.text,1.4));p.drawPath(path);
        p.setPen(QPen(theme.accent,1.8));
        if(kind=="add"){p.drawLine(11,17,21,17);p.drawLine(16,12,16,22);}
        else {p.drawLine(11,17,21,17);p.drawLine(18,14,21,17);p.drawLine(18,20,21,17);}
    } else if(kind=="play") {
        p.setPen(QPen(theme.accent,1.7));p.setBrush(theme.selected);p.drawEllipse(QRectF(3,3,26,26));
        QPainterPath triangle;triangle.moveTo(13,10);triangle.lineTo(22,16);triangle.lineTo(13,22);triangle.closeSubpath();p.fillPath(triangle,theme.accent);
    } else if(kind=="build") {
        p.setPen(QPen(theme.text,1.6));
        QPainterPath tool;tool.moveTo(25,4);tool.lineTo(20,9);tool.lineTo(23,12);tool.lineTo(28,7);tool.cubicTo(31,14,24,19,19,15);tool.lineTo(8,27);tool.cubicTo(4,30,1,26,5,23);tool.lineTo(16,12);tool.cubicTo(12,7,18,1,25,4);p.drawPath(tool);
        p.drawEllipse(QPointF(7,25),1,1);
    } else if(kind=="stop") {
        p.setPen(QPen(theme.text,1.6));p.setBrush(theme.raised);p.drawRoundedRect(QRectF(6,6,20,20),3,3);
    } else if(kind=="settings") {
        QPainterPath gear;
        for(int i=0;i<32;++i){const double angle=(i*2.0*3.141592653589793/32.0)-3.141592653589793/2;const double radius=(i%4==1||i%4==2)?13:10;
            const QPointF pt(16+std::cos(angle)*radius,16+std::sin(angle)*radius);if(i==0)gear.moveTo(pt);else gear.lineTo(pt);}
        gear.closeSubpath();p.setPen(QPen(theme.text,1.5));p.setBrush(theme.raised);p.drawPath(gear);p.drawEllipse(QRectF(11,11,10,10));
    } else if(kind=="properties") {
        p.setPen(QPen(theme.text,1.8));
        p.drawLine(5,8,27,8);p.drawLine(5,16,27,16);p.drawLine(5,24,27,24);
        p.setBrush(theme.raised);p.drawRoundedRect(QRectF(10,5,5,6),1,1);p.drawRoundedRect(QRectF(21,13,5,6),1,1);p.drawRoundedRect(QRectF(7,21,5,6),1,1);
    } else if(kind=="grid") {
        p.setBrush(theme.raised);for(int y:{5,18})for(int x:{5,18})p.drawRoundedRect(QRectF(x,y,9,9),1,1);
    } else if(kind=="list") {
        for(int y:{7,16,25}){p.drawRect(QRectF(4,y-2,4,4));p.drawLine(13,y,28,y);}
    } else if(kind=="log") {
        p.drawRoundedRect(QRectF(6,3,20,26),2,2);for(int y:{10,16,22})p.drawLine(11,y,21,y);
    } else if(kind=="import") {
        p.drawRoundedRect(QRectF(4,5,24,22),3,3);p.drawLine(4,11,28,11);
        p.setPen(QPen(theme.accent,1.8));p.drawLine(16,14,16,24);p.drawLine(12,20,16,24);p.drawLine(20,20,16,24);
    } else if(kind=="filter") {
        p.drawEllipse(QRectF(4,3,16,16));p.drawLine(18,17,27,26);
        p.setPen(QPen(theme.accent,1.5));p.drawLine(3,24,13,24);p.drawLine(6,28,13,28);
    }
    return QIcon(image);
}
QString displayPath(const QString& path) { return path.isEmpty() ? QString("Not linked") : QDir::toNativeSeparators(path); }
QWidget* browseField(QLineEdit* edit, QWidget* parent, const QString& title, const QString& filter, bool folder=false) {
    auto* row=new QWidget;auto* layout=new QHBoxLayout(row);layout->setContentsMargins(0,0,0,0);layout->setSpacing(5);
    auto* button=new QPushButton("…");button->setFixedWidth(30);button->setToolTip(title);layout->addWidget(edit,1);layout->addWidget(button);
    QObject::connect(button,&QPushButton::clicked,parent,[=]{
        const QString path=folder?QFileDialog::getExistingDirectory(parent,title,edit->text()):QFileDialog::getOpenFileName(parent,title,edit->text(),filter);
        if(!path.isEmpty())edit->setText(QDir::toNativeSeparators(path));
    });return row;
}
QFormLayout* tabForm(QTabWidget* tabs,const QString& name) {
    auto* page=new QWidget;auto* form=new QFormLayout(page);form->setContentsMargins(18,22,18,20);form->setHorizontalSpacing(15);form->setVerticalSpacing(12);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);tabs->addTab(page,name);return form;
}
}

Window::Window(const QString& file,const QString& root,bool isDemo):library(file),builder(this),demo(isDemo) {
    setWindowTitle("SwitchRecomp");resize(1280,800);setMinimumSize(920,600);setAcceptDrops(true);
    setDockOptions(QMainWindow::AllowTabbedDocks|QMainWindow::AnimatedDocks);
    QString error;library.load(&error);
    const QString installedKeys=QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("keys/prod.keys");
    if(library.keysPath.isEmpty()&&QFileInfo(installedKeys).isFile())library.keysPath=installedKeys;
    if(!root.isEmpty())library.toolRoot=QFileInfo(root).absoluteFilePath();
    if(library.toolRoot.isEmpty())library.toolRoot=Toolchain::discoverRoot();
    Theme::apply(*qApp,library.colorMode,library.accentColor);
    if(demo)seedDemo();
    if(!demo)running=findRunningGames(library.games);
    createActions();createMenusAndToolbar();createLibrary();createBuildDock();createStatusBar();
    if(!demo&&!library.geometry.isEmpty())restoreGeometry(library.geometry);
    if(!demo&&!library.dockState.isEmpty())restoreState(library.dockState,1);
    setViewMode(library.viewMode);

    connect(&builder,&BuildManager::output,console,&QPlainTextEdit::appendPlainText);
    connect(&builder,&BuildManager::progress,this,[this](const QString& phase,int current,int total){
        buildPhase=phase;buildCurrent=current;buildTotal=total;
        if(auto* card=cards.value(builder.gameId()))card->setBuildProgress(phase,current,total);
        jobLabel->setText(total?QString("%1  %2/%3").arg(phase).arg(current).arg(total):phase);
        progress->setVisible(true);progress->setRange(0,total);progress->setValue(current);
    });
    connect(&builder,&BuildManager::finished,this,[this](const QString& id,bool okay,const QString& detail){
        if(auto* game=library.find(id)){
            if(okay){game->builtSource=game->source;game->buildIncomplete=false;game->lastBuilt=QDateTime::currentDateTime();}
            save();
        }
        console->appendPlainText("\n"+detail);buildDock->setWindowTitle(okay?"Build output — complete":"Build output — stopped");
        progress->hide();jobLabel->setText(okay?"Build complete — ready to play":"Build stopped — see output");
        refreshLibrary();emit smokeFinished(okay);
        if(closing)QTimer::singleShot(0,this,&QWidget::close);
    });
    new QShortcut(QKeySequence("Ctrl+F"),this,[this]{showFilters();});
    auto* galleryEnter=new QShortcut(QKeySequence(Qt::Key_Return),gallery,[this]{activateGame();});galleryEnter->setContext(Qt::WidgetShortcut);
    auto* tableEnter=new QShortcut(QKeySequence(Qt::Key_Return),table,[this]{activateGame();});tableEnter->setContext(Qt::WidgetShortcut);
    auto* poll=new QTimer(this);poll->setInterval(2500);
    connect(poll,&QTimer::timeout,this,[this]{
        if(!demo){const auto detected=findRunningGames(library.games);if(detected!=running){running=detected;refreshLibrary();}}
    });poll->start();
    refreshTools();refreshLibrary();
#ifdef Q_OS_WIN
    // Ask Windows for a dark native caption while retaining its standard window controls.
    if(HMODULE dwm=LoadLibraryW(L"dwmapi.dll")){
        using SetAttribute=HRESULT(WINAPI*)(HWND,DWORD,LPCVOID,DWORD);
        auto set=reinterpret_cast<SetAttribute>(GetProcAddress(dwm,"DwmSetWindowAttribute"));
        BOOL dark=library.colorMode!="light";if(set)set(reinterpret_cast<HWND>(winId()),20,&dark,sizeof dark);FreeLibrary(dwm);
    }
#endif
    if(!error.isEmpty())QTimer::singleShot(0,this,[this,error]{message("Library could not be loaded",error,true);});
}

void Window::createActions(){
    auto make=[this](const QString& name,const QString& icon,const QString& tip){auto* a=new QAction(commandIcon(icon),name,this);a->setProperty("iconKind",icon);a->setToolTip(tip);return a;};
    addAction=make("Add game","add","Add a NUP/NSP package or NRO (Ctrl+O)");addAction->setShortcut(QKeySequence("Ctrl+O"));
    connect(addAction,&QAction::triggered,this,[this]{for(const auto& p:QFileDialog::getOpenFileNames(this,"Add game",{},"Game packages (*.nup *.nsp *.nro)"))importSource(p);});
    addFolderAction=make("Add extracted folder…","folder","Add a folder containing ExeFS and RomFS");
    connect(addFolderAction,&QAction::triggered,this,[this]{auto p=QFileDialog::getExistingDirectory(this,"Add extracted game folder");if(!p.isEmpty())importSource(p);});
    importAction=make("Import build…","import","Add an already compiled game to your library");
    connect(importAction,&QAction::triggered,this,[this]{auto p=QFileDialog::getOpenFileName(this,"Import native build",{},QString("Native games (*%1)").arg(executableSuffix));if(!p.isEmpty())importBuild(p);});
    playAction=make("Play","play","Launch the selected game");connect(playAction,&QAction::triggered,this,&Window::play);
    buildAction=make("Build","build","Build or rebuild the selected game using saved options (Ctrl+B)");buildAction->setShortcut(QKeySequence("Ctrl+B"));connect(buildAction,&QAction::triggered,this,&Window::startBuild);
    stopAction=make("Stop","stop","Cancel the active build or close the selected running game");connect(stopAction,&QAction::triggered,this,&Window::stop);
    propertiesAction=make("Properties","properties","Game files, build options, renderer, and artwork (Alt+Enter)");propertiesAction->setShortcut(QKeySequence("Alt+Return"));connect(propertiesAction,&QAction::triggered,this,[this]{showProperties();});
    buildOptionsAction=new QAction("Build options…",this);connect(buildOptionsAction,&QAction::triggered,this,[this]{showProperties(1);});
    settingsAction=make("Settings","settings","Toolkit paths, defaults, and appearance");connect(settingsAction,&QAction::triggered,this,[this]{showSettings();});
    installKeysAction=new QAction("Install keys…",this);connect(installKeysAction,&QAction::triggered,this,&Window::installKeys);
    folderAction=make("Open build folder","folder","Open generated game files");connect(folderAction,&QAction::triggered,this,[this]{openFolder();});
    saveFolderAction=new QAction("Open save folder",this);connect(saveFolderAction,&QAction::triggered,this,[this]{openFolder(true);});
    logAction=make("View game log","log","Open the runtime log");connect(logAction,&QAction::triggered,this,[this]{openGameFile(".log");});
    removeAction=new QAction("Remove from library…",this);connect(removeAction,&QAction::triggered,this,&Window::removeGame);
    auto* group=new QActionGroup(this);group->setExclusive(true);
    galleryAction=make("Gallery","grid","Cover gallery");tableAction=make("List","list","Sortable details list");
    for(auto* a:{galleryAction,tableAction}){a->setCheckable(true);group->addAction(a);}
    connect(galleryAction,&QAction::triggered,this,[this]{setViewMode(0);save();});
    connect(tableAction,&QAction::triggered,this,[this]{setViewMode(1);save();});
    backgroundAction=new QAction("Show selected game background",this);backgroundAction->setCheckable(true);backgroundAction->setChecked(library.showBackground);
    connect(backgroundAction,&QAction::toggled,this,[this](bool on){library.showBackground=on;refreshSelection();save();});
    filterAction=make("Search & Filter","filter","Search, filter, and choose the library view (Ctrl+F)");
    connect(filterAction,&QAction::triggered,this,[this]{if(filters&&filters->isVisible())filters->hide();else showFilters();});
}

void Window::createMenusAndToolbar(){
    auto* file=menuBar()->addMenu("&File");file->addAction(addAction);file->addAction(addFolderAction);file->addAction(importAction);file->addSeparator();file->addAction("Exit",QKeySequence("Alt+F4"),this,&QWidget::close);
    auto* game=menuBar()->addMenu("&Game");game->addAction(playAction);game->addAction(stopAction);game->addSeparator();game->addAction(propertiesAction);game->addAction(folderAction);game->addAction(saveFolderAction);game->addAction(logAction);game->addSeparator();game->addAction(removeAction);
    auto* build=menuBar()->addMenu("&Build");build->addAction(buildAction);build->addAction(buildOptionsAction);build->addAction("Show build output",QKeySequence("Ctrl+L"),this,&Window::showBuildOutput);
    auto* keys=menuBar()->addMenu("&Keys");keys->addAction(installKeysAction);keys->addAction("Open keys folder",this,[this]{const auto path=QFileInfo(library.keysPath).absolutePath();if(QDir(path).exists())QDesktopServices::openUrl(QUrl::fromLocalFile(path));});
    auto* view=menuBar()->addMenu("&View");view->setObjectName("ViewMenu");view->addAction(galleryAction);view->addAction(tableAction);view->addSeparator();view->addAction(filterAction);view->addAction(backgroundAction);
    view->addAction("Refresh library",QKeySequence("F5"),this,[this]{invalidateArtwork();refreshTools();refreshLibrary();});
    auto* toolsMenu=menuBar()->addMenu("&Tools");toolsMenu->addAction(settingsAction);toolsMenu->addAction("Open toolkit folder",this,[this]{QDesktopServices::openUrl(QUrl::fromLocalFile(library.toolRoot));});
    auto* help=menuBar()->addMenu("&Help");help->addAction("Launcher guide",this,[this]{QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(library.toolRoot).filePath("docs/LAUNCHER.md")));});
    help->addAction("About SwitchRecomp",this,[this]{message("SwitchRecomp by MZ Labs","SwitchRecomp Qt launcher\nCopyright 2026 MZ Labs\nGNU AGPL v3 — see LICENSE.txt and NOTICE.txt.\n\nBuild and launch your own game dumps from one library.\nNo games, keys, or firmware are included.\n\nNative controller input is supported. Vibration and some Switch 2 audio/video features remain incomplete.");});

    auto* bar=addToolBar("Commands");bar->setObjectName("Commands");bar->setMovable(false);bar->setIconSize({32,32});bar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    auto* addButton=new QToolButton;addButton->setObjectName("AddGameButton");addButton->setDefaultAction(addAction);addButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);addButton->setIconSize({32,32});
    auto* menu=new QMenu(addButton);menu->addAction(addAction);menu->addAction(addFolderAction);menu->addAction(importAction);addButton->setMenu(menu);addButton->setPopupMode(QToolButton::MenuButtonPopup);bar->addWidget(addButton);
    auto* spacer=new QWidget;spacer->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);bar->addWidget(spacer);
    bar->addAction(filterAction);filterAnchor=bar->widgetForAction(filterAction);
    for(auto* button:bar->findChildren<QToolButton*>())button->setFocusPolicy(Qt::StrongFocus);
    filters=new FilterPopup(this);search=filters->search;sort=filters->sort;
    filters->galleryButton->setDefaultAction(galleryAction);filters->listButton->setDefaultAction(tableAction);
    coverSize=filters->coverSize;coverSize->setValue(library.coverWidth);
    connect(coverSize,&QSlider::valueChanged,this,[this](int size){
        library.coverWidth=size;if(!gallery)return;gallery->setGridSize({size+12,GameCard::heightFor(size)+12});
        for(int i=0;i<gallery->count();++i)gallery->item(i)->setSizeHint({size,GameCard::heightFor(size)});
        for(auto* card:cards)card->setCardWidth(size);gallery->doItemsLayout();
    });
    connect(coverSize,&QSlider::sliderReleased,this,&Window::save);
    connect(filters,&FilterPopup::filtersChanged,this,[this]{
        if(table){if(sort->currentIndex()==0)table->sortItems(0,Qt::AscendingOrder);else if(sort->currentIndex()==1)table->sortItems(4,Qt::DescendingOrder);else if(sort->currentIndex()==2)table->sortItems(1,Qt::AscendingOrder);}
        refreshLibrary();
    });
}

void Window::createLibrary(){
    canvas=new LibraryCanvas;setCentralWidget(canvas);auto* layout=new QVBoxLayout(canvas);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
    views=new QStackedWidget;views->setStyleSheet("QStackedWidget { background: transparent; }");layout->addWidget(views,1);
    gallery=new QListWidget;gallery->setViewMode(QListView::IconMode);gallery->setResizeMode(QListView::Adjust);gallery->setMovement(QListView::Static);
    gallery->setAccessibleName("Game library gallery");
    gallery->setWrapping(true);gallery->setSpacing(8);gallery->setSelectionMode(QAbstractItemView::SingleSelection);gallery->setMouseTracking(true);gallery->setStyleSheet("QListWidget {background:transparent;padding:8px;}");
    gallery->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);gallery->viewport()->setAutoFillBackground(false);
    gallery->setItemDelegate(new GalleryDelegate(gallery));gallery->setGridSize({library.coverWidth+12,GameCard::heightFor(library.coverWidth)+12});
    gallery->setContextMenuPolicy(Qt::CustomContextMenu);views->addWidget(gallery);
    table=new QTreeWidget;table->setColumnCount(6);table->setHeaderLabels({"Name","Status","Format","Renderer","Last played","Build folder"});
    table->setAccessibleName("Game library details");
    table->setItemDelegate(new TableDelegate(table));
    table->setRootIsDecorated(false);table->setUniformRowHeights(true);table->setAlternatingRowColors(true);table->setSelectionMode(QAbstractItemView::SingleSelection);table->setSortingEnabled(true);table->sortItems(0,Qt::AscendingOrder);
    table->setColumnWidth(0,260);table->setColumnWidth(1,130);table->setColumnWidth(2,110);table->setColumnWidth(3,80);table->setColumnWidth(4,150);table->header()->setStretchLastSection(true);
    connect(table->header(),&QHeaderView::sortIndicatorChanged,this,[this](int column,Qt::SortOrder order){
        const int preset=column==0&&order==Qt::AscendingOrder?0:column==4&&order==Qt::DescendingOrder?1:column==1&&order==Qt::AscendingOrder?2:-1;
        QSignalBlocker block(sort);sort->setCurrentIndex(preset);
    });
    table->setContextMenuPolicy(Qt::CustomContextMenu);views->addWidget(table);
    empty=textLabel("Your library is empty.\n\nAdd a NUP or NRO, import an existing build,\nor drop a game package into this window.","Empty");empty->setAlignment(Qt::AlignCenter);layout->addWidget(empty,1);
    connect(gallery,&QListWidget::currentItemChanged,this,[this](QListWidgetItem* item){if(item)selectGame(item->data(Qt::UserRole).toString());});
    connect(table,&QTreeWidget::currentItemChanged,this,[this](QTreeWidgetItem* item){if(item)selectGame(item->data(0,Qt::UserRole).toString());});
    connect(gallery,&QListWidget::itemDoubleClicked,this,[this]{activateGame();});connect(table,&QTreeWidget::itemDoubleClicked,this,[this]{activateGame();});
    connect(gallery,&QListWidget::customContextMenuRequested,this,[this](QPoint p){if(auto* item=gallery->itemAt(p)){gallery->setCurrentItem(item);showContextMenu(gallery->viewport()->mapToGlobal(p));}});
    connect(table,&QTreeWidget::customContextMenuRequested,this,[this](QPoint p){if(auto* item=table->itemAt(p)){table->setCurrentItem(item);showContextMenu(table->viewport()->mapToGlobal(p));}});
}

void Window::createBuildDock(){
    buildDock=new QDockWidget("Build output",this);buildDock->setObjectName("BuildOutput");buildDock->setAllowedAreas(Qt::BottomDockWidgetArea|Qt::RightDockWidgetArea);
    auto* page=new QWidget;auto* layout=new QVBoxLayout(page);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
    auto* toolsBar=new QToolBar;toolsBar->setIconSize({16,16});
    toolsBar->addAction("Open log",this,[this]{const auto* g=library.find(builder.gameId());if(!g)g=selected();if(g)QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(g->buildDir).filePath(g->outputName+".build.log")));});
    toolsBar->addAction("Clear view",this,[this]{console->clear();});layout->addWidget(toolsBar);
    console=new QPlainTextEdit;console->setReadOnly(true);console->setMaximumBlockCount(12000);console->setPlaceholderText("Build output appears here. Select a game and choose Build to begin.");layout->addWidget(console,1);
    buildDock->setWidget(page);addDockWidget(Qt::BottomDockWidgetArea,buildDock);buildDock->hide();
    if(auto* view=menuBar()->findChild<QMenu*>("ViewMenu"))view->addAction(buildDock->toggleViewAction());
}
void Window::createStatusBar(){
    statusBar()->setSizeGripEnabled(true);
    countLabel=textLabel("0 games");countLabel->setMinimumWidth(65);statusBar()->addWidget(countLabel);
    selectionLabel=textLabel("No selection","Muted");selectionLabel->setMinimumWidth(100);selectionLabel->setMaximumWidth(410);statusBar()->addWidget(selectionLabel,1);
    jobLabel=textLabel("Ready","Muted");jobLabel->setMaximumWidth(320);statusBar()->addPermanentWidget(jobLabel);
    progress=new QProgressBar;progress->setAccessibleName("Native build progress");progress->setFixedWidth(125);progress->setTextVisible(false);progress->hide();statusBar()->addPermanentWidget(progress);
    toolStatus=textLabel("Checking toolkit…","Muted");statusBar()->addPermanentWidget(toolStatus);
    auto* settings=new QToolButton;settings->setObjectName("StatusSettings");settings->setDefaultAction(settingsAction);settings->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);settings->setIconSize({17,17});statusBar()->addPermanentWidget(settings);
}

Game* Window::selected(){return library.find(selectedId);}
QString Window::statusFor(const Game& g)const{
    if(demo)return "Preview";
    if(builder.busy()&&builder.gameId()==g.id)return "Building";
    if(running.contains(g.id))return "Running";
    if(g.hasBuild()&&g.source.endsWith(".nup",Qt::CaseInsensitive)&&!g.sourceAvailable())return "Source offline";
    return g.hasBuild()?"Ready to play":"Build needed";
}
void Window::save(){if(demo)return;QString error;if(!library.save(&error))message("Could not save library",error,true);}
void Window::message(const QString& title,const QString& text,bool error){if(error)QMessageBox::warning(this,title,text);else QMessageBox::information(this,title,text);}
void Window::refreshLibrary(){
    if(!gallery)return;
    QSignalBlocker a(gallery),b(table);cards.clear();gallery->clear();table->clear();table->setSortingEnabled(false);
    LibraryFilter query{search->text(),filters->state->currentIndex(),filters->source->currentIndex()};
    QVector<const Game*> games;for(const auto& g:library.games)if(query.matches(g,running.contains(g.id),builder.busy()&&builder.gameId()==g.id))games.append(&g);
    std::sort(games.begin(),games.end(),[this](const Game* x,const Game* y){
        if(sort->currentIndex()==1&&x->lastPlayed!=y->lastPlayed)return x->lastPlayed>y->lastPlayed;
        if(sort->currentIndex()==2&&statusFor(*x)!=statusFor(*y))return statusFor(*x)<statusFor(*y);
        return QString::localeAwareCompare(x->title,y->title)<0;
    });
    bool found=false;
    for(const Game* g:games){
        const auto status=statusFor(*g);auto* item=new QListWidgetItem;item->setData(Qt::UserRole,g->id);
        item->setText(g->title);item->setData(Qt::AccessibleTextRole,g->title+", "+status);
        item->setData(Qt::AccessibleDescriptionRole,g->format()+", "+g->renderer);
        item->setSizeHint({library.coverWidth,GameCard::heightFor(library.coverWidth)});
        item->setToolTip(g->title+"\n"+status+"\n"+displayPath(g->buildDir));gallery->addItem(item);
        auto state=cardState(*g,running.contains(g->id),builder.busy()&&builder.gameId()==g->id,builder.busy(),tools.canBuild());
        if(demo)state.actionEnabled=false;
        auto* card=new GameCard(*g,state,library.coverWidth);cards.insert(g->id,card);gallery->setItemWidget(item,card);
        if(state.building)card->setBuildProgress(buildPhase,buildCurrent,buildTotal);
        connect(card,&GameCard::selected,this,&Window::selectGame);
        connect(card,&GameCard::primaryRequested,this,[this](const QString& id){QTimer::singleShot(0,this,[this,id]{cardAction(id);});});
        connect(card,&GameCard::propertiesRequested,this,[this](const QString& id){QTimer::singleShot(0,this,[this,id]{selectGame(id);showProperties();});});
        auto* row=new QTreeWidgetItem({g->title,status,g->format(),g->renderer,g->lastPlayed.isValid()?g->lastPlayed.toLocalTime().toString("yyyy-MM-dd HH:mm"):QString("—"),displayPath(g->buildDir)});
        row->setData(0,Qt::UserRole,g->id);row->setToolTip(5,displayPath(g->buildDir));table->addTopLevelItem(row);
        if(g->id==selectedId){gallery->setCurrentItem(item);table->setCurrentItem(row);found=true;}
    }
    if(!found){selectedId=games.isEmpty()?QString{}:games.front()->id;if(!games.isEmpty()){gallery->setCurrentRow(0);table->setCurrentItem(table->topLevelItem(0));}}
    table->setSortingEnabled(true);
    empty->setVisible(games.isEmpty());views->setVisible(!games.isEmpty());
    empty->setText(library.games.isEmpty()?"Your library is empty.\n\nAdd a NUP or NRO, import an existing build,\nor drop a game package into this window.":"No games match these filters.\nPress Ctrl+F to adjust or clear them.");
    filters->setResultCount(int(games.size()),int(library.games.size()));
    filterAction->setText(query.activeCount()?QString("Search && Filter (%1)").arg(query.activeCount()):"Search && Filter");
    filterAction->setToolTip(query.activeCount()?QString("%1 active filters — Ctrl+F to edit or clear").arg(query.activeCount()):"Search, filter, and sort your library (Ctrl+F)");
    countLabel->setText(query.activeCount()?QString("%1 / %2 games").arg(games.size()).arg(library.games.size()):QString("%1 game%2").arg(library.games.size()).arg(library.games.size()==1?"":"s"));refreshSelection();
}
void Window::selectGame(const QString& id){
    selectedId=id;QSignalBlocker a(gallery),b(table);
    for(int i=0;i<gallery->count();++i)if(gallery->item(i)->data(Qt::UserRole).toString()==id)gallery->setCurrentRow(i);
    for(int i=0;i<table->topLevelItemCount();++i)if(table->topLevelItem(i)->data(0,Qt::UserRole).toString()==id)table->setCurrentItem(table->topLevelItem(i));
    refreshSelection();
}
void Window::selectTitle(const QString& title){for(const auto& game:library.games)if(game.title.compare(title,Qt::CaseInsensitive)==0){selectGame(game.id);return;}}
void Window::refreshSelection(){
    if(!selectionLabel)return;
    const auto* g=selected();const bool exists=g;const bool playing=exists&&running.contains(g->id);const bool building=exists&&builder.busy()&&builder.gameId()==g->id;
    playAction->setEnabled(exists&&!demo&&g->hasBuild()&&!playing&&!building);
    buildAction->setEnabled(exists&&!demo&&!builder.busy()&&!playing&&g->sourceAvailable()&&tools.canBuild());
    stopAction->setEnabled(building||playing);stopAction->setText(building?"Cancel build":"Stop");
    propertiesAction->setEnabled(exists);buildOptionsAction->setEnabled(exists&&!playing&&!building);removeAction->setEnabled(exists&&!playing&&!building);
    folderAction->setEnabled(exists);saveFolderAction->setEnabled(exists&&g->hasBuild());logAction->setEnabled(exists&&QFileInfo(QDir(g->buildDir).filePath(g->outputName+".log")).exists());
    const QString info=g?g->title+" · "+g->format()+" · "+g->renderer:"No selection";
    selectionLabel->setText(fontMetrics().elidedText(info,Qt::ElideRight,390));selectionLabel->setToolTip(g?info+"\n"+displayPath(g->source):info);
    canvas->setBackground(g?(g->background.isEmpty()?g->cover:g->background):QString{},library.showBackground);
    for(auto it=cards.begin();it!=cards.end();++it)it.value()->setSelected(it.key()==selectedId);
}
void Window::setViewMode(int mode){
    library.viewMode=mode==1?1:0;if(views)views->setCurrentIndex(library.viewMode);
    galleryAction->setChecked(library.viewMode==0);tableAction->setChecked(library.viewMode==1);
    if(coverSize)coverSize->setEnabled(library.viewMode==0);
    if(gallery&&table&&(!filters||!filters->isVisible())){if(library.viewMode==0)gallery->setFocus();else table->setFocus();}
}
void Window::showBuildOutput(){buildDock->show();resizeDocks({buildDock},{190},Qt::Vertical);buildDock->raise();}
void Window::showFilters(){if(filters&&filterAnchor)filters->openBelow(filterAnchor);}
void Window::installKeys(){
    const QString source=QFileDialog::getOpenFileName(this,"Install Nintendo keys",{},"Nintendo key file (prod.keys);;All files (*)");if(source.isEmpty())return;
    QFile input(source);if(!input.open(QIODevice::ReadOnly)){message("Cannot install keys",input.errorString(),true);return;}
    const QByteArray data=input.readAll();
    if(!data.contains("header_key")&&!data.contains("header_key_source")){message("Invalid key file","This file does not contain the NCA header keys required for NSP packages.",true);return;}
    const QString destination=QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("keys/prod.keys");
    if(!QDir().mkpath(QFileInfo(destination).absolutePath())){message("Cannot install keys","The launcher could not create its keys folder.",true);return;}
    QSaveFile output(destination);if(!output.open(QIODevice::WriteOnly)||output.write(data)!=data.size()||!output.commit()){message("Cannot install keys",output.errorString(),true);return;}
    library.keysPath=destination;save();refreshTools();message("Keys installed","prod.keys was installed in the launcher data folder. Key contents are never copied into builds or release packages.");
}
void Window::setAppearance(const QString& mode,const QString& accent){
    Theme::apply(*qApp,mode,accent);
    for(auto* action:findChildren<QAction*>()){const auto kind=action->property("iconKind").toString();if(!kind.isEmpty())action->setIcon(commandIcon(kind));}
    for(auto* card:cards)card->refreshAppearance();if(canvas)canvas->update();if(gallery)gallery->viewport()->update();
#ifdef Q_OS_WIN
    if(HMODULE dwm=LoadLibraryW(L"dwmapi.dll")){using SetAttribute=HRESULT(WINAPI*)(HWND,DWORD,LPCVOID,DWORD);auto set=reinterpret_cast<SetAttribute>(GetProcAddress(dwm,"DwmSetWindowAttribute"));BOOL dark=mode!="light";if(set)set(reinterpret_cast<HWND>(winId()),20,&dark,sizeof dark);FreeLibrary(dwm);}
#endif
}

void Window::refreshTools(){
    tools=Toolchain::find(library.toolRoot);tools.keys=library.keysPath;dotnetReady=false;const int generation=++toolsGeneration;
    const auto state=[](const QString& p){return p.isEmpty()?QString("Missing"):QDir::toNativeSeparators(p);};
    healthText=QString("Python: %1\nGCC: %2\nRuntime: %3\nKeys: %4\n.NET 10: checking…").arg(state(tools.python),state(tools.compiler),state(tools.runtime),state(tools.keys));
    toolStatus->setText(tools.canBuild()?"Toolkit ready":"Setup needed");toolStatus->setToolTip(tools.canBuild()?displayPath(tools.root):tools.problem());
    emit toolsChanged();
    if(tools.dotnet.isEmpty()){healthText.replace("checking…","Not found");emit toolsChanged();return;}
    auto* check=new QProcess(this);
    connect(check,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,check,generation](int code,QProcess::ExitStatus){
        if(generation==toolsGeneration){dotnetReady=code==0&&QString::fromUtf8(check->readAllStandardOutput()).contains(QRegularExpression("Microsoft\\.NETCore\\.App 10\\."));healthText.replace("checking…",dotnetReady?"Ready":"Not found");emit toolsChanged();}
        check->deleteLater();
    });
    connect(check,&QProcess::errorOccurred,this,[this,check,generation](QProcess::ProcessError error){if(error==QProcess::FailedToStart){if(generation==toolsGeneration){healthText.replace("checking…","Could not start dotnet");emit toolsChanged();}check->deleteLater();}});
    check->start(tools.dotnet,{"--list-runtimes"});QTimer::singleShot(10000,check,[check]{if(check->state()!=QProcess::NotRunning)check->kill();});
}

bool Window::editGame(Game& game,int tab,bool adding){
    const bool locked=running.contains(game.id)||(builder.busy()&&builder.gameId()==game.id);
    QDialog dialog(this);dialog.setWindowTitle(adding?"Add game":game.title+" — Properties");dialog.resize(670,400);
    auto* layout=new QVBoxLayout(&dialog);layout->setContentsMargins(10,10,10,10);auto* tabs=new QTabWidget;layout->addWidget(tabs,1);
    auto* general=tabForm(tabs,"General");auto* title=new QLineEdit(game.title);auto* source=new QLineEdit(game.source.isEmpty()?QString{}:displayPath(game.source));
    general->addRow("Display name",title);auto* sourceField=browseField(source,&dialog,"Choose source","Game packages (*.nup *.nsp *.nro);;All files (*)");sourceField->setEnabled(!locked);general->addRow("Source",sourceField);
    auto* folder=new QPushButton("Choose extracted folder…");folder->setEnabled(!locked);general->addRow("",folder);connect(folder,&QPushButton::clicked,&dialog,[&dialog,source]{auto p=QFileDialog::getExistingDirectory(&dialog,"Choose extracted game folder");if(!p.isEmpty())source->setText(QDir::toNativeSeparators(p));});
    general->addRow("Format",textLabel(game.format()));general->addRow("Status",textLabel(adding?"Not built":statusFor(game)));
    auto* pathsNote=textLabel("NUP/NSP game data stays in the original package. Changing the source requires a rebuild.","Muted");pathsNote->setWordWrap(true);general->addRow(pathsNote);
    auto* build=tabForm(tabs,"Build");auto* destination=new QLineEdit(displayPath(game.buildDir));build->addRow("Build folder",browseField(destination,&dialog,"Choose build output folder",{},true));
    auto* name=new QLineEdit(game.outputName);name->setReadOnly(!adding);build->addRow("Executable name",name);
    auto* workers=new QSpinBox;workers->setRange(0,64);workers->setSpecialValueText("Use default");workers->setValue(game.workers);build->addRow("Compiler workers",workers);
    auto* optimization=new QComboBox;optimization->addItems({"O0 — no optimization","O1 — quick build","O2 — balanced (recommended)","O3 — maximum optimization"});optimization->setCurrentIndex(game.optimization);build->addRow("Optimization",optimization);
    auto* runtime=tabForm(tabs,"Runtime");auto* renderer=new QComboBox;renderer->addItem("Vulkan — native NVN","vulkan");renderer->addItem("OpenGL — compatibility","opengl");renderer->setCurrentIndex(qMax(0,renderer->findData(game.renderer)));runtime->addRow("Graphics backend",renderer);
    auto* runtimeNote=textLabel("Vulkan needs the bundled shader tools, Python, and .NET 10.\n\nOther runtime options remain in the game's INI. Renderer changes are applied when you next play.","Muted");runtimeNote->setWordWrap(true);runtime->addRow(runtimeNote);
    auto* openIni=new QPushButton("Open game settings file");openIni->setEnabled(QFileInfo(game.iniPath()).exists());runtime->addRow("",openIni);connect(openIni,&QPushButton::clicked,&dialog,[&game]{QDesktopServices::openUrl(QUrl::fromLocalFile(game.iniPath()));});
    auto* artwork=tabForm(tabs,"Artwork");auto* cover=new QLineEdit(game.cover);auto* background=new QLineEdit(game.background);
    artwork->addRow("Cover image",browseField(cover,&dialog,"Choose cover","Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
    artwork->addRow("Background image",browseField(background,&dialog,"Choose background artwork","Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
    auto* note=textLabel("The background follows your selected game and is dimmed behind the gallery. Leave it empty to use the cover. Clear both paths to restore generated artwork.","Muted");note->setWordWrap(true);artwork->addRow(note);
    tabs->widget(1)->setEnabled(!locked);tabs->widget(2)->setEnabled(!locked);
    tabs->setCurrentIndex(qBound(0,tab,3));
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);if(adding)buttons->button(QDialogButtonBox::Ok)->setText("Add game");layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{
        if(title->text().trimmed().isEmpty()||destination->text().trimmed().isEmpty()){QMessageBox::warning(&dialog,"Missing details","A title and build folder are required.");return;}
        for(const auto& pair:{qMakePair(cover->text().trimmed(),game.cover),qMakePair(background->text().trimmed(),game.background)}){
            if(!pair.first.isEmpty()&&pair.first!=pair.second){QImageReader reader(pair.first);if(!reader.canRead()){QMessageBox::warning(&dialog,"Cannot load artwork",reader.errorString()+"\n"+pair.first);return;}}
        }
        game.title=title->text().trimmed();game.source=source->text().trimmed().isEmpty()?QString{}:QFileInfo(QDir::fromNativeSeparators(source->text().trimmed())).absoluteFilePath();
        game.buildDir=QFileInfo(QDir::fromNativeSeparators(destination->text().trimmed())).absoluteFilePath();
        if(adding)game.outputName=outputNameFor(name->text());game.workers=workers->value();game.optimization=optimization->currentIndex();game.renderer=renderer->currentData().toString();
        game.cover=QDir::fromNativeSeparators(cover->text().trimmed());game.background=QDir::fromNativeSeparators(background->text().trimmed());dialog.accept();
    });
    return dialog.exec()==QDialog::Accepted;
}
void Window::showProperties(int tab){
    auto* game=selected();if(!game)return;const auto id=game->id;Game edited=*game;
    if(editGame(edited,tab)){
        if(auto* current=library.find(id)){
            current->title=edited.title;current->source=edited.source;current->buildDir=edited.buildDir;
            current->renderer=edited.renderer;current->workers=edited.workers;current->optimization=edited.optimization;
            current->cover=edited.cover;current->background=edited.background;
        }
        invalidateArtwork();save();refreshLibrary();
    }
}
void Window::showAppearance(){showSettings(2);}
void Window::showSettings(int initialTab){
    QDialog dialog(this);dialog.setWindowTitle("SwitchRecomp — Settings");dialog.resize(690,440);auto* layout=new QVBoxLayout(&dialog);auto* tabs=new QTabWidget;layout->addWidget(tabs,1);
    auto* toolkit=tabForm(tabs,"Toolkit");auto* root=new QLineEdit(library.toolRoot);toolkit->addRow("Toolkit folder",browseField(root,&dialog,"Choose SwitchRecomp toolkit",{},true));
    auto* health=textLabel(healthText,"Muted");health->setWordWrap(true);health->setTextInteractionFlags(Qt::TextSelectableByMouse);toolkit->addRow(health);
    connect(this,&Window::toolsChanged,&dialog,[this,health]{health->setText(healthText);});
    auto* guide=new QPushButton("Open setup guide");toolkit->addRow("",guide);connect(guide,&QPushButton::clicked,&dialog,[this]{QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(library.toolRoot).filePath("docs/BUILD_ENVIRONMENT_GUIDE.md")));});
    auto* builds=tabForm(tabs,"Build defaults");auto* output=new QLineEdit(library.outputRoot);output->setPlaceholderText("Empty: create builds beside the source package");builds->addRow("Build location",browseField(output,&dialog,"Choose default output folder",{},true));
    auto* workers=new QSpinBox;workers->setRange(1,64);workers->setValue(library.jobs);builds->addRow("Compiler workers",workers);
    auto* appearance=tabForm(tabs,"Appearance");
    auto* mode=new QComboBox;mode->addItem("Dark","dark");mode->addItem("Light","light");mode->setCurrentIndex(library.colorMode=="light"?1:0);appearance->addRow("Color mode",mode);
    auto* accent=new QComboBox;for(const auto& color:Theme::accents())accent->addItem(color.label,color.id);accent->setCurrentIndex(qMax(0,accent->findData(library.accentColor)));appearance->addRow("Accent color",accent);
    const auto updateColors=[this,mode,accent]{
        for(int i=0;i<accent->count();++i){QPixmap swatch(18,18);swatch.fill(Theme::colorsFor(mode->currentData().toString(),accent->itemData(i).toString()).accent);accent->setItemIcon(i,QIcon(swatch));}
        setAppearance(mode->currentData().toString(),accent->currentData().toString());
    };
    connect(mode,qOverload<int>(&QComboBox::currentIndexChanged),&dialog,updateColors);connect(accent,qOverload<int>(&QComboBox::currentIndexChanged),&dialog,updateColors);updateColors();
    auto* backdrop=new QCheckBox("Show selected game artwork behind the gallery");backdrop->setChecked(library.showBackground);appearance->addRow(backdrop);
    auto* location=textLabel("Library file:\n"+displayPath(library.path),"Muted");location->setWordWrap(true);location->setTextInteractionFlags(Qt::TextSelectableByMouse);appearance->addRow(location);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);layout->addWidget(buttons);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{
        if(builder.busy()&&(root->text()!=library.toolRoot||output->text()!=library.outputRoot||workers->value()!=library.jobs)){QMessageBox::information(&dialog,"Build in progress","Toolkit/build defaults cannot change during a build. Appearance can still be changed.");return;}
        const auto selectedRoot=QDir::fromNativeSeparators(root->text().trimmed());
        if(selectedRoot!=library.toolRoot&&!QFileInfo(QDir(selectedRoot).filePath("switchrecomp.py")).isFile()){QMessageBox::warning(&dialog,"Toolkit not found","Select the folder containing switchrecomp.py.");return;}
        library.toolRoot=selectedRoot.isEmpty()?QString{}:QFileInfo(selectedRoot).absoluteFilePath();library.outputRoot=QDir::fromNativeSeparators(output->text().trimmed());library.jobs=workers->value();library.showBackground=backdrop->isChecked();
        library.colorMode=mode->currentData().toString();library.accentColor=accent->currentData().toString();
        backgroundAction->setChecked(library.showBackground);save();refreshTools();refreshSelection();dialog.accept();
    });tabs->setCurrentIndex(qBound(0,initialTab,2));dialog.exec();setAppearance(library.colorMode,library.accentColor);
}

bool Window::importSource(const QString& source,bool interactive,const QString& output){
    QString error;if(!validSource(source,&error)){if(interactive)message("Cannot add game",error,true);return false;}
    Game game=gameFromSource(source,library.outputRoot);if(!output.isEmpty())game.buildDir=QFileInfo(output).absoluteFilePath();
    for(const auto& g:library.games)if(samePath(g.source,game.source)){selectedId=g.id;refreshLibrary();return true;}
    if(interactive&&!editGame(game,0,true))return false;
    if(!library.add(game)){if(interactive)message("Already in library","That source or native build is already in the library.");return false;}
    selectedId=game.id;save();refreshLibrary();return true;
}
bool Window::importBuild(const QString& executable){
    QString error;Game game;if(!gameFromExecutable(executable,&game,&error)){message("Cannot import build",error,true);return false;}
    for(const auto& g:library.games)if(samePath(g.executable(),game.executable())){selectedId=g.id;refreshLibrary();return true;}
    if(!library.add(game)){message("Already in library","This source package already has a library entry.");return false;}
    selectedId=game.id;save();refreshLibrary();return true;
}
void Window::startBuild(){
    auto* game=selected();if(!game||demo||running.contains(game->id))return;
    QString error;if(!builder.start(*game,tools,library.jobs,&error)){message("Cannot start build",error,true);emit smokeFinished(false);return;}
    game->buildIncomplete=true;save();console->clear();console->appendPlainText("Source: "+displayPath(game->source)+"\nOutput: "+displayPath(game->buildDir)+"\n");
    buildDock->setWindowTitle("Build output — "+game->title);showBuildOutput();refreshLibrary();
}
void Window::activateGame(){if(auto* game=selected()){if(game->hasBuild())play();else showProperties(1);}}
void Window::cardAction(const QString& id){selectGame(id);if(demo)return;auto* game=selected();if(!game)return;if((builder.busy()&&builder.gameId()==id)||running.contains(id))stop();else if(game->hasBuild())play();else startBuild();}
void Window::play(){
    auto* game=selected();if(!game||demo||!game->hasBuild()||running.contains(game->id)||(builder.busy()&&builder.gameId()==game->id))return;
    if(inPlacePackage(game->source)&&!game->sourceAvailable()){message("Source unavailable","Reconnect the drive containing the source package, or update the source in Properties and rebuild.",true);return;}
    if(game->renderer=="vulkan"&&(!dotnetReady||tools.python.isEmpty())){message("Shader tools need setup","Vulkan requires Python and .NET 10. Check Tools → Settings → Toolkit.",true);return;}
    if(game->renderer=="vulkan"&&!QFileInfo(QDir(game->buildDir).filePath("tools/nxshader/nxs_live.py")).exists()){
        message("Shader tools are missing","Rebuild this game with the current toolkit to stage its Vulkan shader tools before playing.",true);return;
    }
    QString error;if(!writeRenderer(game->iniPath(),game->renderer,&error)){message("Cannot save game settings",error,true);return;}
    QProcess process;process.setProgram(game->executable());process.setWorkingDirectory(game->buildDir);auto env=tools.environment();
    process.setProcessEnvironment(env);qint64 pid=0;if(!process.startDetached(&pid)){message("Could not launch",process.errorString(),true);return;}
    running[game->id]=pid;game->lastPlayed=QDateTime::currentDateTime();save();refreshLibrary();jobLabel->setText("Game launched");
}
void Window::stop(){
    auto* game=selected();if(!game)return;
    if(builder.busy()&&builder.gameId()==game->id){builder.cancel();return;}
    if(!running.contains(game->id))return;
    if(requestGameClose(running.value(game->id),game->executable()))jobLabel->setText("Close requested — waiting for game");
    else message("Could not close game","Close the game from its own window.");
}
void Window::openGameFile(const QString& suffix){auto* game=selected();if(!game)return;const auto path=QDir(game->buildDir).filePath(game->outputName+suffix);if(!QFileInfo::exists(path)){message("File not available","Run this game once to create its log and settings.");return;}QDesktopServices::openUrl(QUrl::fromLocalFile(path));}
void Window::openFolder(bool saves){auto* game=selected();if(!game)return;const QString path=saves?QDir(game->dataDir()).filePath("save"):game->buildDir;if(!QDir(path).exists()){message("Folder not available",saves?"This game has not created a save folder yet.":"Build this game first to create its output folder.");return;}QDesktopServices::openUrl(QUrl::fromLocalFile(path));}
void Window::removeGame(){
    auto* game=selected();if(!game||running.contains(game->id)||(builder.busy()&&builder.gameId()==game->id))return;
    if(QMessageBox::question(this,"Remove from library","Remove “"+game->title+"” from the library?\n\nYour package, builds, and saves will not be deleted.")!=QMessageBox::Yes)return;
    for(int i=0;i<library.games.size();++i)if(library.games[i].id==selectedId){library.games.removeAt(i);break;}
    selectedId.clear();save();refreshLibrary();
}
void Window::showContextMenu(const QPoint& point){QMenu menu(this);menu.addAction(playAction);menu.addAction(buildAction);menu.addAction(propertiesAction);menu.addSeparator();menu.addAction(folderAction);menu.addAction(saveFolderAction);menu.addAction(logAction);menu.addSeparator();menu.addAction(removeAction);menu.exec(point);}
void Window::dragEnterEvent(QDragEnterEvent* event){if(event->mimeData()->hasUrls())event->acceptProposedAction();}
void Window::dropEvent(QDropEvent* event){for(const auto& url:event->mimeData()->urls()){if(!url.isLocalFile())continue;const auto path=url.toLocalFile();Game game;QString error;if(gameFromExecutable(path,&game,&error))importBuild(path);else importSource(path);}event->acceptProposedAction();}
void Window::closeEvent(QCloseEvent* event){
    if(builder.busy()){if(!closing&&QMessageBox::question(this,"Build in progress","Cancel the build and close the launcher?")!=QMessageBox::Yes){event->ignore();return;}closing=true;builder.cancel();event->ignore();return;}
    library.geometry=saveGeometry();library.dockState=saveState(1);save();event->accept();
}
void Window::seedDemo(){
    library.games.clear();library.viewMode=0;library.coverWidth=175;
    const QStringList names{"Donkey Kong Bananza","Mario Kart World","Homebrew collection","Another game","Test application","Local collection"};
    for(int i=0;i<names.size();++i){Game game;game.id=QString::number(i);game.title=names[i];game.outputName=names[i];game.source="Games/"+names[i]+(i==2?".nro":".nup");game.buildDir="Games/Native builds/"+names[i];library.games.append(game);}selectedId="0";
}
