// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QProcessEnvironment>

#ifdef Q_OS_WIN
inline constexpr auto executableSuffix = ".exe";
inline constexpr auto moduleSuffix = "_code.dll";
inline constexpr auto nativeTarget = "win64";
inline constexpr auto nativeBackend = "win32";
#else
inline constexpr auto executableSuffix = "";
inline constexpr auto moduleSuffix = "_code.so";
inline constexpr auto nativeTarget = "linux";
inline constexpr auto nativeBackend = "x11";
#endif

struct Game {
    QString id, title, source, buildDir, outputName, cover, background;
    QString renderer = "vulkan";
    QString builtSource;
    bool buildIncomplete = false;
    int workers = 0; // Zero inherits the launcher default.
    int optimization = 2;
    QDateTime lastPlayed, lastBuilt;
    QString executable() const;
    QString codeModule() const;
    QString dataDir() const;
    QString iniPath() const;
    QString format() const;
    bool hasBuild() const;
    bool sourceAvailable() const;
    QJsonObject json() const;
    static Game fromJson(const QJsonObject& object);
};

struct Toolchain {
    QString root, python, compiler, runtime, dotnet;
    bool canBuild() const;
    QString problem() const;
    QProcessEnvironment environment() const;
    static Toolchain find(const QString& root);
    static QString discoverRoot();
};

class Library {
public:
    explicit Library(QString file);
    bool load(QString* error);
    bool save(QString* error) const;
    Game* find(const QString& id);
    const Game* find(const QString& id) const;
    bool add(Game game);
    QString path;
    QString toolRoot, outputRoot;
    int jobs = 8;
    int viewMode = 0, coverWidth = 180;
    bool showBackground = true;
    QString colorMode = "dark", accentColor = "lime";
    QByteArray geometry, dockState;
    QVector<Game> games;
private:
    bool writable = true;
};

QString outputNameFor(const QString& title);
QString titleForSource(const QString& source);
bool validSource(const QString& source, QString* error);
Game gameFromSource(const QString& source, const QString& outputRoot = {});
bool gameFromExecutable(const QString& path, Game* game, QString* error);
bool writeRenderer(const QString& ini, const QString& renderer, QString* error);
QString readRenderer(const QString& ini);
bool samePath(const QString& left, const QString& right);

struct LibraryFilter {
    QString text;
    int state = 0; // All, Ready, Needs build, Running.
    int source = 0; // All, NUP, NRO, extracted, existing native build.
    bool matches(const Game& game, bool running = false, bool building = false) const;
    int activeCount() const;
};

struct CardState {
    QString action, status;
    bool actionEnabled = false, needsAttention = false, building = false, running = false;
};
CardState cardState(const Game& game, bool running, bool building, bool otherBuild, bool toolsReady);
