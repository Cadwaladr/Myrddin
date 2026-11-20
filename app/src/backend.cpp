#include "backend.h"

#include "panel_key.h"   // (fg/: the keys the layer knows for its panel)

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLibrary>
#include <QMimeData>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#include <cstdio>
#include <functional>

#include <unistd.h>

namespace {

const char* kGame = "StarCitizen.exe";
const char* kAppId = "io.github.cadwaladr.Myrddin";

QString home() { return QDir::homePath(); }

QString xdgDir(const char* variable, const QString& underHome) {
    const QString set = qEnvironmentVariable(variable);
    return set.isEmpty() ? home() + underHome : set;
}

QString readAll(const QString& path, qint64 lastBytes = -1) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    if (lastBytes > 0 && file.size() > lastBytes) file.seek(file.size() - lastBytes);
    return QString::fromUtf8(file.readAll());
}

// A value as bash takes it in single quotes.
QString shellQuoted(const QString& text) {
    QString quoted = text;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

// What a line NAME=value of sc-turbo.conf stands for: quotes off, $HOME and $SC_PREFIX put in.
QString confValue(QString raw, const QString& prefix) {
    raw = raw.trimmed();
    const bool single = raw.size() >= 2 && raw.startsWith(QLatin1Char('\'')) && raw.endsWith(QLatin1Char('\''));
    const bool dbl = raw.size() >= 2 && raw.startsWith(QLatin1Char('"')) && raw.endsWith(QLatin1Char('"'));
    if (single || dbl) raw = raw.mid(1, raw.size() - 2);
    if (single) {
        raw.replace(QStringLiteral("'\\''"), QStringLiteral("'"));
        return raw;
    }
    raw.replace(QStringLiteral("${HOME}"), home()).replace(QStringLiteral("$HOME"), home());
    raw.replace(QStringLiteral("${SC_PREFIX}"), prefix).replace(QStringLiteral("$SC_PREFIX"), prefix);
    if (raw.startsWith(QStringLiteral("~/"))) raw = home() + raw.mid(1);
    return raw;
}

QVariantMap check(const QString& key, const QString& title, const QString& detail, const QString& level,
                  const QString& fix = {}, const QString& page = {}) {
    return {{QStringLiteral("key"), key},     {QStringLiteral("title"), title}, {QStringLiteral("detail"), detail},
            {QStringLiteral("level"), level}, {QStringLiteral("fix"), fix},     {QStringLiteral("page"), page}};
}

const QRegularExpression& statLine() {
    static const QRegularExpression re(QStringLiteral(
        R"(^\s*([\d.]+) STAT x(\d+) real=([\d.]+)/s generated=([\d.]+)/s shown=([\d.]+)/s.*?game frame ms mean=([\d.]+) p50=([\d.]+) p95=([\d.]+) p99=([\d.]+))"),
        QRegularExpression::MultilineOption);
    return re;
}

QString configFile() {
    const QString dir = qEnvironmentVariable("MYRDDIN_CONFIG_DIR", xdgDir("XDG_CONFIG_HOME", QStringLiteral("/.config")) + QStringLiteral("/myrddin"));
    return dir + QStringLiteral("/myrddin.conf");
}

}   // namespace

// ---------------------------------------------------------------------------------------------
// Before any window: where the runtime is, and the copy of it this AppImage carries.

QString Backend::resolveRuntime(const QString& runtimeOverride) {
    if (!runtimeOverride.isEmpty()) return QDir(runtimeOverride).absolutePath();
    const QString fromEnv = qEnvironmentVariable("MYRDDIN_RUNTIME");
    if (!fromEnv.isEmpty()) return QDir(fromEnv).absolutePath();
    return xdgDir("XDG_DATA_HOME", QStringLiteral("/.local/share")) + QStringLiteral("/myrddin/runtime");
}

bool Backend::processUp(const char* comm) {
    const QByteArray wanted(comm);
    const QStringList entries = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& entry : entries) {
        if (entry.isEmpty() || !entry.at(0).isDigit()) continue;
        QFile file(QStringLiteral("/proc/") + entry + QStringLiteral("/comm"));
        if (file.open(QIODevice::ReadOnly) && file.readLine().trimmed() == wanted) return true;
    }
    return false;
}

// The RSI Launcher renames its processes once it runs (the main one is "CrBrowserMain", its page
// "CrRendererMain"), so its name is no sign of it: a window opened while the launcher was already open
// saw none, and offered no Stop. It is found the way sc-turbo's "stop" and "kill" find a session
// (prefix_processes): this user's processes whose program is one of Wine's, here those whose command
// line names the launcher, and, for a prefix, only those whose environment names that prefix: what
// Stop and Kill reach. Only what a process was started with is read.
bool Backend::launcherOpen(const QString& prefix) {
    // (a folder as sc-turbo's "realpath -m" gives it: links followed where it exists, tidied where not)
    const auto real = [](const QString& path) {
        const QString canonical = QFileInfo(path).canonicalFilePath();
        return canonical.isEmpty() ? QDir::cleanPath(QDir(path).absolutePath()) : canonical;
    };
    const QString want = prefix.isEmpty() ? QString() : real(prefix);
    const uint me = ::getuid();
    const QStringList entries = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& entry : entries) {
        if (entry.isEmpty() || !entry.at(0).isDigit()) continue;
        const QString dir = QStringLiteral("/proc/") + entry;
        if (QFileInfo(dir).ownerId() != me) continue;
        QString exe = QFile::symLinkTarget(dir + QStringLiteral("/exe"));
        if (exe.endsWith(QStringLiteral(" (deleted)"))) exe.chop(10);
        if (!QFileInfo(exe).fileName().startsWith(QStringLiteral("wine"))) continue;
        QFile cmdline(dir + QStringLiteral("/cmdline"));
        if (!cmdline.open(QIODevice::ReadOnly) || !cmdline.readAll().replace('\0', ' ').toLower().contains("rsi launcher.exe")) continue;
        if (want.isEmpty()) return true;
        QFile environment(dir + QStringLiteral("/environ"));
        if (!environment.open(QIODevice::ReadOnly)) continue;
        for (const QByteArray& variable : environment.readAll().split('\0'))
            if (variable.startsWith("WINEPREFIX=")) {
                if (real(QFile::decodeName(variable.mid(11))) == want) return true;
                break;
            }
    }
    return false;
}

// The AppImage carries the runtime (sc-turbo, its tools, the layer) read-only, and its mount is gone when
// the window closes, while the game may still run with the layer loaded. So the runtime is copied to a
// lasting place, and sc-turbo runs from there: it finds everything beside itself and writes its logs there.
// Kept across updates: logs/ and sc-turbo.conf. Not while the game is up (it has the layer's file open).
QString Backend::installPayload(const QString& runtime, bool gameUp) {
    const QString payload = qEnvironmentVariable("MYRDDIN_PAYLOAD");
    if (payload.isEmpty() || !QFile::exists(payload + QStringLiteral("/sc-turbo"))) return {};
    // a runtime that was named (a source checkout, a test's stand-in) is somebody's own: never written over
    if (!qEnvironmentVariableIsEmpty("MYRDDIN_RUNTIME")) return {};
    const QString wanted = readAll(payload + QStringLiteral("/VERSION")).trimmed();
    const QString have = readAll(runtime + QStringLiteral("/VERSION")).trimmed();
    if (!wanted.isEmpty() && wanted == have && QFile::exists(runtime + QStringLiteral("/sc-turbo"))) return {};
    if (gameUp && QFile::exists(runtime + QStringLiteral("/sc-turbo")))
        return QStringLiteral("A newer version is ready. It is put in place once the game and the RSI Launcher are closed.");
    if (!QDir().mkpath(runtime)) return QStringLiteral("Cannot create %1").arg(runtime);

    QStringList files;
    QDirIterator it(payload, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString source = it.next();
        const QString relative = QDir(payload).relativeFilePath(source);
        if (relative == QStringLiteral("sc-turbo.conf") || relative.startsWith(QStringLiteral("logs/"))) continue;   // the user's own
        const QString target = runtime + QLatin1Char('/') + relative;
        const QString temp = target + QStringLiteral(".new");
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile::remove(temp);
        if (!QFile::copy(source, temp)) return QStringLiteral("Cannot write %1").arg(target);
        QFile::setPermissions(temp, QFile::permissions(source) | QFileDevice::WriteOwner | QFileDevice::ReadOwner);
        if (std::rename(QFile::encodeName(temp).constData(), QFile::encodeName(target).constData()) != 0)
            return QStringLiteral("Cannot replace %1").arg(target);
        files << relative;
    }
    // what the version before had and this one has not
    const QSet<QString> now(files.begin(), files.end());
    const QStringList before = readAll(runtime + QStringLiteral("/MANIFEST")).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& old : before)
        if (!now.contains(old) && !old.startsWith(QStringLiteral("logs/")) && old != QStringLiteral("sc-turbo.conf") && !old.contains(QStringLiteral("..")))
            QFile::remove(runtime + QLatin1Char('/') + old);
    QSaveFile manifest(runtime + QStringLiteral("/MANIFEST"));
    if (manifest.open(QIODevice::WriteOnly)) {
        manifest.write(files.join(QLatin1Char('\n')).toUtf8() + '\n');
        manifest.commit();
    }
    return {};
}

// What a program started from here gets as its environment: the user's own, without what an AppImage
// adds for itself. The game runs under Wine with the system's libraries; nothing of this window's may
// reach it.
QProcessEnvironment Backend::childEnvironment() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString appDir = env.value(QStringLiteral("APPDIR"));
    for (const char* name : {"APPDIR", "APPIMAGE", "ARGV0", "OWD", "APPIMAGE_EXTRACT_AND_RUN", "MYRDDIN_PAYLOAD",
                             "MYRDDIN_RUNTIME", "MYRDDIN_FAKE", "MYRDDIN_CONFIG_DIR", "MYRDDIN_BIN_DIR"})
        env.remove(QString::fromLatin1(name));
    if (!appDir.isEmpty()) {
        for (const char* name : {"LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QML_IMPORT_PATH", "QML2_IMPORT_PATH",
                                 "QT_QPA_PLATFORM_PLUGIN_PATH", "XDG_DATA_DIRS", "PATH", "GSETTINGS_SCHEMA_DIR"}) {
            const QString key = QString::fromLatin1(name);
            if (!env.contains(key)) continue;
            QStringList kept;
            for (const QString& part : env.value(key).split(QLatin1Char(':'), Qt::SkipEmptyParts))
                if (!part.startsWith(appDir)) kept << part;
            if (kept.isEmpty()) env.remove(key);
            else env.insert(key, kept.join(QLatin1Char(':')));
        }
    }
    // (a window drawn nowhere, for pictures and tests, must not make its children draw nowhere too)
    if (env.value(QStringLiteral("QT_QPA_PLATFORM")) == QStringLiteral("offscreen")) env.remove(QStringLiteral("QT_QPA_PLATFORM"));
    env.remove(QStringLiteral("QT_QUICK_BACKEND"));
    return env;
}

// ---------------------------------------------------------------------------------------------

Backend::Backend(const QString& runtimeOverride, QObject* parent) : QObject(parent), m_runtime(resolveRuntime(runtimeOverride)) {
    m_fake = qEnvironmentVariable("MYRDDIN_FAKE");
    if (runtimeOverride.isEmpty()) m_runtimeNote = installPayload(m_runtime, processUp(kGame) || launcherOpen({}));

    static const QStringList known = {QStringLiteral("lavd"),  QStringLiteral("bpfland"), QStringLiteral("flash"), QStringLiteral("cosmos"),
                                      QStringLiteral("rusty"), QStringLiteral("p2dq"),    QStringLiteral("cake"),  QStringLiteral("beerland")};
    for (const QString& name : known)
        if (!QStandardPaths::findExecutable(QStringLiteral("scx_") + name).isEmpty()) m_schedulers << name;

    loadConf();
    loadPrefs();
    // One thing is drawn over the game, or none. Settings that have both: MangoHud, which counts the shown frames.
    if (m_conf.value(QStringLiteral("mangohud")).toBool() && m_prefs.value(QStringLiteral("panel")).toBool()) {
        m_prefs.insert(QStringLiteral("panel"), false);
        QSettings(configFile(), QSettings::IniFormat).setValue(QStringLiteral("panel"), false);
    }
    connect(&m_conf, &QQmlPropertyMap::valueChanged, this, [this](const QString&, const QVariant&) { m_saveLater.start(); });
    connect(&m_prefs, &QQmlPropertyMap::valueChanged, this, [this](const QString& key, const QVariant& value) {
        QSettings(configFile(), QSettings::IniFormat).setValue(key, value);
        emit commandChanged();
    });
    m_saveLater.setSingleShot(true);
    m_saveLater.setInterval(300);
    connect(&m_saveLater, &QTimer::timeout, this, [this] {
        saveConf();
        emit commandChanged();
        refreshChecks();
    });

    // first start: find the game where the LUG Helper put it
    if (m_conf.value(QStringLiteral("prefix")).toString().isEmpty()) {
        const QString found = detectPrefix();
        if (!found.isEmpty()) {
            m_conf.insert(QStringLiteral("prefix"), found);
            saveConf();
        }
    }

    {
        QSettings stored(configFile(), QSettings::IniFormat);
        m_logPending = stored.value(QStringLiteral("logPending"), false).toBool();
        m_logSince = stored.value(QStringLiteral("logSince"), 0).toLongLong();
        m_sessionLog = stored.value(QStringLiteral("logOffer")).toString();
        if (!m_sessionLog.isEmpty() && !QFile::exists(m_sessionLog)) m_sessionLog.clear();
    }

    connect(&m_poll, &QTimer::timeout, this, &Backend::poll);
    m_poll.start(1500);
    poll();
    refreshChecks();
}

QString Backend::version() const { return QStringLiteral(MYRDDIN_VERSION); }

QString Backend::runtimeVersion() const {
    const QString text = readAll(m_runtime + QStringLiteral("/VERSION")).trimmed();
    return text.isEmpty() ? QStringLiteral("from the source tree") : text.section(QLatin1Char('\n'), 0, 0);
}

bool Backend::fromAppImage() const { return !qEnvironmentVariable("APPIMAGE").isEmpty(); }

// The licence texts of everything an AppImage carries lie inside it (appimage/packaging/licenses.py).
QString Backend::licenceFolder() const {
    const QString dir = qEnvironmentVariable("APPDIR") + QStringLiteral("/usr/share/licenses/myrddin");
    return qEnvironmentVariableIsEmpty("APPDIR") || !QDir(dir).exists() ? QString() : dir;
}

QString Backend::confPath() const { return m_runtime + QStringLiteral("/sc-turbo.conf"); }

QString Backend::selfPath() const {
    const QString image = qEnvironmentVariable("APPIMAGE");
    return image.isEmpty() ? QCoreApplication::applicationFilePath() : image;
}

// ---------------------------------------------------------------------------------------------
// sc-turbo.conf: read by sc-turbo itself, so the window and the command line agree.

void Backend::loadConf() {
    QString prefix, launch, monitor, target, sched;
    bool mangohud = false, markers = true, keepHud = true, gapfill = true, pictures = true, boost = false, performance = true;
    QString panelKey = QStringLiteral("Shift+7");
    m_confKept.clear();
    static const QRegularExpression assignment(QStringLiteral(R"(^(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)=(.*)$)"));
    const QStringList lines = readAll(confPath()).split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QStringLiteral("# Written by Myrddin")) || trimmed.startsWith(QStringLiteral("# (lines the window does not know"))) continue;
        const auto match = assignment.match(trimmed);
        const QString name = match.hasMatch() ? match.captured(1) : QString();
        const QString value = match.hasMatch() ? match.captured(2) : QString();
        if (name == QStringLiteral("SC_PREFIX")) prefix = confValue(value, {});
        else if (name == QStringLiteral("SC_LAUNCH")) launch = confValue(value, prefix);
        else if (name == QStringLiteral("SC_MONITOR")) monitor = confValue(value, prefix);
        else if (name == QStringLiteral("SC_FG_TARGET")) target = confValue(value, prefix);
        else if (name == QStringLiteral("SC_MANGOHUD")) mangohud = confValue(value, prefix) == QStringLiteral("1");
        else if (name == QStringLiteral("SC_SCHED")) sched = confValue(value, prefix);
        else if (name == QStringLiteral("SC_FG_MARKERS")) markers = confValue(value, prefix) != QStringLiteral("0");
        else if (name == QStringLiteral("SC_FG_KEEP_HUD")) keepHud = confValue(value, prefix) != QStringLiteral("0");
        else if (name == QStringLiteral("SC_FG_GAPFILL")) gapfill = confValue(value, prefix) != QStringLiteral("0");
        else if (name == QStringLiteral("SC_FG_PICTURES")) pictures = confValue(value, prefix) != QStringLiteral("0");
        else if (name == QStringLiteral("SC_FG_BOOST")) boost = confValue(value, prefix) == QStringLiteral("1");
        else if (name == QStringLiteral("SC_FG_FILL")) performance = confValue(value, prefix) != QStringLiteral("1");
        else if (name == QStringLiteral("SC_FG_PANEL_KEY")) {
            panel_key::Combo combo;   // (one the panel does not know: the default stays)
            if (panel_key::Parse(confValue(value, prefix).toUtf8().constData(), combo)) panelKey = QString::fromUtf8(combo.text);
        }
        else m_confKept << line;
    }
    if (sched == QStringLiteral("stock")) sched.clear();
    m_conf.insert(QStringLiteral("prefix"), prefix);
    m_conf.insert(QStringLiteral("launchScript"), launch);
    m_conf.insert(QStringLiteral("monitor"), monitor);
    m_conf.insert(QStringLiteral("targetRate"), target);
    m_conf.insert(QStringLiteral("mangohud"), mangohud);
    m_conf.insert(QStringLiteral("scheduler"), sched);
    m_conf.insert(QStringLiteral("markers"), markers);
    m_conf.insert(QStringLiteral("keepHud"), keepHud);
    m_conf.insert(QStringLiteral("gapfill"), gapfill);
    m_conf.insert(QStringLiteral("pictures"), pictures);
    m_conf.insert(QStringLiteral("boost"), boost);
    m_conf.insert(QStringLiteral("performance"), performance);
    m_conf.insert(QStringLiteral("panelKey"), panelKey);
}

void Backend::saveConf() {
    if (!QDir(m_runtime).exists()) return;
    QStringList out;
    out << QStringLiteral("# Written by Myrddin's window; sc-turbo reads it at every launch (sc-turbo.conf.example explains the names).");
    const auto text = [this](const char* key) { return m_conf.value(QString::fromLatin1(key)).toString().trimmed(); };
    if (!text("prefix").isEmpty()) out << QStringLiteral("SC_PREFIX=") + shellQuoted(text("prefix"));
    if (!text("launchScript").isEmpty()) out << QStringLiteral("SC_LAUNCH=") + shellQuoted(text("launchScript"));
    if (!text("monitor").isEmpty()) out << QStringLiteral("SC_MONITOR=") + shellQuoted(text("monitor"));
    static const QRegularExpression number(QStringLiteral(R"(^\d{2,3}(\.\d+)?$)"));
    if (number.match(text("targetRate")).hasMatch()) out << QStringLiteral("SC_FG_TARGET=") + text("targetRate");
    if (m_conf.value(QStringLiteral("mangohud")).toBool()) out << QStringLiteral("SC_MANGOHUD=1");
    static const QRegularExpression word(QStringLiteral(R"(^[a-z0-9_]+(:[a-z0-9_]+)?$)"));
    if (word.match(text("scheduler")).hasMatch()) out << QStringLiteral("SC_SCHED=") + text("scheduler");
    if (!m_conf.value(QStringLiteral("markers")).toBool()) out << QStringLiteral("SC_FG_MARKERS=0");
    if (!m_conf.value(QStringLiteral("keepHud")).toBool()) out << QStringLiteral("SC_FG_KEEP_HUD=0");
    // ("Performance" off: the game's frames are limited so that the display is filled evenly)
    if (!m_conf.value(QStringLiteral("performance")).toBool()) out << QStringLiteral("SC_FG_FILL=1");
    if (text("panelKey") != QStringLiteral("Shift+7") && !text("panelKey").isEmpty()) out << QStringLiteral("SC_FG_PANEL_KEY=") + shellQuoted(text("panelKey"));
    // (the layer reads these from the environment itself, so they are exported from the settings file)
    if (!m_conf.value(QStringLiteral("gapfill")).toBool()) out << QStringLiteral("export SC_FG_GAPFILL=0");
    if (!m_conf.value(QStringLiteral("pictures")).toBool()) out << QStringLiteral("export SC_FG_PICTURES=0");
    if (m_conf.value(QStringLiteral("boost")).toBool()) out << QStringLiteral("export SC_FG_BOOST=1");
    if (!m_confKept.isEmpty()) {
        out << QStringLiteral("# (lines the window does not know, kept as they were)");
        out << m_confKept;
    }
    QSaveFile file(confPath());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(out.join(QLatin1Char('\n')).toUtf8() + '\n');
        file.commit();
    }
}

void Backend::loadPrefs() {
    QSettings stored(configFile(), QSettings::IniFormat);
    const auto take = [&](const char* key, const QVariant& fallback) {
        m_prefs.insert(QString::fromLatin1(key), stored.value(QString::fromLatin1(key), fallback));
    };
    // (An ini file gives everything back as text. A switch goes into the map as a truth value at
    // once: put in as the text "false" first, the map keeps that text when the truth value false is
    // put in after it, taking the two for equal, and to the pages the text "false" counts as on.)
    const auto flag = [&](const char* key) { m_prefs.insert(QString::fromLatin1(key), stored.value(QString::fromLatin1(key), false).toBool()); };
    take("mode", QString());
    take("limit", 0);
    flag("panel");
    take("panelCorner", QStringLiteral("tl"));
    flag("watch");
    flag("closeAfterLaunch");
    flag("welcomed");
    flag("lastLaunchLayer");
    // What is chosen on Play: off, a multiplier that is held (2, 3, 4), or auto: the layer picks the
    // multiplier as the game's own rate changes, up to the limit. Settings from before "mode" had one
    // number, which was off (1) or that limit: they become off, or auto with that limit.
    static const QStringList modes = {QStringLiteral("off"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("4"), QStringLiteral("auto")};
    const int before = stored.value(QStringLiteral("multiplier"), 0).toInt();
    QString mode = m_prefs.value(QStringLiteral("mode")).toString();
    if (!modes.contains(mode)) mode = before == 1 ? QStringLiteral("off") : QStringLiteral("auto");
    m_prefs.insert(QStringLiteral("mode"), mode);
    int limit = m_prefs.value(QStringLiteral("limit")).toInt();   // (an ini file gives everything back as text)
    if (limit < 2) limit = before > 1 ? before : 4;
    m_prefs.clear(QStringLiteral("limit"));   // (so that the number takes the text's place, see above)
    m_prefs.insert(QStringLiteral("limit"), qBound(2, limit, 4));
}

QString Backend::launchScriptInUse() const {
    const QString own = m_conf.value(QStringLiteral("launchScript")).toString().trimmed();
    if (!own.isEmpty()) return own;
    const QString prefix = m_conf.value(QStringLiteral("prefix")).toString().trimmed();
    return (prefix.isEmpty() ? home() + QStringLiteral("/Games/star-citizen") : prefix) + QStringLiteral("/sc-launch.sh");
}

QStringList Backend::launchWords() const {
    const QString mode = m_prefs.value(QStringLiteral("mode")).toString();
    const bool logs = m_prefs.value(QStringLiteral("watch")).toBool();   // "Run with logs"
    if (mode == QStringLiteral("off")) return logs ? QStringList{QStringLiteral("run"), QStringLiteral("watch")} : QStringList{QStringLiteral("run")};
    // auto: "fg N", the layer picks up to N; a multiplier of one's own: "fg N fixed", held there
    const bool held = mode != QStringLiteral("auto");
    QStringList words{QStringLiteral("fg"), held ? mode : QString::number(m_prefs.value(QStringLiteral("limit")).toInt())};
    if (held) words << QStringLiteral("fixed");
    if (m_prefs.value(QStringLiteral("panel")).toBool()) words << QStringLiteral("panel");
    if (logs) words << QStringLiteral("watch");
    return words;
}

QString Backend::command() const { return QStringLiteral("myrddin ") + launchWords().join(QLatin1Char(' ')); }

// ---------------------------------------------------------------------------------------------
// Running sc-turbo.

bool Backend::scTurbo(const QStringList& words, QString* output, int timeoutMs) const {
    QProcess process;
    process.setProgram(m_runtime + QStringLiteral("/sc-turbo"));
    process.setArguments(words);
    process.setWorkingDirectory(m_runtime);
    process.setProcessEnvironment(childEnvironment());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(500);
        if (output) *output = QStringLiteral("sc-turbo did not answer");
        return false;
    }
    if (output) *output = QString::fromUtf8(process.readAll()).trimmed();
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

void Backend::startAsync(const QString& program, const QStringList& words, std::function<void(int, const QString&)> done) {
    auto* process = new QProcess(this);
    process->setProgram(program);
    process->setArguments(words);
    process->setWorkingDirectory(QDir(m_runtime).exists() ? m_runtime : home());
    process->setProcessEnvironment(childEnvironment());
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto finished = std::make_shared<bool>(false);
    connect(process, &QProcess::finished, this, [process, done, finished](int code, QProcess::ExitStatus status) {
        if (*finished) return;
        *finished = true;
        done(status == QProcess::NormalExit ? code : -1, QString::fromUtf8(process->readAll()).trimmed());
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [process, done, finished](QProcess::ProcessError error) {
        if (*finished || error != QProcess::FailedToStart) return;
        *finished = true;
        done(-1, {});
        process->deleteLater();
    });
    QTimer::singleShot(15000, process, [process] { process->kill(); });
    process->start();
}

void Backend::launch() {
    if (m_state != QStringLiteral("idle")) {
        emit said(m_state == QStringLiteral("starting") ? QStringLiteral("Already starting") : QStringLiteral("The RSI Launcher is already open: start the game from it"));
        return;
    }
    if (!ready()) {
        emit said(m_blocker);
        return;
    }
    saveConf();
    const QStringList words = launchWords();
    const QString log = m_runtime + QStringLiteral("/logs/launch.out");
    QDir().mkpath(m_runtime + QStringLiteral("/logs"));
    QProcess process;
    process.setProgram(QStringLiteral("/bin/sh"));
    // (sc-turbo's own words go to a file the window shows; sc-turbo replaces itself by the launch script.
    // The file's name is handed over as a word, not in the environment: the game inherits that.)
    process.setArguments(QStringList{QStringLiteral("-c"), QStringLiteral("log=$1; shift; exec \"$@\" > \"$log\" 2>&1"), QStringLiteral("sh"), log,
                                     m_runtime + QStringLiteral("/sc-turbo")} + words);
    process.setWorkingDirectory(m_runtime);
    process.setProcessEnvironment(childEnvironment());
    if (!process.startDetached()) {
        emit said(QStringLiteral("Could not start sc-turbo"));
        return;
    }
    m_startedAt = QDateTime::currentMSecsSinceEpoch();
    m_launchLogSize = -1;
    m_prefs.insert(QStringLiteral("lastLaunchLayer"), words.first() == QStringLiteral("fg"));
    m_logPending = words.contains(QStringLiteral("watch"));
    m_logSince = m_logPending ? m_startedAt : 0;
    {
        QSettings stored(configFile(), QSettings::IniFormat);
        stored.setValue(QStringLiteral("lastLaunchLayer"), words.first() == QStringLiteral("fg"));
        stored.setValue(QStringLiteral("logPending"), m_logPending);
        stored.setValue(QStringLiteral("logSince"), m_logSince);
        stored.setValue(QStringLiteral("logCommand"), QStringLiteral("myrddin ") + words.join(QLatin1Char(' ')));
    }
    poll();
    if (m_prefs.value(QStringLiteral("closeAfterLaunch")).toBool()) QTimer::singleShot(2500, qApp, &QCoreApplication::quit);
}

bool Backend::steering() const { return (m_state == QStringLiteral("running") || m_state == QStringLiteral("launcher")) && m_layerSession; }

void Backend::setMode(const QString& mode) {
    static const QStringList modes = {QStringLiteral("off"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("4"), QStringLiteral("auto")};
    if (!modes.contains(mode)) return;
    m_prefs.insert(QStringLiteral("mode"), mode);
    QSettings(configFile(), QSettings::IniFormat).setValue(QStringLiteral("mode"), mode);
    emit commandChanged();
    if (m_state != QStringLiteral("running") && m_state != QStringLiteral("launcher")) return;
    if (!m_layerSession) {
        if (mode != QStringLiteral("off")) emit said(QStringLiteral("This session was not started with the layer from here: generated frames come with the next launch"));
        return;
    }
    QString answer;
    if (mode == QStringLiteral("off")) {
        scTurbo({QStringLiteral("fg-off")}, &answer);
    } else {
        scTurbo({QStringLiteral("fg-on")});
        if (mode == QStringLiteral("auto")) scTurbo({QStringLiteral("fg-mult"), QString::number(m_prefs.value(QStringLiteral("limit")).toInt()), QStringLiteral("auto")}, &answer);
        else scTurbo({QStringLiteral("fg-mult"), mode, QStringLiteral("fixed")}, &answer);
    }
    emit said(answer.section(QLatin1Char('\n'), -1));
    poll();
}

void Backend::setLimit(int n) {
    n = qBound(2, n, 4);
    m_prefs.insert(QStringLiteral("limit"), n);
    QSettings(configFile(), QSettings::IniFormat).setValue(QStringLiteral("limit"), n);
    emit commandChanged();
    // (the limit is Auto's: a multiplier that is held is not moved by it)
    if (!steering() || m_prefs.value(QStringLiteral("mode")).toString() != QStringLiteral("auto")) return;
    QString answer;
    scTurbo({QStringLiteral("fg-mult"), QString::number(n), QStringLiteral("auto")}, &answer);
    emit said(answer.section(QLatin1Char('\n'), -1));
}

// A switch of Options: written to sc-turbo.conf for the next launch, and sent to a running game.
void Backend::setOption(const QString& name, bool on) {
    static const QStringList params = {QStringLiteral("markers"), QStringLiteral("gapfill"), QStringLiteral("pictures"), QStringLiteral("boost")};
    if (name != QStringLiteral("keepHud") && name != QStringLiteral("performance") && !params.contains(name)) return;
    m_conf.insert(name, on);
    m_saveLater.start();   // (insert() from here does not say "changed", which is what a page's own change does)
    if (!steering()) return;
    QString answer;
    if (name == QStringLiteral("keepHud")) scTurbo({QStringLiteral("fg-hud"), on ? QStringLiteral("on") : QStringLiteral("off")}, &answer);
    // ("Performance" is the layer's "fill" the other way round)
    else if (name == QStringLiteral("performance")) scTurbo({QStringLiteral("fg-params"), on ? QStringLiteral("fill=0") : QStringLiteral("fill=1")}, &answer);
    else scTurbo({QStringLiteral("fg-params"), name + (on ? QStringLiteral("=1") : QStringLiteral("=0"))}, &answer);
    emit said(answer.section(QLatin1Char('\n'), 0, 0));
}

// One thing over the game's picture, or none: MangoHud (SC_MANGOHUD in sc-turbo.conf) or the layer's own panel.
void Backend::setOverlay(const QString& which) {
    static const QStringList known = {QStringLiteral("off"), QStringLiteral("mangohud"), QStringLiteral("panel")};
    if (!known.contains(which)) return;
    m_conf.insert(QStringLiteral("mangohud"), which == QStringLiteral("mangohud"));
    m_prefs.insert(QStringLiteral("panel"), which == QStringLiteral("panel"));
    QSettings(configFile(), QSettings::IniFormat).setValue(QStringLiteral("panel"), which == QStringLiteral("panel"));
    m_saveLater.start();
    emit commandChanged();
}

void Backend::panel(const QString& word) {
    static const QStringList allowed = {QStringLiteral("next"), QStringLiteral("off"), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"),
                                        QStringLiteral("tl"),   QStringLiteral("tr"),  QStringLiteral("bl"), QStringLiteral("br")};
    if (!allowed.contains(word)) return;
    QString answer;
    scTurbo({QStringLiteral("fg-panel"), word}, &answer);
    emit said(answer.section(QLatin1Char('\n'), 0, 0));
}

// A key press as the panel's key: Qt hands over the keyboard's own number for the key (on Linux the
// input code plus 8), which is what the layer goes by, whatever the keyboard layout prints.
QString Backend::keyName(int nativeScanCode, int modifiers) const {
    if (modifiers & Qt::MetaModifier) return {};   // (no Super keys: desktops keep those)
    const char* name = nativeScanCode > 8 ? panel_key::NameOf(static_cast<uint16_t>(nativeScanCode - 8)) : nullptr;
    if (!name) return {};
    return QLatin1String(modifiers & Qt::ControlModifier ? "Ctrl+" : "") + QLatin1String(modifiers & Qt::ShiftModifier ? "Shift+" : "") +
           QLatin1String(modifiers & Qt::AltModifier ? "Alt+" : "") + QString::fromUtf8(name);
}

void Backend::setPanelKey(const QString& keys) {
    panel_key::Combo combo;
    if (!panel_key::Parse(keys.toUtf8().constData(), combo)) {
        emit said(QStringLiteral("Not a key the panel can use: %1").arg(keys));
        return;
    }
    const QString tidy = QString::fromUtf8(combo.text);
    m_conf.insert(QStringLiteral("panelKey"), tidy);
    m_saveLater.start();
    QString answer;
    if (steering() && m_prefs.value(QStringLiteral("panel")).toBool()) scTurbo({QStringLiteral("fg-panel"), QStringLiteral("key=") + tidy}, &answer);
    emit said(answer.isEmpty() ? QStringLiteral("Panel key: %1").arg(tidy) : answer.section(QLatin1Char('\n'), 0, 0));
}

// Stop: the game, the RSI Launcher and what else of Wine runs in the prefix are asked to end. Should
// any of it still be there two seconds later, "Kill" is offered, which ends it without asking.
void Backend::stop() {
    if (m_stopping || (m_state != QStringLiteral("launcher") && m_state != QStringLiteral("running"))) return;
    m_stopping = true;
    m_canKill = false;
    emit stateChanged();
    startAsync(m_runtime + QStringLiteral("/sc-turbo"), {QStringLiteral("stop")}, [this](int, const QString& output) {
        if (!output.isEmpty()) emit said(output.section(QLatin1Char('\n'), -1));
        poll();
    });
    // (the wait belongs to this Stop: one that ended, followed by a launch and another Stop within
    // the two seconds, must not offer "Kill" early for the new one)
    const int round = ++m_stopRound;
    QTimer::singleShot(2000, this, [this, round] {
        if (round != m_stopRound) return;
        poll();
        if (!m_stopping || m_canKill) return;
        m_canKill = true;
        emit stateChanged();
    });
    m_poll.start(400);
}

void Backend::kill() {
    if (!m_stopping || !m_canKill) return;
    startAsync(m_runtime + QStringLiteral("/sc-turbo"), {QStringLiteral("kill")}, [this](int, const QString& output) {
        if (!output.isEmpty()) emit said(output.section(QLatin1Char('\n'), -1));
        poll();
    });
}

void Backend::showLaunchLog() {
    m_toolBusy = false;
    m_toolTitle = QStringLiteral("The last launch");
    m_toolOutput = m_launchLog.isEmpty() ? QStringLiteral("Nothing yet: what sc-turbo says when it starts the game shows up here (the display it found, the runner, anything in the way).") : m_launchLog;
    emit toolChanged();
}

void Backend::closeTool() {
    if (m_toolTitle.isEmpty()) return;
    m_toolTitle.clear();   // (one that still runs ends by itself; what it says is not shown)
    if (!m_toolBusy) m_toolOutput.clear();
    emit toolChanged();
}

void Backend::runTool(const QString& title, const QStringList& words) {
    static const QStringList allowed = {QStringLiteral("check"),   QStringLiteral("latency"), QStringLiteral("displays"), QStringLiteral("fg-stat"),
                                        QStringLiteral("fg-watch"), QStringLiteral("threads"), QStringLiteral("fg-where"), QStringLiteral("iowatch")};
    if (m_toolBusy || words.isEmpty() || !allowed.contains(words.first())) return;
    m_toolBusy = true;
    m_toolTitle = title;
    m_toolOutput.clear();
    emit toolChanged();
    startAsync(m_runtime + QStringLiteral("/sc-turbo"), words, [this](int code, const QString& output) {
        m_toolBusy = false;
        m_toolOutput = output.isEmpty() ? (code == 0 ? QStringLiteral("(nothing to report)") : QStringLiteral("(no answer)")) : output;
        emit toolChanged();
    });
}

// ---------------------------------------------------------------------------------------------
// What is going on: asked of /proc (names only) and read from the layer's log.

void Backend::poll() {
    const QString before = m_state;
    bool gameUp, launcherUp;
    if (!m_fake.isEmpty()) {
        gameUp = m_fake == QStringLiteral("running") || m_fake == QStringLiteral("plain");
        launcherUp = gameUp || m_fake == QStringLiteral("launcher");
    } else {
        gameUp = processUp(kGame);
        // (the prefix sc-turbo acts on: its default where none is set)
        const QString prefix = m_conf.value(QStringLiteral("prefix")).toString().trimmed();
        launcherUp = gameUp || launcherOpen(prefix.isEmpty() ? home() + QStringLiteral("/Games/star-citizen") : prefix);
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (gameUp) m_state = QStringLiteral("running");
    else if (launcherUp) m_state = QStringLiteral("launcher");
    else if (m_startedAt && now - m_startedAt < 90000) m_state = QStringLiteral("starting");
    else m_state = QStringLiteral("idle");
    if (m_state != QStringLiteral("starting")) m_startedAt = 0;   // (the launcher closed again is "idle", not "starting")
    const bool stopped = m_stopping && m_state == QStringLiteral("idle");
    if (stopped) m_stopping = m_canKill = false;

    readStats(gameUp);
    // With the layer: the game writes the layer's log (a line every ten seconds at least). Before the game
    // is up, what the last launch from this window asked for is all there is to go by.
    const bool layer = gameUp ? m_statsFresh : (launcherUp && m_prefs.value(QStringLiteral("lastLaunchLayer")).toBool());
    const bool generating = layer && !QFile::exists(m_runtime + QStringLiteral("/logs/fg/OFF"));
    const bool changed = before != m_state || layer != m_layerSession || generating != m_generating || stopped;
    m_layerSession = layer;
    m_generating = generating;
    if (changed) emit stateChanged();
    readLaunchLog();
    // After a launch with logs: each time the game has closed there is a log to make (also for a game that
    // closed while this window was shut). The RSI Launcher may stay open and start the game again, so the
    // launch stays "pending" until the launcher has closed too.
    if (m_logPending && !m_logBusy && (m_state == QStringLiteral("idle") || m_state == QStringLiteral("launcher"))) makeSessionLog(m_state == QStringLiteral("idle"));

    if (before != QStringLiteral("idle") && m_state == QStringLiteral("idle") && !m_runtimeNote.isEmpty()) {
        m_runtimeNote = installPayload(m_runtime, false);
        emit runtimeChanged();
        refreshChecks();
    }
    m_poll.setInterval(m_stopping ? 400 : m_state == QStringLiteral("idle") ? 2500 : 1500);
}

void Backend::readStats(bool gameUp) {
    QString newest;
    QDateTime newestTime;
    const QFileInfoList logs = QDir(m_runtime + QStringLiteral("/logs/fg")).entryInfoList({QStringLiteral("fg-*.log")}, QDir::Files);
    for (const QFileInfo& info : logs)
        if (newest.isEmpty() || info.lastModified() > newestTime) {
            newest = info.absoluteFilePath();
            newestTime = info.lastModified();
        }
    const bool fresh = gameUp && !newest.isEmpty() && (!m_fake.isEmpty() || newestTime.secsTo(QDateTime::currentDateTime()) < 30);
    bool changed = fresh != m_statsFresh;
    m_statsFresh = fresh;
    if (fresh) {
        const bool otherLog = newest != m_statsLog;
        const QString tail = readAll(newest, otherLog ? 262144 : 16384);
        auto matches = statLine().globalMatch(tail);
        QRegularExpressionMatch last;
        if (otherLog) {
            m_baseHistory.clear();
            m_shownHistory.clear();
            m_statsStamp = -1;
        }
        while (matches.hasNext()) {
            const auto match = matches.next();
            const double stamp = match.captured(1).toDouble();
            if (stamp > m_statsStamp) {
                m_baseHistory << match.captured(3).toDouble();
                m_shownHistory << match.captured(5).toDouble();
                m_statsStamp = stamp;
                last = match;
            }
        }
        while (m_baseHistory.size() > 90) {
            m_baseHistory.removeFirst();
            m_shownHistory.removeFirst();
        }
        m_statsLog = newest;
        if (last.hasMatch()) {
            m_liveMult = last.captured(2).toInt();
            m_base = last.captured(3).toDouble();
            m_shown = last.captured(5).toDouble();
            m_p50 = last.captured(7).toDouble();
            m_p99 = last.captured(9).toDouble();
            changed = true;
        }
    } else if (!gameUp && !m_statsLog.isEmpty()) {
        m_statsLog.clear();
        m_statsStamp = -1;
        m_base = m_shown = m_p50 = m_p99 = 0;
        m_liveMult = 0;
        changed = true;
    }
    if (changed) emit statsChanged();
}

void Backend::readLaunchLog() {
    const QFileInfo info(m_runtime + QStringLiteral("/logs/launch.out"));
    const qint64 size = info.exists() ? info.size() : 0;
    if (size == m_launchLogSize) return;
    m_launchLogSize = size;
    m_launchLog = readAll(info.absoluteFilePath(), 6000).trimmed();
    emit launchLogChanged();
}

// ---------------------------------------------------------------------------------------------
// "Run with logs": one text file per session, made when the game has closed, from what sc-turbo said at
// the launch, the recorder's report (the game's disk, memory and graphics memory use, read from outside)
// and the layer's frame rates. The window then offers it: read, copy, delete or keep.

QString Backend::tidyText(QString text) const {
    // (a log is made to be passed on: no home folder and no login name in it)
    text.replace(home(), QStringLiteral("~"));
    const QString login = QFileInfo(home()).fileName();
    if (login.size() > 1) text.replace(QLatin1Char('/') + login + QLatin1Char('/'), QStringLiteral("/USER/"));
    return text;
}

// LAST: the launcher has closed as well, so nothing more can come of this launch.
void Backend::makeSessionLog(bool last) {
    const auto over = [this, last] {
        m_logBusy = false;
        if (!last) return;
        m_logPending = false;
        QSettings(configFile(), QSettings::IniFormat).setValue(QStringLiteral("logPending"), false);
    };
    // the newest recording of this launch (logs/io/io-PID.csv, one per run of the game); without one the
    // game has not run, and there is no log
    QString recording;
    qint64 newest = 0;
    const QFileInfoList found = QDir(m_runtime + QStringLiteral("/logs/io")).entryInfoList({QStringLiteral("io-*.csv")}, QDir::Files);
    for (const QFileInfo& info : found) {
        const qint64 changed = info.lastModified().toMSecsSinceEpoch();
        if (changed >= m_logSince && (recording.isEmpty() || changed > newest)) {
            recording = info.absoluteFilePath();
            newest = changed;
        }
    }
    const QString mark = recording + QLatin1Char('|') + QString::number(newest);
    if (recording.isEmpty() || mark == QSettings(configFile(), QSettings::IniFormat).value(QStringLiteral("logMadeFor")).toString()) {
        over();   // (nothing recorded, or that run's log is made already)
        return;
    }
    if (QDateTime::currentMSecsSinceEpoch() - newest < 3000) return;   // (the recorder may still be writing its last lines: at the next look)
    m_logBusy = true;
    const bool layer = m_prefs.value(QStringLiteral("lastLaunchLayer")).toBool();
    const auto write = [this, over, recording, mark](const QString& report, const QString& rates) {
        QSettings stored(configFile(), QSettings::IniFormat);
        stored.setValue(QStringLiteral("logMadeFor"), mark);
        const QDateTime now = QDateTime::currentDateTime();
        QString text = QStringLiteral("Myrddin session log\nSession ended: %1\nMyrddin %2 (runtime %3)\nLaunched with: %4\n")
                           .arg(now.toString(QStringLiteral("yyyy-MM-dd HH:mm")), version(), runtimeVersion().section(QLatin1Char('\n'), 0, 0),
                                stored.value(QStringLiteral("logCommand")).toString());
        text += QStringLiteral("\n== What the launch said ==\n") + readAll(m_runtime + QStringLiteral("/logs/launch.out"), 20000).trimmed() + QLatin1Char('\n');
        text += QStringLiteral("\n== The session: disk, memory and frame rates, place by place ==\n") + (report.isEmpty() ? QStringLiteral("(no report)") : report) + QLatin1Char('\n');
        if (!rates.isEmpty()) text += QStringLiteral("\n== Frame rates, a line every ten seconds (the last hour) ==\n") + rates + QLatin1Char('\n');
        const QString dir = m_runtime + QStringLiteral("/logs/sessions");
        QDir().mkpath(dir);
        const QString path = dir + QStringLiteral("/session-") + now.toString(QStringLiteral("yyyyMMdd-HHmmss")) + QStringLiteral(".log");
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly) && file.write(tidyText(text).toUtf8()) >= 0 && file.commit()) {
            m_sessionLog = path;
            stored.setValue(QStringLiteral("logOffer"), path);
            stored.setValue(QStringLiteral("logRecording"), recording);
            emit sessionLogChanged();
        }
        over();
    };
    startAsync(m_runtime + QStringLiteral("/sc-turbo"), {QStringLiteral("iowatch"), QStringLiteral("report"), recording}, [this, write, layer](int, const QString& report) {
        if (!layer) {
            write(report, {});
            return;
        }
        startAsync(m_runtime + QStringLiteral("/sc-turbo"), {QStringLiteral("fg-watch"), QStringLiteral("--last"), QStringLiteral("360")}, [write, report](int, const QString& rates) { write(report, rates); });
    });
}

QString Backend::pickSessionLog() const {
    if (!m_sessionLog.isEmpty() && QFile::exists(m_sessionLog)) return m_sessionLog;
    const QFileInfoList kept = QDir(m_runtime + QStringLiteral("/logs/sessions")).entryInfoList({QStringLiteral("session-*.log")}, QDir::Files, QDir::Name);
    return kept.isEmpty() ? QString() : kept.last().absoluteFilePath();
}

QString Backend::sessionLogNote() const {
    if (m_sessionLog.isEmpty()) return {};
    const QFileInfo info(m_sessionLog);
    return QStringLiteral("%1, %2 kB, in %3").arg(info.fileName()).arg(qMax<qint64>(1, (info.size() + 512) / 1024)).arg(tidyPath(info.absolutePath()));
}

void Backend::readSessionLog() {
    const QString path = pickSessionLog();
    if (path.isEmpty()) {
        emit said(QStringLiteral("There is no session log yet: switch on \"Run with logs\" before a launch"));
        return;
    }
    m_toolBusy = false;
    m_toolTitle = QStringLiteral("Session log: ") + QFileInfo(path).fileName();
    m_toolOutput = readAll(path, 400000);
    emit toolChanged();
}

void Backend::copySessionLog() {
    const QString path = pickSessionLog();
    auto* clipboard = QGuiApplication::clipboard();
    if (path.isEmpty() || !clipboard) {
        emit said(QStringLiteral("There is no session log to copy"));
        return;
    }
    // as text for a message or a report, and as a file for a file manager or an attachment
    auto* data = new QMimeData;
    data->setText(readAll(path, 400000));
    data->setUrls({QUrl::fromLocalFile(path)});
    clipboard->setMimeData(data);
    emit said(QStringLiteral("Copied: paste it as text, or as a file"));
}

void Backend::deleteSessionLog() {
    const QString path = pickSessionLog();
    if (path.isEmpty()) return;
    QSettings stored(configFile(), QSettings::IniFormat);
    const bool offered = path == m_sessionLog;
    QFile::remove(path);
    if (offered) {
        // the recording the log was made from goes with it
        const QString recording = stored.value(QStringLiteral("logRecording")).toString();
        if (recording.startsWith(m_runtime + QStringLiteral("/logs/io/io-")) && recording.endsWith(QStringLiteral(".csv"))) {
            QFile::remove(recording);
            QFile::remove(recording.chopped(4) + QStringLiteral(".threads"));
        }
        stored.remove(QStringLiteral("logOffer"));
        stored.remove(QStringLiteral("logRecording"));
        m_sessionLog.clear();
        emit sessionLogChanged();
    }
    if (m_toolTitle.startsWith(QStringLiteral("Session log"))) {
        m_toolTitle.clear();
        m_toolOutput.clear();
        emit toolChanged();
    }
    emit said(QStringLiteral("The session's log is deleted"));
}

void Backend::keepSessionLog() {
    if (m_sessionLog.isEmpty()) return;
    QSettings stored(configFile(), QSettings::IniFormat);
    stored.remove(QStringLiteral("logOffer"));
    stored.remove(QStringLiteral("logRecording"));
    m_sessionLog.clear();
    emit sessionLogChanged();
    emit said(QStringLiteral("Kept in the log folder"));
}

// ---------------------------------------------------------------------------------------------
// The checks: what a launch needs, and what is known to get in the way.

QString Backend::detectPrefix() const {
    QStringList candidates;
    // the LUG Helper writes down where it installed the game
    const QString lug = readAll(xdgDir("XDG_CONFIG_HOME", QStringLiteral("/.config")) + QStringLiteral("/starcitizen-lug/winedir.conf")).trimmed();
    if (!lug.isEmpty()) candidates << lug;
    candidates << home() + QStringLiteral("/Games/star-citizen");
    // its menu entry for the RSI Launcher names the launch script
    const QDir entries(xdgDir("XDG_DATA_HOME", QStringLiteral("/.local/share")) + QStringLiteral("/applications"));
    static const QRegularExpression script(QStringLiteral(R"(^Exec=.*?["']?(/[^"'\n]*?)/sc-launch\.sh)"), QRegularExpression::MultilineOption);
    for (const QString& name : entries.entryList({QStringLiteral("*.desktop")}, QDir::Files)) {
        if (!name.contains(QStringLiteral("rsi"), Qt::CaseInsensitive) && !name.contains(QStringLiteral("star"), Qt::CaseInsensitive)) continue;
        const auto match = script.match(readAll(entries.filePath(name)));
        if (match.hasMatch()) candidates << match.captured(1);
    }
    for (const QString& dir : candidates)
        if (looksLikePrefix(dir)) return QDir(dir).absolutePath();
    return {};
}

bool Backend::looksLikePrefix(const QString& dir) const {
    const QString path = fullPath(dir);
    return !path.isEmpty() && (QFile::exists(path + QStringLiteral("/sc-launch.sh")) || QDir(path + QStringLiteral("/drive_c")).exists());
}

void Backend::refreshChecks() {
    QVariantList list;
    QString blocker;
    const auto bad = [&](const QString& text) {
        if (blocker.isEmpty()) blocker = text;
    };

    // this program's own files
    const bool scTurboThere = QFileInfo(m_runtime + QStringLiteral("/sc-turbo")).isExecutable();
    const bool layerThere = QFile::exists(m_runtime + QStringLiteral("/build/fg/libsc_fg.so")) && QFile::exists(m_runtime + QStringLiteral("/build/fg/sc_fg.json"));
    if (!scTurboThere) {
        list << check(QStringLiteral("runtime"), QStringLiteral("Myrddin's files"), QStringLiteral("sc-turbo is not in %1").arg(tidyPath(m_runtime)), QStringLiteral("bad"));
        bad(QStringLiteral("Myrddin's own files are missing"));
    } else if (!layerThere) {
        list << check(QStringLiteral("runtime"), QStringLiteral("Myrddin's files"), QStringLiteral("The frame-generation layer is not built (build/fg/libsc_fg.so). Without it the game can only be started as it is."), QStringLiteral("warn"));
    } else {
        list << check(QStringLiteral("runtime"), QStringLiteral("Myrddin's files"), QStringLiteral("%1, in %2").arg(runtimeVersion(), tidyPath(m_runtime)), QStringLiteral("ok"));
    }

    // the game
    const QString prefix = m_conf.value(QStringLiteral("prefix")).toString().trimmed();
    const QString script = launchScriptInUse();
    if (prefix.isEmpty() && !QFile::exists(script)) {
        list << check(QStringLiteral("prefix"), QStringLiteral("Game folder"), QStringLiteral("Not set. It is the Wine prefix Star Citizen is installed in, the folder with sc-launch.sh."), QStringLiteral("bad"), QStringLiteral("Find it"), QStringLiteral("setup"));
        bad(QStringLiteral("Tell Myrddin where the game is: Setup"));
    } else if (!prefix.isEmpty() && !QDir(prefix).exists()) {
        list << check(QStringLiteral("prefix"), QStringLiteral("Game folder"), QStringLiteral("%1 does not exist").arg(tidyPath(prefix)), QStringLiteral("bad"), QStringLiteral("Find it"), QStringLiteral("setup"));
        bad(QStringLiteral("The game's folder does not exist: Setup"));
    } else {
        list << check(QStringLiteral("prefix"), QStringLiteral("Game folder"), tidyPath(prefix.isEmpty() ? QFileInfo(script).absolutePath() : prefix), QStringLiteral("ok"));
    }
    const QFileInfo scriptInfo(script);
    QString runnerBin;
    if (!scriptInfo.exists()) {
        list << check(QStringLiteral("launch"), QStringLiteral("Launch script"), QStringLiteral("%1 is not there. The LUG Helper makes it when it installs the game.").arg(tidyPath(script)), QStringLiteral("bad"), {}, QStringLiteral("setup"));
        bad(QStringLiteral("The launch script is missing: Setup"));
    } else if (!scriptInfo.isExecutable()) {
        list << check(QStringLiteral("launch"), QStringLiteral("Launch script"), QStringLiteral("%1 is not executable").arg(tidyPath(script)), QStringLiteral("bad"), {}, QStringLiteral("setup"));
        bad(QStringLiteral("The launch script cannot be run: Setup"));
    } else {
        list << check(QStringLiteral("launch"), QStringLiteral("Launch script"), tidyPath(script), QStringLiteral("ok"));
        static const QRegularExpression winePath(QStringLiteral(R"re(^[ \t]*(?:export[ \t]+)?wine_path="(/[^"$]*)")re"), QRegularExpression::MultilineOption);
        const auto match = winePath.match(readAll(script));
        if (match.hasMatch()) runnerBin = match.captured(1);
    }
    if (!runnerBin.isEmpty()) {
        QString root = runnerBin;
        if (root.endsWith(QStringLiteral("/bin"))) root.chop(4);
        const QFileInfo dll(root + QStringLiteral("/lib/wine/x86_64-windows/d3d12core.dll"));
        const QString name = QFileInfo(root).fileName();
        if (!QDir(root).exists())
            list << check(QStringLiteral("runner"), QStringLiteral("Wine runner"), QStringLiteral("%1 is named in the launch script but is not there").arg(name), QStringLiteral("warn"));
        else if (dll.exists() && dll.size() < 1000000)
            list << check(QStringLiteral("runner"), QStringLiteral("Wine runner"), QStringLiteral("%1 has no vkd3d-proton, and frame generation needs it: the game will show no generated frames. The LUG's experimental runners have it.").arg(name), QStringLiteral("warn"));
        else
            list << check(QStringLiteral("runner"), QStringLiteral("Wine runner"), dll.exists() ? QStringLiteral("%1, with vkd3d-proton").arg(name) : name, QStringLiteral("ok"));
    }

    // What DLSS needs in the prefix under Wine: NVAPI (nvapi64.dll of dxvk-nvapi, which the LUG Helper
    // installs) and the NVIDIA driver's NGX libraries (which the LUG's runners link in). The same test
    // as sc-turbo's own note at a launch; nothing is put into the prefix from here.
    const QString system32 = (prefix.isEmpty() ? QFileInfo(script).absolutePath() : prefix) + QStringLiteral("/drive_c/windows/system32");
    if (QDir(system32).exists()) {
        const bool nvapi = QFileInfo::exists(system32 + QStringLiteral("/nvapi64.dll"));
        const bool ngx = QFileInfo::exists(system32 + QStringLiteral("/nvngx.dll")) && QFileInfo::exists(system32 + QStringLiteral("/_nvngx.dll"));
        const QString title = QStringLiteral("NVAPI in the prefix");
        const QString without = QStringLiteral(" the game offers no DLSS, and without DLSS there are no generated frames.");
        if (!nvapi)
            list << check(QStringLiteral("nvapi"), title, QStringLiteral("The prefix has no nvapi64.dll (dxvk-nvapi). The LUG Helper installs it. Without it") + without, QStringLiteral("warn"));
        else if (!ngx)
            list << check(QStringLiteral("nvapi"), title, QStringLiteral("nvapi64.dll is there, but NVIDIA's NGX libraries (nvngx.dll and _nvngx.dll) are not. A LUG runner links them in from the driver. Without them") + without, QStringLiteral("warn"));
        else
            list << check(QStringLiteral("nvapi"), title, QStringLiteral("nvapi64.dll (dxvk-nvapi) and NVIDIA's NGX libraries are there: the game can offer DLSS"), QStringLiteral("ok"));
    }

    // the graphics driver
    const QString nvidia = readAll(QStringLiteral("/proc/driver/nvidia/version"));
    static const QRegularExpression driverVersion(QStringLiteral(R"(Kernel Module.*?\s(\d+\.\d+(?:\.\d+)?)\s)"));
    if (nvidia.isEmpty()) {
        list << check(QStringLiteral("gpu"), QStringLiteral("Graphics driver"), QStringLiteral("No NVIDIA driver is loaded. The layer takes DLSS's inputs, which only NVIDIA's driver hands out: without it the game runs, but without generated frames."), QStringLiteral("warn"));
    } else {
        const auto match = driverVersion.match(nvidia);
        list << check(QStringLiteral("gpu"), QStringLiteral("Graphics driver"), QStringLiteral("NVIDIA %1").arg(match.hasMatch() ? match.captured(1) : QStringLiteral("driver")), QStringLiteral("ok"));
    }

    // MangoHud, when asked for
    m_mangohud = false;
    for (const QString& dir : {QStringLiteral("/usr/share/vulkan/implicit_layer.d"), QStringLiteral("/etc/vulkan/implicit_layer.d"),
                               home() + QStringLiteral("/.local/share/vulkan/implicit_layer.d")})
        if (!QDir(dir).entryList({QStringLiteral("MangoHud*.json")}, QDir::Files).isEmpty()) m_mangohud = true;
    if (m_conf.value(QStringLiteral("mangohud")).toBool() && !m_mangohud)
        list << check(QStringLiteral("mangohud"), QStringLiteral("MangoHud"), QStringLiteral("Switched on in Options, but MangoHud is not installed: the game starts without it."), QStringLiteral("warn"), {}, QStringLiteral("options"));

    // The game's own choice of upscaler, read from the settings file it keeps in its folder (the newest
    // of its channels': LIVE, PTU ...). Only read, and only the one value. "UpscalingTechnique" 2 is
    // DLSS: what the file held on 2026-11-20 while the game's own log said "DLSS initialized
    // successfully" and frames were generated. The game writes the file when a setting is changed.
    {
        const QString title = QStringLiteral("DLSS in the game");
        const QString why = QStringLiteral(" Its motion vectors and depth are what the generated frames are made from.");
        const QString games = (prefix.isEmpty() ? QFileInfo(script).absolutePath() : prefix) + QStringLiteral("/drive_c/Program Files/Roberts Space Industries/StarCitizen");
        QFileInfo newest;
        const QFileInfoList channels = QDir(games).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& channel : channels) {
            for (const char* inside : {"/user/client/0/Profiles/default/attributes.xml", "/USER/Client/0/Profiles/default/attributes.xml"}) {
                const QFileInfo file(channel.absoluteFilePath() + QLatin1String(inside));
                if (file.isFile() && (!newest.exists() || file.lastModified() > newest.lastModified())) newest = file;
            }
        }
        if (!newest.exists()) {
            list << check(QStringLiteral("dlss"), title, QStringLiteral("Not known yet: the game keeps its settings in a file that is written once it has run. Set the upscaler to DLSS there (Options > Graphics).") + why, QStringLiteral("info"));
        } else {
            static const QRegularExpression technique(QStringLiteral(R"re(<Attr\s+name="UpscalingTechnique"\s+value="([^"]*)")re"));
            const auto match = technique.match(readAll(newest.absoluteFilePath()));
            const QString channel = newest.absoluteFilePath().mid(games.size() + 1).section(QLatin1Char('/'), 0, 0);
            // (a file that names no upscaler at all says nothing either way: not known, not a fault)
            if (!match.hasMatch())
                list << check(QStringLiteral("dlss"), title, QStringLiteral("Not known: the game's settings (%1) do not name an upscaler yet. Set it to DLSS in the game (Options > Graphics > Upscaling Technique).").arg(channel) + why, QStringLiteral("info"));
            else if (match.captured(1) == QStringLiteral("2"))
                list << check(QStringLiteral("dlss"), title, QStringLiteral("The game's upscaler is set to DLSS (%1)").arg(channel), QStringLiteral("ok"));
            else
                list << check(QStringLiteral("dlss"), title, QStringLiteral("The game's upscaler is not set to DLSS (%1): no frames are generated until it is. In the game: Options > Graphics > Upscaling Technique.").arg(channel) + why, QStringLiteral("warn"));
        }
    }

    m_checks = list;
    m_blocker = blocker;
    m_asyncChecks.clear();

    // what has to be asked of other programs: answers come in later (those of an earlier round are dropped)
    m_pendingChecks = 0;
    const int round = ++m_checkRound;
    if (scTurboThere) {
        ++m_pendingChecks;
        startAsync(m_runtime + QStringLiteral("/sc-turbo"), {QStringLiteral("displays")}, [this, round](int, const QString& output) {
            if (round != m_checkRound) return;
            m_displays.clear();
            m_displayChoice.clear();
            for (const QString& line : output.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                if (line.startsWith(QStringLiteral("choice:"))) m_displayChoice = line.mid(7).trimmed();
                else if (!line.startsWith(QLatin1Char(' ')) && line.contains(QStringLiteral(" at ")))
                    m_displays << line.section(QStringLiteral("<-"), 0, 0).simplified();
            }
            if (m_displays.isEmpty())
                m_asyncChecks << check(QStringLiteral("display"), QStringLiteral("Display"), QStringLiteral("This desktop does not say which display is the main one (KDE Plasma and COSMIC do). The runner picks the largest; name one in Options if that is wrong."), QStringLiteral("info"), {}, QStringLiteral("options"));
            else
                m_asyncChecks << check(QStringLiteral("display"), QStringLiteral("Display"), m_displayChoice.isEmpty() ? m_displays.first() : m_displayChoice, QStringLiteral("ok"));
            finishChecks();
        });
        ++m_pendingChecks;
        startAsync(m_runtime + QStringLiteral("/tools/sc-sched"), {QStringLiteral("allowed")}, [this, round](int code, const QString&) {
            if (round != m_checkRound) return;
            m_schedAllowed = code == 0;
            const QString wanted = m_conf.value(QStringLiteral("scheduler")).toString();
            if (!wanted.isEmpty() && !m_schedAllowed)
                m_asyncChecks << check(QStringLiteral("sched"), QStringLiteral("CPU scheduler"), QStringLiteral("scx_%1 is chosen in Options, but switching schedulers is not set up here (it needs scx-scheds, scx-tools and your own permission to switch without a password: docs/optional.md says how). The game runs with the stock scheduler.").arg(wanted), QStringLiteral("warn"), {}, QStringLiteral("options"));
            finishChecks();
        });
    }
    if (qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains(QStringLiteral("KDE")) && !QStandardPaths::findExecutable(QStringLiteral("kreadconfig6")).isEmpty()) {
        ++m_pendingChecks;
        startAsync(QStringLiteral("kreadconfig6"), {QStringLiteral("--file"), QStringLiteral("kwinrc"), QStringLiteral("--group"), QStringLiteral("Compositing"), QStringLiteral("--key"), QStringLiteral("AllowTearing"), QStringLiteral("--default"), QStringLiteral("true")},
                   [this, round](int, const QString& output) {
                       if (round != m_checkRound) return;
                       if (output.trimmed() != QStringLiteral("false"))
                           m_asyncChecks << check(QStringLiteral("kde-tearing"), QStringLiteral("Screen tearing (KDE Plasma)"), QStringLiteral("KWin lets fullscreen windows tear. If the picture tears in the game, switch that off."), QStringLiteral("info"), QStringLiteral("Switch off"));
                       finishChecks();
                   });
        if (processUp("plasma-keyboard")) {
            ++m_pendingChecks;
            startAsync(QStringLiteral("kreadconfig6"), {QStringLiteral("--file"), QStringLiteral("plasmakeyboardrc"), QStringLiteral("--group"), QStringLiteral("General"), QStringLiteral("--key"), QStringLiteral("diacriticsPopupEnabled"), QStringLiteral("--default"), QStringLiteral("true")},
                       [this, round](int, const QString& output) {
                           if (round != m_checkRound) return;
                           if (output.trimmed() != QStringLiteral("false"))
                               m_asyncChecks << check(QStringLiteral("kde-diacritics"), QStringLiteral("Held keys (KDE Plasma)"), QStringLiteral("The on-screen keyboard offers accented letters when a key is held. While it does, held A, S, D, C and E do not reach the game as keys."), QStringLiteral("warn"), QStringLiteral("Switch off"));
                           finishChecks();
                       });
        }
    }
    emit checksChanged();
}

void Backend::finishChecks() {
    if (m_pendingChecks > 0) --m_pendingChecks;
    if (m_pendingChecks > 0) return;
    // in a fixed order, whatever order the answers came in
    QVariantList merged = m_checks;
    for (const char* key : {"display", "sched", "kde-diacritics", "kde-tearing"})
        for (const QVariant& item : std::as_const(m_asyncChecks))
            if (item.toMap().value(QStringLiteral("key")).toString() == QLatin1String(key)) {
                // before the closing reminder about the game's own setting
                merged.insert(qMax<qsizetype>(merged.size() - 1, 0), item);
            }
    m_checks = merged;
    m_asyncChecks.clear();
    emit checksChanged();
}

void Backend::applyFix(const QString& key) {
    if (key == QStringLiteral("prefix")) {
        const QString found = detectPrefix();
        if (found.isEmpty()) {
            emit said(QStringLiteral("No game folder found in the usual places: choose it in Setup"));
            return;
        }
        m_conf.insert(QStringLiteral("prefix"), found);
        saveConf();
        emit said(QStringLiteral("Found the game in %1").arg(tidyPath(found)));
        refreshChecks();
    } else if (key == QStringLiteral("kde-tearing")) {
        startAsync(QStringLiteral("kwriteconfig6"), {QStringLiteral("--file"), QStringLiteral("kwinrc"), QStringLiteral("--group"), QStringLiteral("Compositing"), QStringLiteral("--key"), QStringLiteral("AllowTearing"), QStringLiteral("--type"), QStringLiteral("bool"), QStringLiteral("false")},
                   [this](int code, const QString&) {
                       if (code == 0) QProcess::startDetached(QStringLiteral("busctl"), {QStringLiteral("--user"), QStringLiteral("call"), QStringLiteral("org.kde.KWin"), QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"), QStringLiteral("reconfigure")});
                       emit said(code == 0 ? QStringLiteral("Tearing switched off in KWin") : QStringLiteral("Could not change KWin's setting"));
                       refreshChecks();
                   });
    } else if (key == QStringLiteral("kde-diacritics")) {
        startAsync(QStringLiteral("kwriteconfig6"), {QStringLiteral("--file"), QStringLiteral("plasmakeyboardrc"), QStringLiteral("--group"), QStringLiteral("General"), QStringLiteral("--key"), QStringLiteral("diacriticsPopupEnabled"), QStringLiteral("--type"), QStringLiteral("bool"), QStringLiteral("--notify"), QStringLiteral("false")},
                   [this](int code, const QString&) {
                       emit said(code == 0 ? QStringLiteral("Accented-letter popup switched off") : QStringLiteral("Could not change the keyboard's setting"));
                       refreshChecks();
                   });
    }
}

// ---------------------------------------------------------------------------------------------
// A place in the application menu and a command in the terminal, for an AppImage that lies anywhere.

static QString menuFile() { return xdgDir("XDG_DATA_HOME", QStringLiteral("/.local/share")) + QStringLiteral("/applications/") + QLatin1String(kAppId) + QStringLiteral(".desktop"); }
static QString iconFile() { return xdgDir("XDG_DATA_HOME", QStringLiteral("/.local/share")) + QStringLiteral("/icons/hicolor/scalable/apps/") + QLatin1String(kAppId) + QStringLiteral(".svg"); }
static QString binDir() { return qEnvironmentVariable("MYRDDIN_BIN_DIR", home() + QStringLiteral("/.local/bin")); }

// The menu entry's icon: Myrddin's mark in the desktop's colours as they were at the window's last start
// (a file in the icon folder cannot follow them by itself), or the picture built in when there are none.
static void writeIcon(const QByteArray& svg) {
    QByteArray wanted = svg;
    if (wanted.isEmpty()) {
        QFile builtIn(QStringLiteral(":/qt/qml/Myrddin/assets/icon.svg"));
        if (builtIn.open(QIODevice::ReadOnly)) wanted = builtIn.readAll();
    }
    QFile now(iconFile());
    if (now.open(QIODevice::ReadOnly) && now.readAll() == wanted) return;
    now.close();
    QDir().mkpath(QFileInfo(iconFile()).absolutePath());
    QSaveFile file(iconFile());
    if (file.open(QIODevice::WriteOnly) && file.write(wanted) >= 0) file.commit();
}

void Backend::setIconSvg(const QByteArray& svg) {
    m_iconSvg = svg;
    if (menuEntry()) writeIcon(m_iconSvg);
}

bool Backend::menuEntry() const { return QFile::exists(menuFile()); }
bool Backend::cliLink() const { return QFileInfo(binDir() + QStringLiteral("/myrddin")).isSymLink(); }

QString Backend::cliNote() const {
    const QStringList path = qEnvironmentVariable("PATH").split(QLatin1Char(':'));
    if (cliLink() && !path.contains(binDir())) return QStringLiteral("%1 is not in your PATH yet: add it in your shell's settings, or type the full path.").arg(tidyPath(binDir()));
    return {};
}

void Backend::setMenuEntry(bool on) {
    if (on) {
        QDir().mkpath(QFileInfo(menuFile()).absolutePath());
        writeIcon(m_iconSvg);
        QSaveFile file(menuFile());
        if (file.open(QIODevice::WriteOnly)) {
            QString exec = selfPath();
            exec.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
            file.write(QStringLiteral("[Desktop Entry]\nType=Application\nName=Myrddin\nGenericName=Frame generation for Star Citizen\n"
                                      "Comment=Launch Star Citizen with frame generation and change its settings\nExec=\"%1\"\nIcon=%2\n"
                                      "Terminal=false\nCategories=Game;\nStartupWMClass=%2\n")
                           .arg(exec, QLatin1String(kAppId))
                           .toUtf8());
            file.commit();
        }
        emit said(QStringLiteral("Myrddin is in the application menu"));
    } else {
        QFile::remove(menuFile());
        QFile::remove(iconFile());
        emit said(QStringLiteral("Removed from the application menu"));
    }
    emit integrationChanged();
}

void Backend::setCliLink(bool on) {
    const QString link = binDir() + QStringLiteral("/myrddin");
    if (on) {
        QDir().mkpath(binDir());
        if (QFileInfo(link).isSymLink()) QFile::remove(link);
        if (QFile::exists(link)) emit said(QStringLiteral("%1 exists and is not a link: left alone").arg(tidyPath(link)));
        else if (QFile::link(selfPath(), link)) emit said(QStringLiteral("\"myrddin\" now starts this program from a terminal"));
        else emit said(QStringLiteral("Could not create %1").arg(tidyPath(link)));
    } else if (QFileInfo(link).isSymLink()) {
        QFile::remove(link);
        emit said(QStringLiteral("The \"myrddin\" command is removed"));
    }
    emit integrationChanged();
}

void Backend::openFolder(const QString& which) {
    QString dir = m_runtime;
    if (which == QStringLiteral("logs")) dir = m_runtime + QStringLiteral("/logs");
    else if (which == QStringLiteral("prefix")) dir = m_conf.value(QStringLiteral("prefix")).toString();
    else if (which == QStringLiteral("licences")) dir = licenceFolder();
    if (dir.isEmpty() || !QDir(dir).exists()) {
        emit said(QStringLiteral("That folder does not exist yet"));
        return;
    }
    QProcess process;
    process.setProgram(QStringLiteral("xdg-open"));
    process.setArguments({dir});
    process.setProcessEnvironment(childEnvironment());
    process.startDetached();
}

void Backend::copy(const QString& text) {
    if (auto* clipboard = QGuiApplication::clipboard()) clipboard->setText(text);
    emit said(QStringLiteral("Copied"));
}

QString Backend::tidyPath(const QString& path) const {
    QString text = path.startsWith(QStringLiteral("file://")) ? QUrl(path).toLocalFile() : path;
    if (text.size() > 1 && text.endsWith(QLatin1Char('/'))) text.chop(1);
    if (text == home()) return QStringLiteral("~");
    if (text.startsWith(home() + QLatin1Char('/'))) return QLatin1Char('~') + text.mid(home().size());
    return text;
}

QString Backend::fullPath(const QString& path) const {
    QString text = path.trimmed();
    if (text.startsWith(QStringLiteral("file://"))) text = QUrl(text).toLocalFile();
    if (text == QStringLiteral("~")) return home();
    if (text.startsWith(QStringLiteral("~/"))) text = home() + text.mid(1);
    if (text.size() > 1 && text.endsWith(QLatin1Char('/'))) text.chop(1);
    return text;
}
