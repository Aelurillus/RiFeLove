#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QUrl>
#include <QCommandLineParser>
#include <QQuickWindow>
#include <QTimer>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QDir>
#ifdef RIFELOVE_DESKTOP_PROCESSING
#include "studio_controller.hpp"
#include "queue_controller.hpp"
#endif

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle("Basic");
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("RifeLove");
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption screenshot("smoke-screenshot","Render one startup frame for the local UI build check and exit.","path");
    parser.addOption(screenshot);
    const QCommandLineOption dataDirectory("data-dir","Store local task history and preferences in this directory.","path");
    parser.addOption(dataDirectory);
    parser.process(app);
#ifdef RIFELOVE_DESKTOP_PROCESSING
    StudioController studio;
    const auto dataRoot = parser.isSet(dataDirectory) ? parser.value(dataDirectory) :
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QueueController taskQueue(QDir(dataRoot).filePath("queue.sqlite"));
#endif
    QQmlApplicationEngine engine;
#ifdef RIFELOVE_DESKTOP_PROCESSING
    engine.rootContext()->setContextProperty("studio", &studio);
    engine.rootContext()->setContextProperty("taskQueue", &taskQueue);
    engine.rootContext()->setContextProperty("processingAvailable", true);
#else
    engine.rootContext()->setContextProperty("studio", QVariant());
    engine.rootContext()->setContextProperty("taskQueue", QVariant());
    engine.rootContext()->setContextProperty("processingAvailable", false);
#endif
    engine.loadFromModule("RifeLove", "Main");
    if (engine.rootObjects().isEmpty()) return 1;
    if (parser.isSet(screenshot)) {
        QTimer::singleShot(1000,&app,[&] {
            const auto window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
            if (!window || !window->grabWindow().save(parser.value(screenshot))) app.exit(2);
            else app.exit(0);
        });
    }
    return app.exec();
}
