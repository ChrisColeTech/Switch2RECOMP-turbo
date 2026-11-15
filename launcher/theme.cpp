// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
#include "theme.h"
#include <QApplication>
#include <QPainter>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>

namespace {
Theme::Colors current=Theme::colorsFor("dark","lime");
QColor blend(QColor a,QColor b,double amount){return QColor::fromRgbF(a.redF()*(1-amount)+b.redF()*amount,a.greenF()*(1-amount)+b.greenF()*amount,a.blueF()*(1-amount)+b.blueF()*amount);}
class SurfaceFocusStyle:public QProxyStyle {
public:
    SurfaceFocusStyle():QProxyStyle(QStyleFactory::create("Fusion")){}
    void drawPrimitive(PrimitiveElement element,const QStyleOption* option,QPainter* painter,const QWidget* widget=nullptr)const override{
        if(element!=PE_FrameFocusRect)QProxyStyle::drawPrimitive(element,option,painter,widget);
    }
};
}
QVector<Theme::Accent> Theme::accents(){return {{"lime","Lime","#b5f36b","#38711b"},{"mint","Mint","#78e4ba","#006b50"},{"violet","Violet","#c6adff","#6741c0"},{"rose","Rose","#ff9cbd","#ab2456"},{"cyan","Cyan","#77dbf2","#006b88"}};}
Theme::Colors Theme::colorsFor(const QString& mode,const QString& name){
    Colors p;p.light=mode=="light";
    p.canvas=QColor(p.light?"#e8ebe9":"#111312");p.surface=QColor(p.light?"#f5f6f5":"#1c1f1d");p.raised=QColor(p.light?"#ffffff":"#292e2b");p.field=QColor(p.light?"#ffffff":"#151916");
    p.text=QColor(p.light?"#202622":"#f1f5f2");p.muted=QColor(p.light?"#505e54":"#b5c0b8");p.border=QColor(p.light?"#a2aea5":"#49594e");p.hover=QColor(p.light?"#e0e7e2":"#354138");
    auto choices=accents();auto chosen=choices.front();for(const auto& value:choices)if(value.id==name)chosen=value;
    p.accent=QColor(p.light?chosen.light:chosen.dark);p.accentText=QColor(p.light?"#ffffff":"#10180b");p.selected=blend(p.surface,p.accent,p.light?.10:.13);
    return p;
}
const Theme::Colors& Theme::colors(){return current;}
void Theme::apply(QApplication& app,const QString& mode,const QString& accent){
    static bool installed=false;if(!installed){app.setStyle(new SurfaceFocusStyle);installed=true;}
    current=colorsFor(mode,accent);const auto& c=current;
    QPalette p;p.setColor(QPalette::Window,c.surface);p.setColor(QPalette::WindowText,c.text);p.setColor(QPalette::Base,c.field);p.setColor(QPalette::AlternateBase,c.surface);
    p.setColor(QPalette::Text,c.text);p.setColor(QPalette::Button,c.raised);p.setColor(QPalette::ButtonText,c.text);p.setColor(QPalette::Highlight,c.selected);p.setColor(QPalette::HighlightedText,c.text);
    p.setColor(QPalette::PlaceholderText,c.muted);p.setColor(QPalette::ToolTipBase,c.raised);p.setColor(QPalette::ToolTipText,c.text);p.setColor(QPalette::Link,c.accent);
    for(auto role:{QPalette::Text,QPalette::ButtonText,QPalette::WindowText})p.setColor(QPalette::Disabled,role,blend(c.muted,c.surface,.45));
    app.setPalette(p);app.setStyleSheet(stylesheet(c));app.setWindowIcon(appIcon());
}
QIcon Theme::appIcon(){
    QPixmap pix(96,96);pix.fill(Qt::transparent);QPainter p(&pix);p.setRenderHint(QPainter::Antialiasing);p.setPen(Qt::NoPen);p.setBrush(current.accent);p.drawRoundedRect(pix.rect(),18,18);
    p.setPen(current.accentText);p.setFont(QFont("Segoe UI",42,QFont::Black));p.drawText(pix.rect(),Qt::AlignCenter,"S/");return QIcon(pix);
}
QString Theme::stylesheet(const Colors& c){
    QString css=QStringLiteral(R"(
QWidget { color: @text; font-family: 'Segoe UI'; font-size: 13px; }
QMainWindow, QDialog { background: @surface; }
QMenuBar { background: @surface; padding: 2px 5px; border-bottom: 1px solid @border; }
QMenuBar::item { padding: 4px 10px; background: transparent; }
QMenuBar::item:selected, QMenu::item:selected { background: @selected; }
QMenu { background: @raised; border: 1px solid @border; padding: 5px; }
QMenu::item { padding: 7px 28px; }
QMenu::separator { height: 1px; background: @border; margin: 5px; }
QToolBar#Commands { background: @surface; border-bottom: 1px solid @border; spacing: 4px; padding: 4px 8px; }
QToolBar#Commands QToolButton { background: transparent; border: none; border-radius: 5px; padding: 6px 11px; min-width: 55px; min-height: 51px; font-size: 12px; }
QToolButton:hover, QToolButton:focus { background: @hover; }
QToolButton:pressed, QToolButton:checked { background: @selected; }
QToolButton#AddGameButton::menu-button { background: transparent; border: none; width: 13px; }
QToolBar::separator { background: @border; width: 1px; margin: 10px 8px; }
QLabel { background: transparent; border: none; }
QLabel#Muted { color: @muted; }
QLabel#Empty { color: @muted; font-size: 14px; }
QLabel#PopupTitle { font-size: 17px; font-weight: 650; }
QLabel#FieldLabel { color: @text; font-size: 12px; font-weight: 600; }
QLabel#ResultSummary { color: @muted; font-size: 12px; }
QPushButton { background: @raised; border: 1px solid @border; border-radius: 5px; padding: 5px 12px; min-height: 20px; }
QPushButton:hover, QPushButton:focus { background: @hover; }
QPushButton:pressed { background: @selected; }
QPushButton:disabled, QToolButton:disabled { color: @disabled; }
QPushButton:default, QPushButton#AccentButton { background: @accent; color: @accentText; border: 1px solid @accent; font-weight: 650; }
QPushButton#AccentButton:hover, QPushButton:default:hover { background: @accentHover; }
QPushButton#CardPrimary { border: none; background: @raised; font-weight: 600; padding: 4px 8px; }
QPushButton#CardPrimary:hover, QPushButton#CardPrimary:focus { background: @selected; color: @accent; }
QToolButton#CardProperties { background: @raised; border: none; border-radius: 5px; padding: 4px; }
QToolButton#CardProperties:hover, QToolButton#CardProperties:focus { background: @selected; }
QLineEdit, QComboBox, QSpinBox { background: @field; border: 1px solid @border; border-radius: 5px; padding: 6px 9px; min-height: 20px; selection-background-color: @selected; selection-color: @text; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { background: @hover; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox::down-arrow { image: url(@chevron); width: 12px; height: 12px; }
QComboBox QAbstractItemView { background: @raised; color: @text; border: 1px solid @border; selection-background-color: @selected; selection-color: @text; padding: 4px; }
QListWidget { background: transparent; border: none; outline: none; }
QListWidget::item { background: transparent; border: none; outline: none; }
QTreeWidget { background: @field; alternate-background-color: @surface; border: none; outline: none; }
QTreeWidget::item { height: 32px; padding-left: 6px; border: none; }
QTreeWidget::item:hover { background: @hover; }
QTreeWidget::item:selected { background: @selected; color: @text; }
QHeaderView::section { background: @raised; color: @text; font-weight: 600; border: none; border-right: 1px solid @border; border-bottom: 1px solid @border; padding: 8px; text-align: left; }
QHeaderView::section:hover { background: @hover; }
QPlainTextEdit { background: @field; border: none; font-family: 'Cascadia Mono', 'Consolas'; font-size: 12px; padding: 7px; color: @text; }
QDockWidget { border: 1px solid @border; }
QDockWidget::title { background: @raised; padding: 6px 8px; text-align: left; }
QStatusBar { background: @surface; border-top: 1px solid @border; }
QStatusBar::item { border: none; }
QToolButton#StatusSettings { border: none; background: transparent; padding: 3px 9px; }
QToolButton#StatusSettings:hover, QToolButton#StatusSettings:focus { background: @hover; }
QProgressBar { background: @border; border: none; border-radius: 3px; min-height: 7px; max-height: 7px; }
QProgressBar::chunk { background: @accent; border-radius: 3px; }
QSlider::groove:horizontal { background: @border; height: 3px; }
QSlider::handle:horizontal { background: @accent; border: none; border-radius: 5px; width: 11px; margin: -5px 0; }
QScrollBar:vertical { background: @surface; width: 10px; }
QScrollBar::handle:vertical { background: @border; border-radius: 4px; min-height: 25px; }
QScrollBar::handle:vertical:hover { background: @muted; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QTabWidget::pane { border: 1px solid @border; background: @surface; }
QTabBar::tab { background: @raised; color: @muted; padding: 8px 16px; border: none; }
QTabBar::tab:selected { background: @selected; color: @text; }
QCheckBox { spacing: 7px; }
QToolTip { background: @raised; color: @text; border: 1px solid @border; padding: 6px; }
QFrame#FilterCard { background: @raised; border: 1px solid @border; border-radius: 8px; }
QFrame#FilterDivider { background: @border; border: none; max-height: 1px; }
QToolButton#ViewChoice { background: @field; border: none; border-radius: 5px; padding: 7px 13px; }
QToolButton#ViewChoice:checked { background: @selected; color: @accent; }
QToolButton#ViewChoice:hover { background: @hover; }
)");
    const QVector<QPair<QString,QColor>> values{{"accentHover",blend(c.accent,c.light?QColor(Qt::black):QColor(Qt::white),.10)},
        {"accentText",c.accentText},{"disabled",blend(c.muted,c.surface,.45)},{"selected",c.selected},{"surface",c.surface},{"raised",c.raised},
        {"canvas",c.canvas},{"field",c.field},{"muted",c.muted},{"accent",c.accent},{"border",c.border},{"hover",c.hover},{"text",c.text}};
    for(const auto& entry:values)css.replace("@"+entry.first,entry.second.name());
    css.replace("@chevron",c.light?":/icons/chevron-down-dark.svg":":/icons/chevron-down.svg");
    return css;
}
