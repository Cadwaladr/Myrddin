#include "gameconsole.h"

#include "backend.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQmlPropertyMap>
#include <QRegularExpression>

#include <unistd.h>

namespace {

constexpr int kLines = 400;             // the lines kept of the log that is shown
constexpr qint64 kFirst = 256 * 1024;   // of a long log, at first only its end is read

QString readText(const QString& path, qint64 max = 1 << 16) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.read(max)) : QString();
}

QString readEnd(const QString& path, qint64 bytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const qint64 size = file.size();
    if (size > bytes) file.seek(size - bytes);
    QString text = QString::fromUtf8(file.readAll());
    if (size > bytes) text = text.section(QLatin1Char('\n'), 1);   // (the first line was cut)
    return text;
}

// "Name:   N kB" in /proc/meminfo or /proc/PID/status: N, or -1
qint64 field(const QString& text, const QString& name) {
    const QRegularExpression line(QStringLiteral("^%1:\\s+(\\d+)").arg(QRegularExpression::escape(name)), QRegularExpression::MultilineOption);
    const auto match = line.match(text);
    return match.hasMatch() ? match.captured(1).toLongLong() : -1;
}

qint64 findGame() {
    const QStringList entries = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& entry : entries) {
        if (entry.isEmpty() || !entry.at(0).isDigit()) continue;
        if (readText(QStringLiteral("/proc/") + entry + QStringLiteral("/comm")).trimmed() == QStringLiteral("StarCitizen.exe")) return entry.toLongLong();
    }
    return 0;
}

// The layer's line every ten seconds is long; its first two parts are what a player wants to know.
QString shorter(const QString& line) {
    if (!line.contains(QStringLiteral(" STAT "))) return line;
    const int first = line.indexOf(QLatin1Char('|'));
    const int second = first < 0 ? -1 : line.indexOf(QLatin1Char('|'), first + 1);
    return second < 0 ? line : line.left(second).trimmed();
}

}  // namespace

GameConsole::GameConsole(Backend* backend, QObject* parent) : QObject(parent), m_backend(backend) {
    m_fake = qEnvironmentVariable("MYRDDIN_FAKE");
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &GameConsole::tick);
}

void GameConsole::setActive(bool on) {
    if (on == m_active) return;
    m_active = on;
    emit activeChanged();
    if (on) {
        tick();
        m_timer.start();
    } else {
        m_timer.stop();
    }
}

void GameConsole::setSource(const QString& which) {
    static const QStringList known = {QStringLiteral("layer"), QStringLiteral("game"), QStringLiteral("launch")};
    if (!known.contains(which) || which == m_source) return;
    m_source = which;
    m_logFile.clear();
    m_logPos = 0;
    m_lines.clear();
    m_partial.clear();
    emit sourceChanged();
    readLog();
}

void GameConsole::openFolder() {
    const QString path = logPath();
    const QString dir = path.isEmpty() ? m_backend->runtimeDir() + QStringLiteral("/logs") : QFileInfo(path).absolutePath();
    if (!QDir(dir).exists()) return;
    QProcess process;
    process.setProgram(QStringLiteral("xdg-open"));
    process.setArguments({dir});
    process.setProcessEnvironment(Backend::childEnvironment());
    process.startDetached();
}

void GameConsole::tick() {
    readProcess();
    readGpu();
    readLog();
}

void GameConsole::readProcess() {
    QVariantMap info;
    const bool fake = !m_fake.isEmpty();
    if (fake) {
        // (pictures and tests: figures of a busy place, nothing of this machine)
        const bool running = m_fake == QStringLiteral("running") || m_fake == QStringLiteral("plain");
        info = {{QStringLiteral("up"), running}, {QStringLiteral("launcher"), running || m_fake == QStringLiteral("launcher")},
                {QStringLiteral("cpus"), 20}, {QStringLiteral("ramFreeGb"), 10.9}, {QStringLiteral("swapGb"), 1.9},
                {QStringLiteral("gpuBusy"), 38}, {QStringLiteral("vramGb"), 10.6}, {QStringLiteral("vramTotalGb"), 12.0},
                {QStringLiteral("gpuWatts"), 76},
                {QStringLiteral("tuning"), QStringLiteral("CPU scheduler: scx_lavd  ·  CPU prefetchers off  ·  huge pages: always (21.4 GB of memory in them)")}};
        if (running) {
            info.insert(QStringLiteral("pid"), 123456);
            info.insert(QStringLiteral("seconds"), 5400);
            info.insert(QStringLiteral("cpu"), 12.6);
            info.insert(QStringLiteral("rssGb"), 40.3);
            info.insert(QStringLiteral("threads"), 191);
        }
        m_info = info;
        emit infoChanged();
        return;
    }
    const long clock = sysconf(_SC_CLK_TCK);
    info.insert(QStringLiteral("cpus"), static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)));
    const QString meminfo = readText(QStringLiteral("/proc/meminfo"));
    info.insert(QStringLiteral("ramFreeGb"), field(meminfo, QStringLiteral("MemAvailable")) / 1048576.0);
    const qint64 swapTotal = field(meminfo, QStringLiteral("SwapTotal")), swapFree = field(meminfo, QStringLiteral("SwapFree"));
    info.insert(QStringLiteral("swapGb"), swapTotal > 0 && swapFree >= 0 ? (swapTotal - swapFree) / 1048576.0 : 0.0);

    const qint64 pid = findGame();
    QString prefix = m_backend->conf()->value(QStringLiteral("prefix")).toString().trimmed();
    if (prefix.isEmpty()) prefix = QDir::homePath() + QStringLiteral("/Games/star-citizen");
    info.insert(QStringLiteral("up"), pid > 0);
    info.insert(QStringLiteral("launcher"), pid > 0 || Backend::launcherOpen(prefix));
    if (pid > 0) {
        // /proc/PID/stat after the name: state is field 0, utime 11, stime 12, starttime 19 (proc(5): 3, 14, 15, 22)
        const QString stat = readText(QStringLiteral("/proc/%1/stat").arg(pid));
        const QStringList f = stat.mid(stat.lastIndexOf(QLatin1Char(')')) + 2).split(QLatin1Char(' '));
        const quint64 ticks = f.value(11).toULongLong() + f.value(12).toULongLong();
        const double uptime = readText(QStringLiteral("/proc/uptime")).section(QLatin1Char(' '), 0, 0).toDouble();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        info.insert(QStringLiteral("pid"), pid);
        info.insert(QStringLiteral("seconds"), uptime - f.value(19).toDouble() / clock);
        // (how many CPU threads it kept busy since the last look; the first look has nothing to compare with)
        const bool again = pid == m_pid && m_ticksAt > 0 && now > m_ticksAt && ticks >= m_ticks;
        info.insert(QStringLiteral("cpu"), again ? (ticks - m_ticks) / double(clock) / ((now - m_ticksAt) / 1000.0)
                                                 : m_info.value(QStringLiteral("cpu"), 0.0).toDouble());
        m_pid = pid;
        m_ticks = ticks;
        m_ticksAt = now;
        const QString status = readText(QStringLiteral("/proc/%1/status").arg(pid));
        info.insert(QStringLiteral("rssGb"), field(status, QStringLiteral("VmRSS")) / 1048576.0);
        info.insert(QStringLiteral("threads"), field(status, QStringLiteral("Threads")));
    } else {
        m_pid = 0;
        m_ticksAt = 0;
    }

    // what is set on the machine for the game: the CPU scheduler, the prefetchers, huge pages
    QStringList tuning;
    const bool sched = readText(QStringLiteral("/sys/kernel/sched_ext/state")).trimmed() == QStringLiteral("enabled");
    tuning << (sched ? QStringLiteral("CPU scheduler: scx_") + readText(QStringLiteral("/sys/kernel/sched_ext/root/ops")).trimmed().section(QLatin1Char('_'), 0, 0)
                     : QStringLiteral("CPU scheduler: the stock one"));
    tuning << (readText(QStringLiteral("/run/myrddin-tune/state")).contains(QStringLiteral("prefetch=off")) ? QStringLiteral("CPU prefetchers off")
                                                                                                            : QStringLiteral("CPU prefetchers on"));
    const QString thp = readText(QStringLiteral("/sys/kernel/mm/transparent_hugepage/enabled"));
    const qint64 huge = field(meminfo, QStringLiteral("AnonHugePages"));
    if (thp.contains(QLatin1Char('[')))
        tuning << QStringLiteral("huge pages: %1 (%2 GB of memory in them)").arg(thp.section(QLatin1Char('['), 1).section(QLatin1Char(']'), 0, 0)).arg(qMax<qint64>(huge, 0) / 1048576.0, 0, 'f', 1);
    info.insert(QStringLiteral("tuning"), tuning.join(QStringLiteral("  ·  ")));
    for (auto it = m_gpu.cbegin(); it != m_gpu.cend(); ++it) info.insert(it.key(), it.value());
    m_info = info;
    emit infoChanged();
}

// The GPU: nvidia-smi every two seconds, without waiting for it.
void GameConsole::readGpu() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_fake.isEmpty() || m_smi || now - m_gpuAt < 2000) return;
    m_gpuAt = now;
    m_smi = new QProcess(this);
    m_smi->setProcessEnvironment(Backend::childEnvironment());
    connect(m_smi, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        const QStringList v = QString::fromUtf8(m_smi->readAllStandardOutput()).section(QLatin1Char('\n'), 0, 0).split(QLatin1Char(','));
        m_gpu.clear();
        if (code == 0 && v.size() >= 4) {
            m_gpu.insert(QStringLiteral("gpuBusy"), v.at(0).trimmed().toDouble());
            m_gpu.insert(QStringLiteral("vramGb"), v.at(1).trimmed().toDouble() / 1024.0);
            m_gpu.insert(QStringLiteral("vramTotalGb"), v.at(2).trimmed().toDouble() / 1024.0);
            m_gpu.insert(QStringLiteral("gpuWatts"), v.at(3).trimmed().toDouble());
        }
        m_smi->deleteLater();
        m_smi = nullptr;
        for (auto it = m_gpu.cbegin(); it != m_gpu.cend(); ++it) m_info.insert(it.key(), it.value());
        emit infoChanged();
    });
    connect(m_smi, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !m_smi) return;   // (no nvidia-smi: no GPU figures)
        m_smi->deleteLater();
        m_smi = nullptr;
    });
    m_smi->start(QStringLiteral("nvidia-smi"), {QStringLiteral("--query-gpu=utilization.gpu,memory.used,memory.total,power.draw"),
                                                QStringLiteral("--format=csv,noheader,nounits")});
}

QString GameConsole::gameLogPath() const {
    QString prefix = m_backend->conf()->value(QStringLiteral("prefix")).toString().trimmed();
    if (prefix.isEmpty()) prefix = QDir::homePath() + QStringLiteral("/Games/star-citizen");
    const QDir games(prefix + QStringLiteral("/drive_c/Program Files/Roberts Space Industries/StarCitizen"));
    QString newest;
    QDateTime when;
    for (const QFileInfo& channel : games.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {   // LIVE, PTU, ...
        const QFileInfo log(channel.absoluteFilePath() + QStringLiteral("/Game.log"));
        if (log.exists() && (newest.isEmpty() || log.lastModified() > when)) {
            newest = log.absoluteFilePath();
            when = log.lastModified();
        }
    }
    return newest;
}

QString GameConsole::logPath() const {
    if (m_source == QStringLiteral("game")) return gameLogPath();
    if (m_source == QStringLiteral("launch")) return m_backend->runtimeDir() + QStringLiteral("/logs/launch.out");
    QString newest;
    QDateTime when;
    const QFileInfoList logs = QDir(m_backend->runtimeDir() + QStringLiteral("/logs/fg")).entryInfoList({QStringLiteral("fg-*.log")}, QDir::Files);
    for (const QFileInfo& log : logs)
        if (newest.isEmpty() || log.lastModified() > when) {
            newest = log.absoluteFilePath();
            when = log.lastModified();
        }
    return newest;
}

void GameConsole::readLog() {
    if (m_source == QStringLiteral("launch")) {
        // small files, read whole each time: what the last launch said, and what was switched for the game
        static const QList<QPair<QString, QString>> parts = {{QStringLiteral("launch.out"), QStringLiteral("The last launch")},
                                                             {QStringLiteral("sched.out"), QStringLiteral("The CPU scheduler")},
                                                             {QStringLiteral("tune.out"), QStringLiteral("The prefetchers and huge pages")}};
        QStringList out;
        for (const auto& [name, title] : parts) {
            const QString text = readEnd(m_backend->runtimeDir() + QStringLiteral("/logs/") + name, 16384).trimmed();
            if (!text.isEmpty()) out << QStringLiteral("== %1 (logs/%2)\n").arg(title, name) + text;
        }
        const QString text = out.join(QStringLiteral("\n\n"));
        if (text != m_text) {
            m_text = text;
            emit textChanged();
        }
        return;
    }
    const QString path = logPath();
    if (path != m_logFile) {
        m_logFile = path;
        m_logPos = 0;
        m_lines.clear();
        m_partial.clear();
        if (!m_text.isEmpty()) {
            m_text.clear();
            emit textChanged();
        }
    }
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;
    const qint64 size = file.size();
    if (size < m_logPos) {   // (a new file under the old name)
        m_logPos = 0;
        m_lines.clear();
        m_partial.clear();
    }
    if (size == m_logPos) return;
    const qint64 from = m_logPos == 0 && size > kFirst ? size - kFirst : m_logPos;
    file.seek(from);
    const QByteArray data = file.read(qMin<qint64>(size - from, 4 * 1024 * 1024));
    const bool cut = m_logPos == 0 && from > 0;
    m_logPos = from + data.size();
    QStringList lines = (m_partial + QString::fromUtf8(data)).split(QLatin1Char('\n'));
    if (cut && !lines.isEmpty()) lines.removeFirst();   // (the first line was cut)
    m_partial = lines.isEmpty() ? QString() : lines.takeLast();   // (a line not ended yet)
    for (const QString& line : lines) m_lines << shorter(line);
    while (m_lines.size() > kLines) m_lines.removeFirst();
    m_text = m_lines.join(QLatin1Char('\n'));
    emit textChanged();
}
