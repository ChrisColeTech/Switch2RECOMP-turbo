// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
#include "filterpopup.h"
#include "theme.h"
#include "startup.h"
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QSignalSpy>
#include <QtTest>

class FilterPopupTests:public QObject {
    Q_OBJECT
private slots:
    void initTestCase(){QApplication::setStyle("Fusion");Theme::apply(*qApp);}
    void clearsAsOneUpdate(){
        QWidget owner;FilterPopup popup(&owner);popup.search->setText("Bananza");popup.state->setCurrentIndex(2);popup.source->setCurrentIndex(1);popup.sort->setCurrentIndex(2);
        QSignalSpy changed(&popup,&FilterPopup::filtersChanged);popup.clearFilters();
        QCOMPARE(changed.count(),1);QVERIFY(popup.search->text().isEmpty());QCOMPARE(popup.state->currentIndex(),0);QCOMPARE(popup.source->currentIndex(),0);QCOMPARE(popup.sort->currentIndex(),0);
    }
    void escapeClosesPopup(){
        QWidget owner;QPushButton anchor("Find games",&owner);owner.resize(400,300);anchor.resize(100,70);owner.show();
        FilterPopup popup(&owner);popup.openBelow(&anchor);QTRY_VERIFY(popup.isVisible());
        QVERIFY(!popup.search->accessibleName().isEmpty());QTest::keyClick(popup.search,Qt::Key_Escape);QTRY_VERIFY(!popup.isVisible());
    }
    void startupArtworkAndHandoff(){
        QWidget library;int finished=0;bool eventProcessed=false;QElapsedTimer elapsed;
        StartupScreen startup([&]{++finished;library.show();});
        startup.resize(SPLASH_W,SPLASH_H);elapsed.start();startup.show();
        QTimer::singleShot(0,&startup,[&]{eventProcessed=true;});
        QImage expected(SPLASH_W,SPLASH_H,QImage::Format_Grayscale8);splash_unpack(expected.bits());
        QCOMPARE(startup.grab().toImage().convertToFormat(QImage::Format_Grayscale8),expected);
        const QString capture=qEnvironmentVariable("SWR_STARTUP_CAPTURE");
        if(!capture.isEmpty()) QVERIFY(startup.grab().save(capture));
        QTest::mouseClick(&startup,Qt::LeftButton);QVERIFY(startup.isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(eventProcessed,500);QCOMPARE(finished,0);
        QTRY_COMPARE_WITH_TIMEOUT(finished,1,3000);QVERIFY(elapsed.elapsed()>=1000);
        QVERIFY(!startup.isVisible());QVERIFY(library.isVisible());
        startup.show();startup.repaint();QTest::qWait(1100);QCOMPARE(finished,1);
    }
};
QTEST_MAIN(FilterPopupTests)
#include "gui_tests.moc"
