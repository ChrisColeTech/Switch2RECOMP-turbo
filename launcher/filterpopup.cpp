// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
#include "filterpopup.h"
#include <QComboBox>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QToolButton>
#include <QSlider>
#include <QVBoxLayout>

FilterPopup::FilterPopup(QWidget* parent):QFrame(parent, Qt::Popup|Qt::FramelessWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAccessibleName("Search and filter games");setWindowTitle("Search & Filter");
    auto* outer=new QVBoxLayout(this);outer->setContentsMargins(14,10,14,18);
    auto* card=new QFrame;card->setObjectName("FilterCard");outer->addWidget(card);
    auto* shadow=new QGraphicsDropShadowEffect(card);shadow->setBlurRadius(24);shadow->setOffset(0,6);shadow->setColor(QColor(0,0,0,190));card->setGraphicsEffect(shadow);
    auto* layout=new QVBoxLayout(card);layout->setContentsMargins(18,16,18,16);layout->setSpacing(10);
    auto* heading=new QHBoxLayout;
    auto* title=new QLabel("Search & Filter");title->setObjectName("PopupTitle");heading->addWidget(title);heading->addStretch();
    auto* close=new QPushButton("×");close->setAccessibleName("Close search and filter");close->setFixedWidth(30);close->setToolTip("Close (Esc)");heading->addWidget(close);connect(close,&QPushButton::clicked,this,&QWidget::hide);
    layout->addLayout(heading);
    auto field=[layout](const QString& name,QWidget* control){auto* label=new QLabel(name);label->setObjectName("FieldLabel");label->setBuddy(control);layout->addWidget(label);layout->addWidget(control);};
    search=new QLineEdit;search->setPlaceholderText("Type a game title…");search->setClearButtonEnabled(true);search->setAccessibleName("Search game titles");field("&Search",search);
    auto* divider=new QFrame;divider->setObjectName("FilterDivider");divider->setFixedHeight(1);layout->addWidget(divider);
    state=new QComboBox;state->addItems({"All states","Ready to play","Needs build","Running"});state->setAccessibleName("Filter by build state");field("&Status",state);
    source=new QComboBox;source->addItems({"All sources","NUP / NSP packages","NRO homebrew","Extracted folders","Native builds"});source->setAccessibleName("Filter by source format");field("S&ource",source);
    sort=new QComboBox;sort->addItems({"Name (A–Z)","Last played (newest first)","Build status"});sort->setPlaceholderText("Custom table order");sort->setAccessibleName("Sort games");field("Sort &by",sort);
    auto* viewLabel=new QLabel("Library view");viewLabel->setObjectName("FieldLabel");layout->addWidget(viewLabel);
    auto* choices=new QHBoxLayout;galleryButton=new QToolButton;listButton=new QToolButton;
    for(auto* choice:{galleryButton,listButton}){choice->setObjectName("ViewChoice");choice->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);choice->setIconSize({18,18});choice->setFocusPolicy(Qt::StrongFocus);choices->addWidget(choice,1);}
    layout->addLayout(choices);
    coverSize=new QSlider(Qt::Horizontal);coverSize->setRange(130,240);coverSize->setAccessibleName("Gallery card size");field("Card size",coverSize);
    summary=new QLabel;summary->setObjectName("ResultSummary");layout->addWidget(summary);
    auto* actions=new QHBoxLayout;auto* clear=new QPushButton("Clear filters");actions->addWidget(clear);actions->addStretch();auto* done=new QPushButton("Done");done->setObjectName("AccentButton");actions->addWidget(done);layout->addLayout(actions);
    connect(clear,&QPushButton::clicked,this,&FilterPopup::clearFilters);connect(done,&QPushButton::clicked,this,&QWidget::hide);
    connect(search,&QLineEdit::textChanged,this,[this]{emit filtersChanged();});
    for(auto* combo:{state,source,sort})connect(combo,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{emit filtersChanged();});
    setFixedWidth(390);adjustSize();
}
void FilterPopup::openBelow(QWidget* anchor) {
    adjustSize();
    QPoint location=anchor->mapToGlobal(QPoint(anchor->width()-width()+14,anchor->height()-4));
    if(auto* screen=anchor->screen()){
        const QRect available=screen->availableGeometry();
        location.setX(qBound(available.left(),location.x(),qMax(available.left(),available.right()-width()+1)));
        location.setY(qBound(available.top(),location.y(),qMax(available.top(),available.bottom()-height()+1)));
    }
    move(location);show();raise();search->setFocus();search->selectAll();
}
void FilterPopup::setResultCount(int visible,int total) {summary->setText(QString("%1 of %2 games shown").arg(visible).arg(total));}
void FilterPopup::clearFilters() {
    {QSignalBlocker a(search),b(state),c(source),d(sort);search->clear();state->setCurrentIndex(0);source->setCurrentIndex(0);sort->setCurrentIndex(0);}
    emit filtersChanged();search->setFocus();
}
void FilterPopup::keyPressEvent(QKeyEvent* event) {
    if(event->key()==Qt::Key_Escape){hide();event->accept();return;}
    if(event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter){hide();event->accept();return;}
    QFrame::keyPressEvent(event);
}
