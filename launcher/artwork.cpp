// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
#include "artwork.h"
#include "theme.h"
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QPushButton>
#include <QStyleOptionViewItem>
#include <QToolButton>
#include <QVBoxLayout>

QPixmap loadArtwork(const QString& path,QString* error){
    if(path.isEmpty())return {};
    QFileInfo info(path);const QString key=info.absoluteFilePath()+"|"+QString::number(info.size())+"|"+QString::number(info.lastModified().toMSecsSinceEpoch());
    QPixmap image;if(QPixmapCache::find(key,&image))return image;
    QImageReader reader(path);reader.setAutoTransform(true);
    QSize size=reader.size();if(size.width()>2048||size.height()>2048){size.scale(2048,2048,Qt::KeepAspectRatio);reader.setScaledSize(size);}
    auto decoded=reader.read();if(decoded.isNull()){if(error)*error=reader.errorString();return {};}
    image=QPixmap::fromImage(decoded);QPixmapCache::insert(key,image);return image;
}
void invalidateArtwork(){QPixmapCache::clear();}
namespace {
void cropImage(QPainter& painter,QRectF target,const QPixmap& image){
    QSizeF scaled=image.size();scaled.scale(target.size(),Qt::KeepAspectRatioByExpanding);double factor=scaled.width()/image.width();
    QSizeF source(target.width()/factor,target.height()/factor);
    painter.drawPixmap(target,image,QRectF((image.width()-source.width())/2,(image.height()-source.height())/2,source.width(),source.height()));
}
QIcon localIcon(const QString& action){
    QPixmap pix(72,72);pix.fill(Qt::transparent);QPainter p(&pix);p.scale(3,3);p.setRenderHint(QPainter::Antialiasing);p.setPen(QPen(Theme::colors().text,1.6,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    if(action=="Play"){QPainterPath t;t.moveTo(8,5);t.lineTo(19,12);t.lineTo(8,19);t.closeSubpath();p.fillPath(t,Theme::colors().accent);}
    else if(action=="Build"){p.drawLine(6,19,17,7);p.drawLine(11,4,21,13);p.drawLine(17,3,22,8);}
    else if(action=="Stop"){p.setBrush(Theme::colors().text);p.drawRoundedRect(QRectF(6,6,12,12),2,2);}
    else if(action=="Cancel build"){p.drawLine(6,6,18,18);p.drawLine(18,6,6,18);}
    else {p.drawLine(4,7,20,7);p.drawLine(4,17,20,17);p.setBrush(Theme::colors().raised);p.drawEllipse(QPointF(9,7),3,3);p.drawEllipse(QPointF(15,17),3,3);}
    return QIcon(pix);
}
}

LibraryCanvas::LibraryCanvas(QWidget* parent):QWidget(parent){setAutoFillBackground(false);}
void LibraryCanvas::setBackground(const QString& path,bool show){enabled=show;image=loadArtwork(path);update();}
void LibraryCanvas::paintEvent(QPaintEvent*){
    QPainter p(this);const auto& c=Theme::colors();p.fillRect(rect(),c.canvas);
    if(enabled&&!image.isNull()){
        p.setOpacity(c.light?.20:.32);cropImage(p,rect(),image);p.setOpacity(1);
        QLinearGradient fade(0,0,0,height());QColor top=c.canvas,bottom=c.canvas;top.setAlpha(15);bottom.setAlpha(210);fade.setColorAt(0,top);fade.setColorAt(1,bottom);p.fillRect(rect(),fade);
    }
}
CoverView::CoverView(QWidget* parent):QWidget(parent){setCursor(Qt::PointingHandCursor);}
void CoverView::setArtwork(const QString& value){path=value;error.clear();image=loadArtwork(path,&error);setToolTip(error.isEmpty()?(path.isEmpty()?"Choose artwork in Properties":path):"Cannot load artwork: "+error);update();}
void CoverView::setState(const CardState& value){state=value;setToolTip((state.needsAttention?state.status+"\n":QString{})+(error.isEmpty()?(path.isEmpty()?"Choose artwork in Properties":path):"Cannot load artwork: "+error));update();}
void CoverView::setProgress(int value,int max){current=value;total=max;update();}
void CoverView::mousePressEvent(QMouseEvent* e){if(e->button()==Qt::LeftButton){emit clicked();e->accept();}else QWidget::mousePressEvent(e);}
void CoverView::mouseDoubleClickEvent(QMouseEvent* e){if(e->button()==Qt::LeftButton){emit activated();e->accept();}else QWidget::mouseDoubleClickEvent(e);}
void CoverView::paintEvent(QPaintEvent*){
    QPainter p(this);p.setRenderHint(QPainter::Antialiasing);const auto& c=Theme::colors();
    QPainterPath clip;clip.addRoundedRect(QRectF(0,0,width(),height()+5),5,5);p.setClipPath(clip);
    if(!image.isNull())cropImage(p,rect(),image);
    else {
        QLinearGradient gradient(0,0,width(),height());gradient.setColorAt(0,c.raised);gradient.setColorAt(1,c.surface);p.fillRect(rect(),gradient);
        p.save();p.translate(width()/2.0,height()/2.0-7);p.setPen(QPen(c.muted,1.6));p.setBrush(Qt::NoBrush);p.drawRoundedRect(QRectF(-25,-16,50,32),10,10);
        p.drawLine(-17,0,-5,0);p.drawLine(-11,-6,-11,6);p.drawEllipse(QPointF(11,-4),2,2);p.drawEllipse(QPointF(17,3),2,2);p.restore();
        p.setFont(QFont("Segoe UI",8));p.setPen(c.muted);p.drawText(rect().adjusted(8,0,-8,-13),Qt::AlignHCenter|Qt::AlignBottom,error.isEmpty()?"Add artwork":"Image unavailable");
    }
    if(state.needsAttention){
        const QRectF badge(width()-31,8,23,23);p.setPen(Qt::NoPen);QColor shade=c.surface;shade.setAlpha(235);p.setBrush(shade);p.drawEllipse(badge);
        p.setPen(QPen(c.accent,1.8,Qt::SolidLine,Qt::RoundCap));const auto center=badge.center();
        if(state.status=="Needs build"){p.drawLine(center+QPointF(-5,5),center+QPointF(5,-5));p.drawLine(center+QPointF(0,-6),center+QPointF(6,0));}
        else {p.drawLine(center+QPointF(0,-5),center+QPointF(0,1));p.drawPoint(center+QPointF(0,5));}
    }
    if(state.building&&total>0){p.fillRect(QRectF(0,height()-3,width(),3),c.border);p.fillRect(QRectF(0,height()-3,width()*qBound(0.0,double(current)/total,1.0),3),c.accent);}
}
GameCard::GameCard(const Game& game,const CardState& value,int width,QWidget* parent):QWidget(parent),id(game.id),title(game.title),state(value){
    setAutoFillBackground(false);setAccessibleName(title+", "+state.status);auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
    cover=new CoverView;cover->setArtwork(game.cover);cover->setState(state);layout->addWidget(cover);
    footer=new QLabel(title);footer->setTextFormat(Qt::PlainText);footer->setWordWrap(true);footer->setAlignment(Qt::AlignVCenter|Qt::AlignLeft);footer->setFixedHeight(42);footer->setToolTip(title+"\n"+state.status);layout->addWidget(footer);
    layout->addSpacing(6);auto* row=new QHBoxLayout;row->setContentsMargins(0,0,0,0);row->setSpacing(5);
    primary=new QPushButton(state.action);primary->setObjectName("CardPrimary");primary->setIconSize({16,16});primary->setFixedHeight(30);primary->setEnabled(state.actionEnabled);primary->setAccessibleName(state.action+" "+title);primary->setToolTip(state.status);
    properties=new QToolButton;properties->setObjectName("CardProperties");properties->setIconSize({18,18});properties->setFixedSize(32,30);properties->setToolTip("Properties — "+title);properties->setAccessibleName("Properties for "+title);properties->setFocusPolicy(Qt::StrongFocus);
    row->addWidget(primary,1);row->addWidget(properties);layout->addLayout(row);
    connect(cover,&CoverView::clicked,this,[this]{emit selected(id);});
    connect(cover,&CoverView::activated,this,[this]{emit selected(id);if(state.action=="Play")emit primaryRequested(id);else emit propertiesRequested(id);});
    connect(primary,&QPushButton::clicked,this,[this]{emit selected(id);emit primaryRequested(id);});
    connect(properties,&QToolButton::clicked,this,[this]{emit selected(id);emit propertiesRequested(id);});
    setCardWidth(width);refreshAppearance();
}
void GameCard::setCardWidth(int width){setFixedSize(width,heightFor(width));cover->setFixedHeight(int(width*.64));}
void GameCard::setSelected(bool value){isSelected=value;refreshAppearance();}
void GameCard::refreshAppearance(){
    const auto& c=Theme::colors();footer->setStyleSheet(QString("QLabel {background:%1;color:%2;padding:4px 9px;font-weight:600;border-bottom-left-radius:5px;border-bottom-right-radius:5px;}").arg((isSelected?c.selected:c.raised).name(),c.text.name()));
    primary->setIcon(localIcon(state.action));properties->setIcon(localIcon("properties"));cover->update();update();
}
void GameCard::setBuildProgress(const QString& phase,int current,int total){cover->setProgress(current,total);primary->setToolTip(total?QString("%1: %2/%3").arg(phase).arg(current).arg(total):phase);}
void GameCard::mousePressEvent(QMouseEvent* e){if(e->button()==Qt::LeftButton)emit selected(id);QWidget::mousePressEvent(e);}
void TableDelegate::paint(QPainter* p,const QStyleOptionViewItem& option,const QModelIndex& index)const{QStyleOptionViewItem clean(option);clean.state&=~QStyle::State_HasFocus;QStyledItemDelegate::paint(p,clean,index);}
