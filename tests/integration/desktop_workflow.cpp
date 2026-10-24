#include "studio_controller.hpp"
#include "queue_controller.hpp"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <iostream>
#include <stdexcept>
#include <functional>

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
void waitUntil(const std::function<bool()>& ready, const char* message) {
    QElapsedTimer elapsed; elapsed.start();
    while (!ready()) {
        require(elapsed.elapsed() < 90000, message);
        QEventLoop loop; QTimer::singleShot(10, &loop, &QEventLoop::quit); loop.exec();
    }
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
        const auto queueFolder = QDir(QFileInfo(arguments[2]).absolutePath()).filePath("queue-output");
        require(QDir().mkpath(queueFolder), "Could not create isolated queue output folder");
        QueueController taskQueue(QDir(QFileInfo(arguments[2]).absolutePath()).filePath("desktop-queue.sqlite"));
        waitUntil([&] { return taskQueue.available() || taskQueue.state() == "error"; }, "Local desktop queue did not initialize");
        require(taskQueue.available(), qPrintable(taskQueue.detail()));
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("studio", &studio);
        engine.rootContext()->setContextProperty("taskQueue", &taskQueue);
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
        const auto batchFolder = QDir(QFileInfo(arguments[1]).absolutePath()).filePath("queue-input");
        taskQueue.addItems({QUrl::fromLocalFile(batchFolder),QUrl::fromLocalFile(arguments[1]),QUrl::fromLocalFile(arguments[1])},QUrl::fromLocalFile(queueFolder),"60",
            QUrl::fromLocalFile(arguments[3]),QUrl::fromLocalFile(arguments[4]),"libopenh264","mkv",true);
        waitUntil([&] { return !taskQueue.importing(); }, "Desktop queue import timed out");
        require(taskQueue.tasks()->rowCount() == 2, qPrintable(taskQueue.detail()));
        require(taskQueue.detail().contains(QStringLiteral("跳过 1 个不支持的文件")), "Unsupported HDR input was not reported as skipped");
        auto* queuePage = root->findChild<QObject*>("queuePage");
        require(queuePage, "Queue page was not loaded into the desktop UI");
        queuePage->setProperty("modelFolder",QUrl::fromLocalFile(arguments[3]));
        queuePage->setProperty("runtimeFolder",QUrl::fromLocalFile(arguments[4]));
        auto* queueList = root->findChild<QObject*>("queueTaskList");
        require(queueList, "Virtualized desktop task list was not created");
        root->setProperty("currentPage",1);
        auto* queueStart = root->findChild<QObject*>("queueStartButton");
        auto* queuePause = root->findChild<QObject*>("queuePauseButton");
        auto* queueResume = root->findChild<QObject*>("queueResumeButton");
        click(queueStart);
        waitUntil([&] { return queuePause && queuePause->property("visible").toBool() && queuePause->property("enabled").toBool(); }, "Queue pause control did not become available");
        click(queuePause);
        waitUntil([&] { return taskQueue.state() == "paused" || !taskQueue.busy(); }, "Queue did not acknowledge its cooperative pause");
        require(taskQueue.state() == "paused" && queueResume && queueResume->property("enabled").toBool(), "Queue pause was not reflected in the QML controls");
        click(queueResume);
        waitUntil([&] { return !taskQueue.busy(); }, "Desktop queue processing timed out");
        require(taskQueue.counts().value("completed").toInt() == 2, qPrintable(taskQueue.detail()));
        QEventLoop queuePaint; QTimer::singleShot(120,&queuePaint,&QEventLoop::quit); queuePaint.exec();
        require(window && window->grabWindow().save(arguments[6] + QStringLiteral(".queue.png")), "Queue page screenshot failed");
        QueueController reopened(QDir(QFileInfo(arguments[2]).absolutePath()).filePath("preferences-check.sqlite"));
        waitUntil([&] { return reopened.available() || reopened.state() == "error"; }, "Saved desktop preferences did not reload");
        require(reopened.available() && reopened.defaults().value("rate").toString() == "60" &&
                QFileInfo(reopened.defaults().value("model").toUrl().toLocalFile()).absoluteFilePath() == QFileInfo(arguments[3]).absoluteFilePath() &&
                QFileInfo(reopened.defaults().value("runtime").toUrl().toLocalFile()).absoluteFilePath() == QFileInfo(arguments[4]).absoluteFilePath() &&
                QFileInfo(reopened.defaults().value("output").toUrl().toLocalFile()).absoluteFilePath() == QFileInfo(queueFolder).absoluteFilePath(),
                "Local queue defaults were not persisted for the next launch");
        QFile report(arguments[7]); require(report.open(QIODevice::WriteOnly | QIODevice::Truncate), "Could not write workflow report");
        report.write(QString("{\"success\":true,\"outputFrames\":36,\"aiFrames\":%1,\"responsiveTicks\":%2,\"existingOutputProtected\":true,\"cancelledWithoutOutput\":true,\"queueCompleted\":2,\"recursiveBatch\":true,\"duplicateInputsDeduplicated\":true,\"unsupportedHdrReported\":true}\n").arg(aiFrames).arg(responsiveTicks).toUtf8());
        std::cout << "Desktop QML studio and persistent queue: recursive batch, collision-safe outputs, duplicate input and HDR rejection passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
