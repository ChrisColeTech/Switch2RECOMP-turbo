// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
// Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
#pragma once
#include "../runtime/splash_img.h"
#include <QImage>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <utility>

class StartupScreen:public QWidget {
public:
    explicit StartupScreen(std::function<void()> ready):QWidget(nullptr,Qt::SplashScreen),ready_(std::move(ready)),image_(SPLASH_W,SPLASH_H,QImage::Format_Grayscale8) {
        splash_unpack(image_.bits());
        setWindowTitle("SwitchRecomp");
        setAttribute(Qt::WA_OpaquePaintEvent);
        const QRect available=screen()->availableGeometry();
        resize(QSize(SPLASH_W,SPLASH_H).boundedTo(available.size()));
        move(available.center()-rect().center());
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);p.fillRect(rect(),Qt::black);
        const QSize size=image_.size().scaled(this->size(),Qt::KeepAspectRatio);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(QRect(QPoint((width()-size.width())/2,(height()-size.height())/2),size),image_);
        // start at the first paint; no wait for a window-manager exposure (offscreen has none).
        if(!started_){started_=true;QTimer::singleShot(1000,Qt::PreciseTimer,this,[this]{hide();ready_();});}
    }
private:
    std::function<void()> ready_;
    QImage image_;
    bool started_=false;
};
