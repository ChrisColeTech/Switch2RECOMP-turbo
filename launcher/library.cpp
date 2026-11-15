// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
#include "library.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace {
QString absolute(const QString& path) { return path.isEmpty() ? QString{} : QFileInfo(path).absoluteFilePath(); }
QString tool(const QString& root, const QStringList& choices, const QString& fallback = {}) {
    for (const auto& candidate : choices) {
        const QString path = QDir(root).filePath(candidate);
        if (!root.isEmpty() && QFileInfo(path).isFile()) return absolute(path);
    }
    return fallback.isEmpty() ? QString{} : QStandardPaths::findExecutable(fallback);
}
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
}

QString Game::executable() const { return QDir(buildDir).filePath(outputName + executableSuffix); }
QString Game::codeModule() const { return QDir(buildDir).filePath(outputName + moduleSuffix); }
QString Game::dataDir() const { return QDir(buildDir).filePath(outputName + "_data"); }
QString Game::iniPath() const { return QDir(buildDir).filePath(outputName + ".ini"); }
QString Game::format() const {
    if (source.endsWith(".nup", Qt::CaseInsensitive)) return "DIRECT NUP";
    if (source.endsWith(".nsp", Qt::CaseInsensitive)) return "DIRECT NSP";
    if (source.endsWith(".nro", Qt::CaseInsensitive)) return "HOMEBREW";
    return source.isEmpty() ? "NATIVE BUILD" : "EXTRACTED";
}
bool Game::hasBuild() const {
    return !buildIncomplete && !buildDir.isEmpty() && !outputName.isEmpty() && QFileInfo(executable()).isFile()
        && QFileInfo(codeModule()).isFile() && QDir(dataDir()).exists()
        && (source.isEmpty() || samePath(builtSource, source));
}
bool Game::sourceAvailable() const { return !source.isEmpty() && QFileInfo::exists(source); }
QJsonObject Game::json() const {
    return {{"id", id}, {"title", title}, {"source", source}, {"buildDir", buildDir},
        {"outputName", outputName}, {"cover", cover}, {"background", background}, {"renderer", renderer},
        {"workers", workers}, {"optimization", optimization},
        {"builtSource", builtSource}, {"buildIncomplete", buildIncomplete}, {"lastPlayed", lastPlayed.toString(Qt::ISODate)},
        {"lastBuilt", lastBuilt.toString(Qt::ISODate)}};
}
Game Game::fromJson(const QJsonObject& o) {
    Game g;
    g.id = o["id"].toString(); g.title = o["title"].toString(); g.source = o["source"].toString();
    g.buildDir = o["buildDir"].toString(); g.outputName = o["outputName"].toString();
    g.cover = o["cover"].toString(); g.renderer = o["renderer"].toString("vulkan");
    g.background = o["background"].toString();
    g.workers = qBound(0, o["workers"].toInt(), 64);
    g.optimization = qBound(0, o["optimization"].toInt(2), 3);
    if (g.renderer != "vulkan" && g.renderer != "opengl") g.renderer = "opengl";
    g.builtSource = o["builtSource"].toString();
    g.buildIncomplete = o["buildIncomplete"].toBool();
    g.lastPlayed = QDateTime::fromString(o["lastPlayed"].toString(), Qt::ISODate);
    g.lastBuilt = QDateTime::fromString(o["lastBuilt"].toString(), Qt::ISODate);
    return g;
}

bool Toolchain::canBuild() const { return problem().isEmpty(); }
QString Toolchain::problem() const {
    if (root.isEmpty() || !QFileInfo(QDir(root).filePath("switchrecomp.py")).isFile()) return "Choose your Switch2Recomp folder in Settings.";
    if (python.isEmpty()) return "Python is missing. Extract the complete toolkit bundle or run the setup script.";
    if (compiler.isEmpty()) return "GCC is missing. Extract the complete toolkit bundle or run the setup script.";
    if (runtime.isEmpty()) return "The native runtime is missing. Extract the complete toolkit bundle or build the runtime first.";
    return {};
}
Toolchain Toolchain::find(const QString& root) {
    Toolchain t;
    t.root = absolute(root);
#ifdef Q_OS_WIN
    t.python = tool(root, {"python/python.exe"}, "python.exe");
    t.compiler = tool(root, {"gcc/Library/bin/x86_64-w64-mingw32-gcc.exe", "gcc/bin/gcc.exe", "w64devkit/bin/gcc.exe"}, "gcc.exe");
    t.runtime = tool(root, {"bin/runtime_win64_win32.exe"});
    t.dotnet = tool(root, {"dotnet/dotnet.exe"}, "dotnet.exe");
#else
    t.python = tool(root, {"python/bin/python3"}, "python3");
    t.compiler = tool(root, {"gcc/bin/x86_64-conda-linux-gnu-gcc", "gcc/bin/gcc"}, "gcc");
    t.runtime = tool(root, {"bin/runtime_linux_x11"});
    t.dotnet = tool(root, {"dotnet/dotnet"}, "dotnet");
#endif
    return t;
}
QProcessEnvironment Toolchain::environment() const {
    auto env = QProcessEnvironment::systemEnvironment();
    QStringList paths;
#ifdef Q_OS_WIN
    const QString shaderTools = QDir(root).filePath("shader-tools/Library/bin");
#else
    const QString shaderTools = QDir(root).filePath("shader-tools/bin");
#endif
    if (QDir(shaderTools).exists()) { paths << shaderTools; env.insert("NXSHADER_TOOLS", shaderTools); }
    paths << QDir(root).filePath("bin/launcher");
    for (const auto& binary : {python, compiler, dotnet}) if (!binary.isEmpty()) paths << QFileInfo(binary).absolutePath();
    paths << env.value("PATH"); env.insert("PATH", paths.join(QDir::listSeparator()));
    if (!python.isEmpty()) env.insert("SWR_PYTHON", python);
    if (!dotnet.isEmpty()) env.insert("DOTNET_ROOT", QFileInfo(dotnet).absolutePath());
    if (!keys.isEmpty()) env.insert("SWR_PROD_KEYS", keys);
#ifdef Q_OS_LINUX
    QStringList libs{QDir(root).filePath("lib"), QDir(root).filePath("gcc/lib"), QDir(root).filePath("shader-tools/lib")};
    if (!env.value("LD_LIBRARY_PATH").isEmpty()) libs << env.value("LD_LIBRARY_PATH");
    env.insert("LD_LIBRARY_PATH", libs.join(':'));
#endif
    return env;
}
QString Toolchain::discoverRoot() {
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 6; ++i) {
        if (QFileInfo(dir.filePath("switchrecomp.py")).isFile()) return dir.absolutePath();
        if (!dir.cdUp()) break;
    }
    return {};
}

Library::Library(QString file) : path(std::move(file)) {}
bool Library::load(QString* error) {
    QFile file(path);
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly)) { writable = false; return fail(error, file.errorString()); }
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject() || doc.object()["schema"].toInt() != 1) {
        writable = false;
        return fail(error, "The library file is invalid or from an unsupported version. It was left untouched: " + path);
    }
    const auto o = doc.object();
    toolRoot = o["toolRoot"].toString(); outputRoot = o["outputRoot"].toString(); keysPath = o["keysPath"].toString();
    jobs = qBound(1, o["jobs"].toInt(8), 64);
    viewMode = qBound(0, o["viewMode"].toInt(), 1);
    coverWidth = qBound(130, o["coverWidth"].toInt(180), 240);
    showBackground = o["showBackground"].toBool(true);
    colorMode = o["colorMode"].toString("dark");
    if(colorMode!="light"&&colorMode!="dark")colorMode="dark";
    accentColor = o["accentColor"].toString("lime");
    geometry = QByteArray::fromBase64(o["geometry"].toString().toLatin1());
    dockState = QByteArray::fromBase64(o["dockState"].toString().toLatin1());
    games.clear();
    for (const auto& value : o["games"].toArray()) {
        auto game = Game::fromJson(value.toObject());
        if (!game.id.isEmpty() && !game.title.isEmpty() && !find(game.id)) games.append(game);
    }
    return true;
}
bool Library::save(QString* error) const {
    if (!writable) return fail(error, "Library could not be loaded; refusing to overwrite it.");
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return fail(error, "Cannot create the library directory.");
    QJsonArray entries;
    for (const auto& g : games) entries.append(g.json());
    QJsonObject o{{"schema", 1}, {"games", entries}, {"toolRoot", toolRoot}, {"outputRoot", outputRoot}, {"keysPath", keysPath}, {"jobs", jobs},
        {"viewMode", viewMode}, {"coverWidth", coverWidth}, {"showBackground", showBackground},
        {"colorMode", colorMode}, {"accentColor", accentColor},
        {"geometry", QString::fromLatin1(geometry.toBase64())}, {"dockState", QString::fromLatin1(dockState.toBase64())}};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return fail(error, file.errorString());
    const auto bytes = QJsonDocument(o).toJson();
    if (file.write(bytes) != bytes.size() || !file.commit()) return fail(error, file.errorString());
    return true;
}
Game* Library::find(const QString& id) { for (auto& g : games) if (g.id == id) return &g; return nullptr; }
const Game* Library::find(const QString& id) const { for (const auto& g : games) if (g.id == id) return &g; return nullptr; }
bool Library::add(Game g) {
    for (const auto& existing : games) {
        if ((!g.source.isEmpty() && samePath(existing.source, g.source)) || samePath(existing.executable(), g.executable())) return false;
    }
    games.append(std::move(g)); return true;
}

QString outputNameFor(const QString& title) {
    QString result;
    for (auto ch : title) if (ch.isLetterOrNumber() || QString("-_ ").contains(ch)) result += ch;
    return result.trimmed().isEmpty() ? "Game" : result.trimmed();
}
bool samePath(const QString& left,const QString& right){
    if(left.isEmpty()||right.isEmpty())return left==right;
    const auto normalized=[](const QString& path){return QDir::cleanPath(QFileInfo(QDir::fromNativeSeparators(path)).absoluteFilePath());};
#ifdef Q_OS_WIN
    return normalized(left).compare(normalized(right),Qt::CaseInsensitive)==0;
#else
    return normalized(left)==normalized(right);
#endif
}
QString titleForSource(const QString& source) {
    QFileInfo info(source);
    QString name = info.isDir() ? info.fileName() : info.completeBaseName();
    name.remove(QRegularExpression("\\s*\\[[^\\]]*\\]"));
    return name.trimmed().isEmpty() ? "Untitled game" : name.trimmed();
}
bool validSource(const QString& source, QString* error) {
    QFileInfo info(source);
    if (info.isDir()) {
        QDir d(source);
        for (const auto& name : {"main", "exefs/main", "ExeFS/main"}) if (QFileInfo(d.filePath(name)).isFile()) return true;
        return fail(error, "This folder does not contain exefs/main or main.");
    }
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, file.errorString());
    const auto header = file.read(0x14);
    if (info.suffix().compare("nup", Qt::CaseInsensitive) == 0 && header.startsWith(QByteArray("NUP\1", 4))) return true;
    if (info.suffix().compare("nsp", Qt::CaseInsensitive) == 0 && header.startsWith("PFS0")) return true;
    if (info.suffix().compare("nro", Qt::CaseInsensitive) == 0 && header.mid(0x10, 4) == "NRO0") return true;
    return fail(error, "Select an indexed NUP, a standard application NSP, an NRO, or an extracted ExeFS folder.");
}
Game gameFromSource(const QString& source, const QString& outputRoot) {
    Game g;
    g.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    g.source = absolute(source); g.title = titleForSource(source); g.outputName = outputNameFor(g.title);
    const QString base = outputRoot.isEmpty() ? QDir(QFileInfo(source).absolutePath()).filePath("SwitchRecomp Builds") : outputRoot;
    g.buildDir = QDir(base).filePath(g.outputName);
    return g;
}
bool gameFromExecutable(const QString& path, Game* game, QString* error) {
    if (!QFileInfo(path).isFile()) return fail(error, "The executable does not exist.");
    Game g;
    g.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    g.title = QFileInfo(path).fileName();
    if (QString(executableSuffix).size() && g.title.endsWith(executableSuffix, Qt::CaseInsensitive)) g.title.chop(QString(executableSuffix).size());
    g.outputName = g.title; g.buildDir = QFileInfo(path).absolutePath();
    if (!g.hasBuild()) return fail(error, "A native build needs its executable, matching code module, and _data folder together.");
    QFile marker(QDir(g.dataDir()).filePath("romfs.nup"));
    if (marker.open(QIODevice::ReadOnly)) g.source = QString::fromUtf8(marker.readLine()).trimmed();
    if (g.source.isEmpty()) {
        QFile nspMarker(QDir(g.dataDir()).filePath("romfs.nsp"));
        if (nspMarker.open(QIODevice::ReadOnly) && nspMarker.readLine().trimmed() == "SWRNSP1")
            g.source = QString::fromUtf8(nspMarker.readLine()).trimmed();
    }
    g.builtSource = g.source; g.renderer = readRenderer(g.iniPath());
    for (const auto& name : {"icon.jpg", "icon.png", "icon_AmericanEnglish.dat"}) {
        const QString icon = QDir(g.dataDir()).filePath(name);
        if (QFileInfo(icon).isFile()) { g.cover = icon; break; }
    }
    *game = g; return true;
}
QString readRenderer(const QString& ini) {
    QFile file(ini);
    QString renderer = "opengl";
    if (!file.open(QIODevice::ReadOnly)) return renderer;
    for (const auto& line : QString::fromUtf8(file.readAll()).split('\n')) {
        const auto trimmed = line.trimmed();
        if (trimmed.startsWith("renderer=")) renderer = trimmed.mid(9).trimmed();
    }
    return renderer;
}
bool writeRenderer(const QString& ini, const QString& renderer, QString* error) {
    if (renderer != "vulkan" && renderer != "opengl") return fail(error, "Unknown renderer.");
    QFile input(ini);
    QString text;
    if (input.exists()) {
        if (!input.open(QIODevice::ReadOnly)) return fail(error, input.errorString());
        text = QString::fromUtf8(input.readAll()); input.close();
    }
    const QString newline = text.contains("\r\n") ? "\r\n" : "\n";
    text.replace("\r\n", "\n");
    auto lines = text.split('\n');
    const QRegularExpression key("^\\s*renderer\\s*=");
    bool found = false;
    int section = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].trimmed() == "[Graphics]") section = i;
        if (key.match(lines[i]).hasMatch()) { lines[i] = "renderer=" + renderer; found = true; }
    }
    if (!found) {
        if (section >= 0) lines.insert(section + 1, "renderer=" + renderer);
        else { lines << "[Graphics]" << "renderer=" + renderer << ""; }
    }
    QSaveFile output(ini);
    if (!output.open(QIODevice::WriteOnly)) return fail(error, output.errorString());
    const auto bytes = lines.join(newline).toUtf8();
    if (output.write(bytes) != bytes.size() || !output.commit()) return fail(error, output.errorString());
    return true;
}

bool LibraryFilter::matches(const Game& game, bool running, bool building) const {
    if (!game.title.contains(text.trimmed(), Qt::CaseInsensitive)) return false;
    const bool nup = inPlacePackage(game.source);
    const bool nro = game.source.endsWith(".nro", Qt::CaseInsensitive);
    if (source == 1 && !nup) return false;
    if (source == 2 && !nro) return false;
    if (source == 3 && (nup || nro || game.source.isEmpty())) return false;
    if (source == 4 && !game.source.isEmpty()) return false;
    if (state == 1 && (!game.hasBuild() || running || building || (nup && !game.sourceAvailable()))) return false;
    if (state == 2 && (game.hasBuild() || building || running)) return false;
    if (state == 3 && !running) return false;
    return true;
}
bool inPlacePackage(const QString& path) {
    return path.endsWith(".nup", Qt::CaseInsensitive) || path.endsWith(".nsp", Qt::CaseInsensitive);
}
int LibraryFilter::activeCount() const { return (!text.trimmed().isEmpty() ? 1 : 0) + (state != 0 ? 1 : 0) + (source != 0 ? 1 : 0); }

CardState cardState(const Game& game, bool running, bool building, bool otherBuild, bool toolsReady) {
    if(building)return {"Cancel build","Building",true,false,true,false};
    if(running)return {"Stop","Running",true,false,false,true};
    if(game.hasBuild()){
        if(inPlacePackage(game.source)&&!game.sourceAvailable())
            return {"Play","Source offline",false,true,false,false};
        return {"Play","Ready to play",true,false,false,false};
    }
    return {"Build",game.buildIncomplete?"Build incomplete":"Needs build",
        !otherBuild&&toolsReady&&game.sourceAvailable(),true,false,false};
}
