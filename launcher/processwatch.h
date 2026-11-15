// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
#pragma once
#include "library.h"
#include <QMap>
QMap<QString,qint64> findRunningGames(const QVector<Game>& games);
bool requestGameClose(qint64 pid, const QString& executable);
