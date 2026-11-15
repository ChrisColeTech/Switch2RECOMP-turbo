// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
#pragma once
#include "library.h"
#include <QPixmap>
#include <QStyledItemDelegate>
#include <QWidget>
class QLabel;
class QPushButton;
class QToolButton;
class QMouseEvent;

QPixmap loadArtwork(const QString& path,QString* error=nullptr);
void invalidateArtwork();

class LibraryCanvas : public QWidget {
public:
    explicit LibraryCanvas(QWidget* parent=nullptr);
    void setBackground(const QString& path,bool enabled);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QPixmap image;
    bool enabled=true;
};

class CoverView : public QWidget {
    Q_OBJECT
public:
    explicit CoverView(QWidget* parent=nullptr);
    void setArtwork(const QString& path);
    void setState(const CardState& state);
    void setProgress(int current,int total);
signals:
    void clicked();
    void activated();
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
private:
    QPixmap image;
    QString path,error;
    CardState state;
    int current=0,total=0;
};

class GameCard : public QWidget {
    Q_OBJECT
public:
    GameCard(const Game& game,const CardState& state,int width,QWidget* parent=nullptr);
    static int heightFor(int width){return int(width*.64)+78;}
    void setCardWidth(int width);
    void setSelected(bool selected);
    void setBuildProgress(const QString& phase,int current,int total);
    void refreshAppearance();
signals:
    void selected(const QString& id);
    void primaryRequested(const QString& id);
    void propertiesRequested(const QString& id);
protected:
    void mousePressEvent(QMouseEvent*) override;
private:
    QString id,title;
    CoverView* cover=nullptr;
    QLabel* footer=nullptr;
    QPushButton* primary=nullptr;
    QToolButton* properties=nullptr;
    CardState state;
    bool isSelected=false;
};

class GalleryDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter*,const QStyleOptionViewItem&,const QModelIndex&)const override{}
};
class TableDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter*,const QStyleOptionViewItem&,const QModelIndex&)const override;
};
