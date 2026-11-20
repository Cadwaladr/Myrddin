#include "systemcolors.h"

#include "backend.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QPalette>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStyleHints>

#include <cmath>

namespace {

double channel(double c) { return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double luminance(const QColor& c) { return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF()); }
double contrast(const QColor& a, const QColor& b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}
QColor mix(const QColor& a, const QColor& b, double t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t, a.blueF() + (b.blueF() - a.blueF()) * t);
}
// A colour of the desktop's that is used for small signs and words: where the scheme sets it too
// near the background to be made out (a contrast under LEAST), it is moved towards the text colour
// until it can be. A scheme whose colours can be read is taken as it is.
QColor legible(QColor colour, const QColor& background, const QColor& text, double least) {
    for (int i = 0; i < 8 && contrast(colour, background) < least; i++) colour = mix(colour, text, 0.2);
    return colour;
}

// "r,g,b" (0-255) or "#rrggbb"
QColor parse(const QString& value) {
    const QStringList parts = value.split(QLatin1Char(','));
    if (parts.size() >= 3) {
        bool r, g, b;
        const QColor colour(parts.at(0).trimmed().toInt(&r), parts.at(1).trimmed().toInt(&g), parts.at(2).trimmed().toInt(&b));
        return r && g && b ? colour : QColor();
    }
    return QColor::fromString(value.trimmed());
}

// The file's "[group] key=value" lines as "group/key" -> value.
QHash<QString, QString> ini(const QString& file) {
    QHash<QString, QString> out;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QString group;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            group = line.mid(1, line.size() - 2);
        } else if (const int at = line.indexOf(QLatin1Char('=')); at > 0 && !line.startsWith(QLatin1Char('#'))) {
            out.insert(group + QLatin1Char('/') + line.left(at).trimmed(), line.mid(at + 1).trimmed());
        }
    }
    return out;
}

QString configHome() { return qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + QStringLiteral("/.config")); }

// The desktop's accent colour, asked of its settings portal with the system's own busctl (one short
// call at the start). Invalid when there is no portal, no such setting, or none is chosen.
QColor portalAccent() {
    QProcess ask;
    ask.setProcessEnvironment(Backend::childEnvironment());   // (the system's busctl with the system's libraries, not an AppImage's)
    ask.start(QStringLiteral("busctl"), {QStringLiteral("--user"), QStringLiteral("--timeout=1"), QStringLiteral("call"), QStringLiteral("org.freedesktop.portal.Desktop"),
                                         QStringLiteral("/org/freedesktop/portal/desktop"), QStringLiteral("org.freedesktop.portal.Settings"), QStringLiteral("ReadOne"),
                                         QStringLiteral("ss"), QStringLiteral("org.freedesktop.appearance"), QStringLiteral("accent-color")});
    if (!ask.waitForFinished(1500) || ask.exitCode() != 0) {
        ask.kill();
        ask.waitForFinished(200);
        return {};
    }
    // v (ddd) 0.2 0.4 0.9
    static const QRegularExpression three(QStringLiteral(R"(\(ddd\)\s+([0-9.eE+-]+)\s+([0-9.eE+-]+)\s+([0-9.eE+-]+))"));
    const auto match = three.match(QString::fromUtf8(ask.readAllStandardOutput()));
    if (!match.hasMatch()) return {};
    const double r = match.captured(1).toDouble(), g = match.captured(2).toDouble(), b = match.captured(3).toDouble();
    if (r < 0 || r > 1 || g < 0 || g > 1 || b < 0 || b > 1) return {};   // (out of range means "none chosen")
    return QColor::fromRgbF(r, g, b);
}

}   // namespace

SystemColors::SystemColors(QObject* parent) : QObject(parent) {
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toUpper();
    if (qEnvironmentVariableIsEmpty("MYRDDIN_COLORS") && !desktop.contains(QStringLiteral("KDE")) && !desktop.isEmpty()) m_accent = portalAccent();
    m = read();
    // The desktop's colours can change while the window is open: looked at every few seconds (the
    // time a file was changed, or Qt's palette), and at once when Qt says its palette changed.
    m_look.setInterval(3000);
    connect(&m_look, &QTimer::timeout, this, &SystemColors::refresh);
    m_look.start();
    if (auto* hints = QGuiApplication::styleHints()) connect(hints, &QStyleHints::colorSchemeChanged, this, [this] { m_stamp = {}; refresh(); });
}

bool SystemColors::dark() const { return luminance(m.window) < luminance(m.windowText); }

bool SystemColors::fromKde(const QString& file, Set* set) const {
    QHash<QString, QString> values = ini(file);
    const auto colour = [&values](const char* group, const char* key) { return parse(values.value(QStringLiteral("Colors:%1/%2").arg(QLatin1String(group), QLatin1String(key)))); };
    if (!colour("Window", "BackgroundNormal").isValid()) {
        // kdeglobals without the colours themselves: they are in the scheme file it names
        const QString scheme = values.value(QStringLiteral("General/ColorScheme"));
        const QString found = scheme.isEmpty() ? QString() : QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("color-schemes/%1.colors").arg(scheme));
        if (found.isEmpty()) return false;
        const QString accent = values.value(QStringLiteral("General/AccentColor"));
        values = ini(found);
        if (!accent.isEmpty()) values.insert(QStringLiteral("General/AccentColor"), accent);
    }
    Set s;
    s.window = colour("Window", "BackgroundNormal");
    s.windowText = colour("Window", "ForegroundNormal");
    s.view = colour("View", "BackgroundNormal");
    s.button = colour("Button", "BackgroundNormal");
    s.highlight = colour("Selection", "BackgroundNormal");
    s.highlightText = colour("Selection", "ForegroundNormal");
    s.positive = colour("Window", "ForegroundPositive");
    s.neutral = colour("Window", "ForegroundNeutral");
    s.negative = colour("Window", "ForegroundNegative");
    if (const QColor accent = parse(values.value(QStringLiteral("General/AccentColor"))); accent.isValid()) s.highlight = accent;
    if (!s.window.isValid() || !s.windowText.isValid()) return false;
    s.source = file;
    *set = s;
    return true;
}

void SystemColors::fromPalette(Set* set) const {
    const QPalette palette = QGuiApplication::palette();
    set->window = palette.color(QPalette::Active, QPalette::Window);
    set->windowText = palette.color(QPalette::Active, QPalette::WindowText);
    set->view = palette.color(QPalette::Active, QPalette::Base);
    set->button = palette.color(QPalette::Active, QPalette::Button);
    set->highlight = m_accent.isValid() ? m_accent : palette.color(QPalette::Active, QPalette::Highlight);
    set->highlightText = m_accent.isValid() ? QColor() : palette.color(QPalette::Active, QPalette::HighlightedText);
    set->source = m_accent.isValid() ? QStringLiteral("the desktop's palette and accent colour") : QStringLiteral("the desktop's palette");
}

SystemColors::Set SystemColors::read() {
    Set s;
    m_file.clear();
    // (MYRDDIN_COLORS: a file like kdeglobals, for the tests and the pictures)
    const QString given = qEnvironmentVariable("MYRDDIN_COLORS");
    const bool kde = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toUpper().contains(QStringLiteral("KDE"));
    const QString file = !given.isEmpty() ? given : kde ? configHome() + QStringLiteral("/kdeglobals") : QString();
    if (!file.isEmpty() && fromKde(file, &s)) m_file = file;
    else fromPalette(&s);
    m_stamp = m_file.isEmpty() ? QDateTime() : QFileInfo(m_file).lastModified();

    // What a scheme leaves out, and what it sets so that it cannot be read.
    const bool dark = luminance(s.window) < luminance(s.windowText);
    if (!s.view.isValid()) s.view = mix(s.window, dark ? QColor(Qt::black) : QColor(Qt::white), 0.35);
    if (!s.button.isValid()) s.button = mix(s.window, s.windowText, 0.08);
    if (!s.highlight.isValid()) s.highlight = dark ? QColor(0x3d, 0xae, 0xe9) : QColor(0x2a, 0x7d, 0xb8);
    if (!s.highlightText.isValid() || contrast(s.highlightText, s.highlight) < 2.0)
        s.highlightText = luminance(s.highlight) > 0.4 ? mix(QColor(Qt::black), s.highlight, 0.12) : QColor(Qt::white);
    if (!s.positive.isValid()) s.positive = dark ? QColor(0x4f, 0xd9, 0x8a) : QColor(0x1e, 0x8e, 0x4e);
    if (!s.neutral.isValid()) s.neutral = dark ? QColor(0xf2, 0xb8, 0x4b) : QColor(0xb0, 0x72, 0x0a);
    if (!s.negative.isValid()) s.negative = dark ? QColor(0xf2, 0x71, 0x6b) : QColor(0xc9, 0x30, 0x2c);
    s.highlight = legible(s.highlight, s.window, s.windowText, 1.6);   // (fills buttons and switches: as the desktop has it)
    s.positive = legible(s.positive, s.window, s.windowText, 3.0);     // (small signs that say how things stand)
    s.neutral = legible(s.neutral, s.window, s.windowText, 3.0);
    s.negative = legible(s.negative, s.window, s.windowText, 3.0);
    return s;
}

void SystemColors::refresh() {
    if (!m_file.isEmpty() && m_stamp.isValid() && QFileInfo(m_file).lastModified() == m_stamp) return;
    const Set now = read();
    if (now == m) return;
    m = now;
    emit changed();
}

QByteArray SystemColors::iconSvg() const {
    const QString edge = mix(m.window, m.windowText, 0.22).name();
    return QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"256\" height=\"256\" viewBox=\"0 0 256 256\">\n"
                          "  <rect x=\"17\" y=\"17\" width=\"222\" height=\"222\" rx=\"52\" fill=\"%1\" stroke=\"%2\" stroke-width=\"2\"/>\n"
                          "  <path d=\"M64 180 V84 L128 148 L192 84 V180\" fill=\"none\" stroke=\"%3\" stroke-width=\"20\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>\n"
                          "  <circle cx=\"128\" cy=\"96\" r=\"9\" fill=\"%4\"/>\n"
                          "</svg>\n")
        .arg(m.window.name(), edge, m.highlight.name(), m.windowText.name())
        .toUtf8();
}
