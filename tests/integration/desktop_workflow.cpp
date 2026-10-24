#include "studio_controller.hpp"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void wait(StudioController& studio) {
    if (!studio.busy()) return;
    QEventLoop loop;
    QTimer timeout; timeout.setSingleShot(true);
    QObject::connect(&studio, &StudioController::operationFinished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(90000); loop.exec();
    require(!studio.busy(), "Desktop operation timed out");
}
void click(QObject* button) {
    require(button && button->property("enabled").toBool(), "QML start button is unavailable");
    require(QMetaObject::invokeMethod(button, "clicked", Qt::DirectConnection), "Could not activate QML start button");
}
}
int main(int argc, char** argv) {
    QQuickStyle::setStyle("Basic");
    QGuiApplication app(argc, argv);
    try {
        const auto arguments = app.arguments();
        require(arguments.size() == 8, "Expected source, output, pack, runtime, QML, screenshot and report paths");
        StudioController studio;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("studio", &studio);
        engine.rootContext()->setContextProperty("processingAvailable", true);
        engine.load(QUrl::fromLocalFile(arguments[5]));
        require(!engine.rootObjects().isEmpty(), "QML application did not load");
        auto* root = engine.rootObjects().front();
        auto* button = root->findChild<QObject*>("startButton");
        require(button && !button->property("enabled").toBool(), "Start should be disabled without input/model");
        root->setProperty("modelFolder", QUrl::fromLocalFile(arguments[3]));
        root->setProperty("runtimeFolder", QUrl::fromLocalFile(arguments[4]));
        studio.openSource(QUrl::fromLocalFile(arguments[1]));
        require(studio.busy(), "Inspection should run asynchronously");
        // Reentrant import is ignored while a worker owns the operation.
        studio.openSource(QUrl("https://example.invalid/video.mkv"));
        wait(studio);
        require(studio.status() == "inspected", "Source inspection failed");
        require(studio.media().value("width").toInt() == 96 && studio.media().value("height").toInt() == 64, "Wrong imported dimensions");
        studio.setOutput(QUrl::fromLocalFile(arguments[2]));
        int responsiveTicks = 0;
        QTimer heartbeat; heartbeat.setInterval(5);
        QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++responsiveTicks; });
        heartbeat.start();
        click(button);
        require(studio.busy(), "QML start did not submit asynchronous work");
        wait(studio);
        require(studio.status() == "completed", qPrintable(studio.detail()));
        require(QFileInfo(arguments[2]).isFile(), "Completed output was not published");
        require(studio.progress().value("frames").toULongLong() == 36, "Wrong output frame count");
        require(studio.progress().value("aiFrames").toULongLong() > 0, "Desktop did not generate AI frames");
        require(responsiveTicks > 5, "UI event loop did not stay responsive during processing");
        const auto aiFrames = studio.progress().value("aiFrames").toULongLong();
        // QML is rendered with the actual successful job, not a mocked view model.
        QEventLoop paint; QTimer::singleShot(100, &paint, &QEventLoop::quit); paint.exec();
        auto* window = qobject_cast<QQuickWindow*>(root);
        require(window && window->grabWindow().save(arguments[6]), "Workflow screenshot failed");
        // A repeated start must reject an existing output before worker launch.
        click(button);
        require(studio.status() == "failed" && !studio.busy(), "Existing output was not protected");
        const auto cancelledOutput = arguments[2] + ".cancelled.mkv";
        studio.setOutput(QUrl::fromLocalFile(cancelledOutput));
        auto* rate = root->findChild<QObject*>("targetRate");
        require(rate && rate->setProperty("editText", "240"), "Target rate control is unavailable");
        click(button);
        QTimer::singleShot(30, &studio, &StudioController::cancel);
        wait(studio);
        require(studio.status() == "cancelled", "Desktop cancellation was not observed");
        require(!QFileInfo::exists(cancelledOutput), "Cancelled output was published");
        require(QDir(QFileInfo(cancelledOutput).absolutePath()).entryList({"*.partial*"}, QDir::Files).isEmpty(), "Cancellation left temporary files");
        studio.openSource(QUrl("https://example.invalid/video.mkv"));
        require(studio.status() == "failed" && !studio.busy(), "Nonlocal import should fail immediately");
        QFile report(arguments[7]); require(report.open(QIODevice::WriteOnly | QIODevice::Truncate), "Could not write workflow report");
        report.write(QString("{\"success\":true,\"outputFrames\":36,\"aiFrames\":%1,\"responsiveTicks\":%2,\"existingOutputProtected\":true,\"cancelledWithoutOutput\":true}\n").arg(aiFrames).arg(responsiveTicks).toUtf8());
        std::cout << "Desktop QML workflow, responsiveness, output protection and cancellation passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
