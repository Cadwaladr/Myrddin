// The Console page's data: what the game's process is doing, read from outside it the way a task
// manager reads it (its CPU time, memory and threads from /proc/PID/stat and /proc/PID/status, the GPU
// from nvidia-smi), and the logs as they grow. Nothing here touches the game: nothing is attached, none
// of its memory is read, and nothing that walks its memory map (smaps) is asked for, since that holds
// the game's own memory calls up for as long as it takes.
#pragma once

#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

class Backend;

class GameConsole : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)   // the page is shown: read once a second
    Q_PROPERTY(QString source READ source NOTIFY sourceChanged)                 // layer | game | launch
    Q_PROPERTY(QString text READ text NOTIFY textChanged)                       // the end of that log
    Q_PROPERTY(QVariantMap info READ info NOTIFY infoChanged)                   // the process, the GPU, the machine

public:
    explicit GameConsole(Backend* backend, QObject* parent = nullptr);

    bool active() const { return m_active; }
    void setActive(bool on);
    QString source() const { return m_source; }
    QString text() const { return m_text; }
    QVariantMap info() const { return m_info; }

    Q_INVOKABLE void setSource(const QString& which);
    Q_INVOKABLE void openFolder();   // the folder of the log that is shown

signals:
    void activeChanged();
    void sourceChanged();
    void textChanged();
    void infoChanged();

private:
    void tick();
    void readProcess();
    void readGpu();
    void readLog();
    QString logPath() const;
    QString gameLogPath() const;

    Backend* m_backend;
    QTimer m_timer;
    bool m_active = false;
    QString m_source = QStringLiteral("layer");
    QString m_text;
    QVariantMap m_info;
    QString m_fake;
    // the game's CPU time at the last look, to tell how much of the CPU it used since
    qint64 m_pid = 0;
    quint64 m_ticks = 0;
    qint64 m_ticksAt = 0;
    // the log that is followed: where its last read ended, and its last lines
    QString m_logFile;
    qint64 m_logPos = 0;
    QStringList m_lines;
    QString m_partial;
    QProcess* m_smi = nullptr;
    QVariantMap m_gpu;
    qint64 m_gpuAt = 0;
};
