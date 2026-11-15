// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
#include "processwatch.h"
#include <QDir>
#include <QFileInfo>
#include <QSet>
#ifdef Q_OS_LINUX
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
namespace {
QString processPath(DWORD pid){
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!process)return {};
    std::vector<wchar_t> path(32768);DWORD size=DWORD(path.size());QString result;
    if(QueryFullProcessImageNameW(process,0,path.data(),&size))result=QString::fromWCharArray(path.data(),int(size));
    CloseHandle(process);return QDir::cleanPath(QDir::fromNativeSeparators(result));
}
}
#endif
QMap<QString,qint64> findRunningGames(const QVector<Game>& games){
    QMap<QString,qint64> result;
#ifdef Q_OS_WIN
    QSet<QString> names;for(const auto& game:games)names.insert(QFileInfo(game.executable()).fileName().toLower());
    if(names.isEmpty())return result;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE)return result;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof entry;
    for(BOOL more=Process32FirstW(snapshot,&entry);more;more=Process32NextW(snapshot,&entry)){
        if(!names.contains(QString::fromWCharArray(entry.szExeFile).toLower()))continue;
        const auto path=processPath(entry.th32ProcessID);if(path.isEmpty())continue;
        for(const auto& game:games)if(path.compare(QDir::cleanPath(QFileInfo(game.executable()).absoluteFilePath()),Qt::CaseInsensitive)==0&&!result.contains(game.id))result[game.id]=entry.th32ProcessID;
    }
    CloseHandle(snapshot);
#elif defined(Q_OS_LINUX)
    for (const auto& entry : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool ok = false; const auto pid = entry.toLongLong(&ok); if (!ok) continue;
        const auto path = QFileInfo("/proc/" + entry + "/exe").symLinkTarget();
        if (path.isEmpty()) continue;
        for (const auto& game : games) if (samePath(path, game.executable()) && !result.contains(game.id)) result[game.id] = pid;
    }
#else
    Q_UNUSED(games);
#endif
    return result;
}
bool requestGameClose(qint64 pid,const QString& executable){
#ifdef Q_OS_WIN
    if(processPath(DWORD(pid)).compare(QDir::cleanPath(QFileInfo(executable).absoluteFilePath()),Qt::CaseInsensitive)!=0)return false;
    struct Request{DWORD pid;bool sent=false;}request{DWORD(pid)};
    EnumWindows([](HWND window,LPARAM data)->BOOL{auto* request=reinterpret_cast<Request*>(data);DWORD id=0;GetWindowThreadProcessId(window,&id);if(id==request->pid&&IsWindowVisible(window)){PostMessageW(window,WM_CLOSE,0,0);request->sent=true;}return TRUE;},reinterpret_cast<LPARAM>(&request));
    return request.sent;
#elif defined(Q_OS_LINUX)
    if (pid <= 0) return false;
    const int fd = int(syscall(SYS_pidfd_open, pid, 0)); if (fd < 0) return false;
    const auto path = QFileInfo(QString("/proc/%1/exe").arg(pid)).symLinkTarget();
    const bool sent = !path.isEmpty() && samePath(path, executable) && syscall(SYS_pidfd_send_signal, fd, SIGTERM, nullptr, 0) == 0;
    ::close(fd); return sent;
#else
    Q_UNUSED(pid);Q_UNUSED(executable);return false;
#endif
}
