// Myrddin: the window for sc-turbo, and its command line.
//
//   myrddin                     the window
//   myrddin fg 4 hud            anything that does not start with "-" goes to sc-turbo as it is
//   myrddin --runtime DIR       use the sc-turbo in DIR (a source checkout) instead of the installed copy
//   myrddin --screenshot FILE [--page NAME] [--settle MS] [--size WxH]   a picture of the window, for tests
//   myrddin --test-eval CODE    for tests: a QML expression run half a second after the start
//                               ("backend.launch()"; "@2600 CODE": after that many milliseconds;
//                               "pointer.move(X, Y)" and "pointer.click(X, Y)" stand in for a mouse)
//
// Run from the AppImage, the runtime (sc-turbo, its tools, the layer) is first copied to
// ~/.local/share/myrddin/runtime when it is missing or older there.
#include "backend.h"
#include "gameconsole.h"
#include "systemcolors.h"
#include "testpointer.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QLoggingCategory>
#include <QPixmap>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

namespace {

// "myrddin fg 4 hud": no window, no display needed. sc-turbo takes over this process.
int passThrough(int argc, char** argv) {
    QCoreApplication core(argc, argv);
    const QString runtime = Backend::resolveRuntime({});
    const QString note = Backend::installPayload(runtime, Backend::processUp("StarCitizen.exe") || Backend::launcherOpen({}));
    if (!note.isEmpty()) std::fprintf(stderr, "myrddin: %s\n", qPrintable(note));
    const QByteArray program = QFile::encodeName(runtime + QStringLiteral("/sc-turbo"));
    if (::access(program.constData(), X_OK) != 0) {
        std::fprintf(stderr, "myrddin: no sc-turbo in %s\n", qPrintable(runtime));
        return 1;
    }
    const QProcessEnvironment env = Backend::childEnvironment();
    ::clearenv();
    for (const QString& key : env.keys()) ::setenv(key.toLocal8Bit().constData(), env.value(key).toLocal8Bit().constData(), 1);
    std::vector<char*> words;
    QByteArray first = program;
    words.push_back(first.data());
    for (int i = 1; i < argc; ++i) words.push_back(argv[i]);
    words.push_back(nullptr);
    ::execv(program.constData(), words.data());
    std::perror("myrddin: sc-turbo");
    return 1;
}

}   // namespace

int main(int argc, char** argv) {
    // sc-turbo's own launcher window is this one now
    if (argc > 1 && argv[1][0] != '-' && std::strcmp(argv[1], "gui") != 0) return passThrough(argc, argv);

    // (Qt ends with a core dump when there is no display to open a window on: say it plainly instead)
    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") && qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        std::fprintf(stderr, "myrddin: no display to open the window on. From a terminal: myrddin fg 4 (all commands: myrddin help)\n");
        return 1;
    }
    // (Qt warns that the desktop's portal does not know this program until its menu entry is installed)
    QLoggingCategory::setFilterRules(QStringLiteral("qt.qpa.services.warning=false"));
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Myrddin"));
    QGuiApplication::setApplicationVersion(QStringLiteral(MYRDDIN_VERSION));
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.cadwaladr.Myrddin"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/qt/qml/Myrddin/assets/icon.svg")));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QCommandLineParser args;
    args.setApplicationDescription(QStringLiteral("Frame generation for Star Citizen: the window. Words that do not start with \"-\" go to sc-turbo (myrddin fg 4)."));
    args.addHelpOption();
    args.addVersionOption();
    args.addOption({QStringLiteral("runtime"), QStringLiteral("Use the sc-turbo in DIR."), QStringLiteral("DIR")});
    args.addOption({QStringLiteral("screenshot"), QStringLiteral("Save a picture of the window to FILE and leave."), QStringLiteral("FILE")});
    args.addOption({QStringLiteral("page"), QStringLiteral("The page to show at the start (play, console, setup, options, about, welcome)."), QStringLiteral("NAME")});
    args.addOption({QStringLiteral("settle"), QStringLiteral("Milliseconds to wait before the picture is taken."), QStringLiteral("MS"), QStringLiteral("1500")});
    args.addOption({QStringLiteral("size"), QStringLiteral("The window's size for the picture."), QStringLiteral("WxH")});
    args.addOption({QStringLiteral("test-eval"), QStringLiteral("For tests: run this QML expression half a second after the start (backend.launch())."), QStringLiteral("CODE")});
    args.addPositionalArgument(QStringLiteral("gui"), QStringLiteral("(optional) the window, as \"sc-turbo gui\" was"), QStringLiteral("[gui]"));
    args.process(app);

    const int font = QFontDatabase::addApplicationFont(QStringLiteral(":/qt/qml/Myrddin/assets/InterVariable.ttf"));
    const QStringList families = font >= 0 ? QFontDatabase::applicationFontFamilies(font) : QStringList();
    if (!families.isEmpty()) {
        QFont base(families.first());
        base.setPixelSize(14);
        QGuiApplication::setFont(base);
    }

    Backend backend(args.value(QStringLiteral("runtime")));
    // The window, its mark and its icons are drawn in the desktop's own colours, and follow them.
    SystemColors colors;
    const auto wearColors = [&colors, &backend] {
        const QByteArray mark = colors.iconSvg();
        QPixmap picture;
        if (picture.loadFromData(mark, "SVG")) QGuiApplication::setWindowIcon(QIcon(picture));
        backend.setIconSvg(mark);
    };
    wearColors();
    QObject::connect(&colors, &SystemColors::changed, &app, wearColors);
    GameConsole gameConsole(&backend);   // the Console page: the game's process and its logs, read from outside
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
    engine.rootContext()->setContextProperty(QStringLiteral("gameConsole"), &gameConsole);
    engine.rootContext()->setContextProperty(QStringLiteral("systemColors"), &colors);
    engine.rootContext()->setContextProperty(QStringLiteral("startPage"), args.value(QStringLiteral("page")));
    engine.rootContext()->setContextProperty(QStringLiteral("picture"), args.isSet(QStringLiteral("screenshot")));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(2); }, Qt::QueuedConnection);
    engine.loadFromModule("Myrddin", "Main");
    if (engine.rootObjects().isEmpty()) return 2;

    if (args.isSet(QStringLiteral("test-eval")))
        engine.rootContext()->setContextProperty(QStringLiteral("pointer"), new TestPointer(qobject_cast<QQuickWindow*>(engine.rootObjects().first()), &engine));
    for (QString code : args.values(QStringLiteral("test-eval"))) {
        int after = 500;   // ("@2600 CODE": that many milliseconds after the start)
        static const QRegularExpression timed(QStringLiteral(R"(^@(\d+)\s+)"));
        if (const auto match = timed.match(code); match.hasMatch()) {
            after = match.captured(1).toInt();
            code = code.mid(match.capturedLength());
        }
        QTimer::singleShot(after, &engine, [&engine, code] {
            QObject* root = engine.rootObjects().first();
            QQmlExpression expression(QQmlEngine::contextForObject(root), root, code);   // (the window's ids are known there)
            expression.evaluate();
            if (expression.hasError()) std::fprintf(stderr, "myrddin: --test-eval: %s\n", qPrintable(expression.error().toString()));
        });
    }

    if (args.isSet(QStringLiteral("screenshot"))) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        const QStringList size = args.value(QStringLiteral("size")).split(QLatin1Char('x'));
        if (window && size.size() == 2) window->resize(size.at(0).toInt(), size.at(1).toInt());
        const QString file = args.value(QStringLiteral("screenshot"));
        QTimer::singleShot(args.value(QStringLiteral("settle")).toInt(), window, [window, file] {
            const bool ok = window && window->grabWindow().save(file);
            QCoreApplication::exit(ok ? 0 : 3);
        });
    }
    return app.exec();
}
