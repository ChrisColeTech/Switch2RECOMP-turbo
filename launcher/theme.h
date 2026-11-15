// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
#pragma once
#include <QColor>
#include <QIcon>
#include <QString>
#include <QVector>
class QApplication;
namespace Theme {
struct Colors {
    QColor canvas, surface, raised, field, text, muted, accent, accentText, selected, border, hover;
    bool light = false;
};
struct Accent { QString id, label, dark, light; };
QVector<Accent> accents();
Colors colorsFor(const QString& mode, const QString& accent);
const Colors& colors();
void apply(QApplication& app, const QString& mode = "dark", const QString& accent = "lime");
QString stylesheet(const Colors& colors);
QIcon appIcon();
}
