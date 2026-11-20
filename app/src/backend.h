// Myrddin's window: everything the pages need from the system. The window never talks to the game:
// it writes sc-turbo's settings file, runs sc-turbo's commands and reads the layer's log.
#pragma once

#include <QObject>
#include <QProcessEnvironment>
#include <QQmlPropertyMap>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <functional>

class Backend : public QObject {
    Q_OBJECT
    // where things are
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString runtimeDir READ runtimeDir CONSTANT)
    Q_PROPERTY(QString runtimeVersion READ runtimeVersion NOTIFY runtimeChanged)
    Q_PROPERTY(QString runtimeNote READ runtimeNote NOTIFY runtimeChanged)
    Q_PROPERTY(bool fromAppImage READ fromAppImage CONSTANT)
    Q_PROPERTY(QString licenceFolder READ licenceFolder CONSTANT)
    // settings: conf is sc-turbo.conf (what the command line reads too), prefs are the window's own
    Q_PROPERTY(QQmlPropertyMap* conf READ conf CONSTANT)
    Q_PROPERTY(QQmlPropertyMap* prefs READ prefs CONSTANT)
    Q_PROPERTY(QString launchScriptInUse READ launchScriptInUse NOTIFY checksChanged)
    Q_PROPERTY(QString command READ command NOTIFY commandChanged)
    // what is going on
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)   // idle | starting | launcher | running
    Q_PROPERTY(bool layerSession READ layerSession NOTIFY stateChanged)
    Q_PROPERTY(bool generating READ generating NOTIFY stateChanged)
    Q_PROPERTY(int liveMultiplier READ liveMultiplier NOTIFY statsChanged)
    Q_PROPERTY(double baseRate READ baseRate NOTIFY statsChanged)
    Q_PROPERTY(double shownRate READ shownRate NOTIFY statsChanged)
    Q_PROPERTY(double frameP50 READ frameP50 NOTIFY statsChanged)
    Q_PROPERTY(double frameP99 READ frameP99 NOTIFY statsChanged)
    Q_PROPERTY(bool statsFresh READ statsFresh NOTIFY statsChanged)
    Q_PROPERTY(QVariantList baseHistory READ baseHistory NOTIFY statsChanged)
    Q_PROPERTY(QVariantList shownHistory READ shownHistory NOTIFY statsChanged)
    Q_PROPERTY(QString launchLog READ launchLog NOTIFY launchLogChanged)
    // the log of a session that ran with logs, while the user has not said what to do with it ("": none)
    Q_PROPERTY(QString sessionLog READ sessionLog NOTIFY sessionLogChanged)
    Q_PROPERTY(QString sessionLogNote READ sessionLogNote NOTIFY sessionLogChanged)
    // the system
    Q_PROPERTY(QVariantList checks READ checks NOTIFY checksChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY checksChanged)
    Q_PROPERTY(QString blocker READ blocker NOTIFY checksChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY checksChanged)
    Q_PROPERTY(QStringList displays READ displays NOTIFY checksChanged)
    Q_PROPERTY(QString displayChoice READ displayChoice NOTIFY checksChanged)
    Q_PROPERTY(QStringList schedulers READ schedulers CONSTANT)
    Q_PROPERTY(bool schedulerAllowed READ schedulerAllowed NOTIFY checksChanged)
    Q_PROPERTY(bool mangohudInstalled READ mangohudInstalled NOTIFY checksChanged)
    Q_PROPERTY(bool menuEntry READ menuEntry NOTIFY integrationChanged)
    Q_PROPERTY(bool cliLink READ cliLink NOTIFY integrationChanged)
    Q_PROPERTY(QString cliNote READ cliNote NOTIFY integrationChanged)
    Q_PROPERTY(bool toolBusy READ toolBusy NOTIFY toolChanged)
    Q_PROPERTY(QString toolTitle READ toolTitle NOTIFY toolChanged)
    Q_PROPERTY(QString toolOutput READ toolOutput NOTIFY toolChanged)

public:
    explicit Backend(const QString& runtimeOverride, QObject* parent = nullptr);

    // Used by main() before any window exists (also for "myrddin fg 4": the command line).
    static QString resolveRuntime(const QString& runtimeOverride);
    static QString installPayload(const QString& runtime, bool gameUp);   // returns a note, empty when all is well
    static bool processUp(const char* comm);
    static QProcessEnvironment childEnvironment();

    QString version() const;
    QString runtimeDir() const { return m_runtime; }
    QString runtimeVersion() const;
    QString runtimeNote() const { return m_runtimeNote; }
    bool fromAppImage() const;
    QString licenceFolder() const;
    QQmlPropertyMap* conf() { return &m_conf; }
    QQmlPropertyMap* prefs() { return &m_prefs; }
    QString launchScriptInUse() const;
    QString command() const;
    QString state() const { return m_state; }
    bool layerSession() const { return m_layerSession; }
    bool generating() const { return m_generating; }
    int liveMultiplier() const { return m_liveMult; }
    double baseRate() const { return m_base; }
    double shownRate() const { return m_shown; }
    double frameP50() const { return m_p50; }
    double frameP99() const { return m_p99; }
    bool statsFresh() const { return m_statsFresh; }
    QVariantList baseHistory() const { return m_baseHistory; }
    QVariantList shownHistory() const { return m_shownHistory; }
    QString launchLog() const { return m_launchLog; }
    QString sessionLog() const { return m_sessionLog; }
    QString sessionLogNote() const;
    void setIconSvg(const QByteArray& svg);   // Myrddin's mark in the desktop's colours: what the menu entry's icon is written from
    QVariantList checks() const { return m_checks; }
    bool ready() const { return m_blocker.isEmpty(); }
    QString blocker() const { return m_blocker; }
    bool checking() const { return m_pendingChecks > 0; }
    QStringList displays() const { return m_displays; }
    QString displayChoice() const { return m_displayChoice; }
    QStringList schedulers() const { return m_schedulers; }
    bool schedulerAllowed() const { return m_schedAllowed; }
    bool mangohudInstalled() const { return m_mangohud; }
    bool menuEntry() const;
    bool cliLink() const;
    QString cliNote() const;
    bool toolBusy() const { return m_toolBusy; }
    QString toolTitle() const { return m_toolTitle; }
    QString toolOutput() const { return m_toolOutput; }

    Q_INVOKABLE void launch();
    Q_INVOKABLE void setMultiplier(int n);            // also while the game runs
    Q_INVOKABLE void setGeneration(bool on);          // while the game runs
    Q_INVOKABLE void sendParam(const QString& name, const QString& value);
    Q_INVOKABLE void liveSwitch(const QString& command, bool on);   // fg-hud, fg-cap, fg-scd, fg-layer
    Q_INVOKABLE void panel(const QString& word);      // next | off | 1 | 2 | 3 | tl | tr | bl | br
    Q_INVOKABLE void runTool(const QString& title, const QStringList& words);
    Q_INVOKABLE void readSessionLog();     // the offered log, or the newest kept one, shown where the reports are
    Q_INVOKABLE void copySessionLog();     // onto the clipboard, as text and as a file
    Q_INVOKABLE void deleteSessionLog();   // the log and the recording it was made from
    Q_INVOKABLE void keepSessionLog();     // it stays in the log folder; the offer goes
    Q_INVOKABLE void refreshChecks();
    Q_INVOKABLE QString detectPrefix() const;         // "" when nothing is found
    Q_INVOKABLE bool looksLikePrefix(const QString& dir) const;
    Q_INVOKABLE void applyFix(const QString& key);
    Q_INVOKABLE void setMenuEntry(bool on);
    Q_INVOKABLE void setCliLink(bool on);
    Q_INVOKABLE void openFolder(const QString& which);   // logs | runtime | prefix | licences
    Q_INVOKABLE void copy(const QString& text);
    Q_INVOKABLE QString tidyPath(const QString& path) const;   // file:// off, ~ for the home folder
    Q_INVOKABLE QString fullPath(const QString& path) const;

signals:
    void runtimeChanged();
    void commandChanged();
    void stateChanged();
    void statsChanged();
    void launchLogChanged();
    void sessionLogChanged();
    void checksChanged();
    void integrationChanged();
    void toolChanged();
    void said(const QString& text);                   // a short line for the window to show and fade

private:
    void loadConf();
    void saveConf();
    void loadPrefs();
    void poll();
    void readStats(bool gameUp);
    void readLaunchLog();
    void makeSessionLog(bool last);
    QString pickSessionLog() const;
    QString tidyText(QString text) const;
    QStringList launchWords() const;
    bool scTurbo(const QStringList& words, QString* output = nullptr, int timeoutMs = 4000) const;
    void startAsync(const QString& program, const QStringList& words, std::function<void(int, const QString&)> done);
    void finishChecks();
    QString confPath() const;
    QString selfPath() const;

    QString m_runtime;
    QString m_runtimeNote;
    QQmlPropertyMap m_conf;
    QQmlPropertyMap m_prefs;
    QStringList m_confKept;        // lines of sc-turbo.conf this window does not know: written back as they were
    QTimer m_poll;
    QTimer m_saveLater;
    QString m_state = QStringLiteral("idle");
    qint64 m_startedAt = 0;        // when this window launched something (ms since the epoch), 0: nothing pending
    bool m_layerSession = false;
    bool m_generating = false;
    int m_liveMult = 0;
    double m_base = 0, m_shown = 0, m_p50 = 0, m_p99 = 0;
    bool m_statsFresh = false;
    QString m_statsLog;
    double m_statsStamp = -1;
    QVariantList m_baseHistory, m_shownHistory;
    QString m_launchLog;
    qint64 m_launchLogSize = -1;
    QString m_sessionLog;          // a session's log that waits for the user's answer
    bool m_logPending = false;     // a launch with logs whose log is still to be made (kept across a restart of the window)
    bool m_logBusy = false;
    qint64 m_logSince = 0;         // when that launch was (ms since the epoch): only a recording made after it counts
    QByteArray m_iconSvg;
    QVariantList m_checks;
    QVariantList m_asyncChecks;
    QString m_blocker;
    int m_pendingChecks = 0;
    int m_checkRound = 0;
    QStringList m_displays;
    QString m_displayChoice;
    QStringList m_schedulers;
    bool m_schedAllowed = false;
    bool m_mangohud = false;
    bool m_toolBusy = false;
    QString m_toolTitle, m_toolOutput;
    QString m_fake;                // MYRDDIN_FAKE=idle|launcher|running: for pictures and tests, no process is looked for
};
