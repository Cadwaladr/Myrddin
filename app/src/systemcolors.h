// The colours the desktop is set to, for the window to draw itself in. Nothing of Myrddin's own
// is mixed in: on KDE Plasma they are the colour scheme's (kdeglobals, or the scheme file it
// names), elsewhere Qt's palette, which follows the desktop's light or dark setting, with the
// desktop's accent colour where its settings portal knows one.
#pragma once

#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimer>

class SystemColors : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor window READ window NOTIFY changed)
    Q_PROPERTY(QColor windowText READ windowText NOTIFY changed)
    Q_PROPERTY(QColor view READ view NOTIFY changed)
    Q_PROPERTY(QColor button READ button NOTIFY changed)
    Q_PROPERTY(QColor highlight READ highlight NOTIFY changed)
    Q_PROPERTY(QColor highlightText READ highlightText NOTIFY changed)
    Q_PROPERTY(QColor positive READ positive NOTIFY changed)
    Q_PROPERTY(QColor neutral READ neutral NOTIFY changed)
    Q_PROPERTY(QColor negative READ negative NOTIFY changed)
    Q_PROPERTY(bool dark READ dark NOTIFY changed)
    Q_PROPERTY(QString source READ source NOTIFY changed)

public:
    explicit SystemColors(QObject* parent = nullptr);

    QColor window() const { return m.window; }
    QColor windowText() const { return m.windowText; }
    QColor view() const { return m.view; }
    QColor button() const { return m.button; }
    QColor highlight() const { return m.highlight; }
    QColor highlightText() const { return m.highlightText; }
    QColor positive() const { return m.positive; }
    QColor neutral() const { return m.neutral; }
    QColor negative() const { return m.negative; }
    bool dark() const;
    QString source() const { return m.source; }   // where they were read from, for the About page and the tests

    QByteArray iconSvg() const;   // Myrddin's mark in these colours
    void refresh();               // reads again when something may have changed

signals:
    void changed();

private:
    struct Set {
        QColor window, windowText, view, button, highlight, highlightText, positive, neutral, negative;
        QString source;
        bool operator==(const Set&) const = default;
    };
    Set read();
    bool fromKde(const QString& file, Set* set) const;
    void fromPalette(Set* set) const;

    Set m;
    QString m_file;       // the file the colours came from, "" for Qt's palette
    QDateTime m_stamp;    // its time of change when it was read
    QColor m_accent;      // the settings portal's accent colour (asked once), invalid when it has none
    QTimer m_look;
};
