#include "studio_controller.hpp"
#include "queue_controller.hpp"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QQuickItem>
#include <QTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QSettings>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <algorithm>

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
void waitUntil(const std::function<bool()>& ready, const char* message, int timeoutMs = 90000) {
    QElapsedTimer elapsed; elapsed.start();
    while (!ready()) {
        require(elapsed.elapsed() < timeoutMs, message);
        QEventLoop loop; QTimer::singleShot(10, &loop, &QEventLoop::quit); loop.exec();
    }
}
void click(QObject* button, const char* name = "unnamed") {
    if (!button) throw std::runtime_error(std::string("Required QML control was not created: ") + name);
    if (!button->property("enabled").toBool()) throw std::runtime_error("QML control is disabled: " + button->objectName().toStdString());
    if (!QMetaObject::invokeMethod(button, "clicked", Qt::DirectConnection)) throw std::runtime_error("Could not activate QML control: " + button->objectName().toStdString());
}
QObject* findVisual(QQuickItem* parent, const QString& name) {
    if (!parent) return nullptr;
    if (parent->objectName() == name && parent->isVisible()) return parent;
    for (auto* child : parent->childItems()) if (auto* found = findVisual(child,name)) return found;
    return nullptr;
}
}
int main(int argc, char** argv) {
    QQuickStyle::setStyle("Material");
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
        click(button,"studio start");
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
        click(button,"studio existing-output start");
        require(studio.status() == "failed" && !studio.busy(), "Existing output was not protected");
        const auto cancelledOutput = arguments[2] + ".cancelled.mkv";
        studio.setOutput(QUrl::fromLocalFile(cancelledOutput));
        auto* rate = root->findChild<QObject*>("targetRate");
        require(rate && rate->setProperty("editText", "240"), "Target rate control is unavailable");
        click(button,"studio cancel workflow start");
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
        require(QFileInfo(queuePage->property("modelFolder").toUrl().toLocalFile()).absoluteFilePath() == QFileInfo(arguments[3]).absoluteFilePath() &&
                QFileInfo(queuePage->property("runtimeFolder").toUrl().toLocalFile()).absoluteFilePath() == QFileInfo(arguments[4]).absoluteFilePath(),
                "Queue page did not retain its selected local model/runtime directories");
        auto* modelPathField = root->findChild<QObject*>("queueModelPath");
        auto* runtimePathField = root->findChild<QObject*>("queueRuntimePath");
        require(modelPathField && !modelPathField->property("text").toString().isEmpty() && runtimePathField &&
                !runtimePathField->property("text").toString().isEmpty(), "Queue page did not display its selected local model/runtime paths");
        auto* queueList = root->findChild<QObject*>("queueTaskList");
        require(queueList, "Virtualized desktop task list was not created");
        root->setProperty("currentPage",1);
        auto* queueStart = root->findChild<QObject*>("queueStartButton");
        auto* queuePause = root->findChild<QObject*>("queuePauseButton");
        auto* queueResume = root->findChild<QObject*>("queueResumeButton");
        click(queueStart,"queue start initial batch");
        waitUntil([&] { return queuePause && queuePause->property("visible").toBool() && queuePause->property("enabled").toBool(); }, "Queue pause control did not become available");
        click(queuePause,"queue pause");
        waitUntil([&] { return taskQueue.state() == "paused" || !taskQueue.busy(); }, "Queue did not acknowledge its cooperative pause");
        require(taskQueue.state() == "paused" && queueResume && queueResume->property("enabled").toBool(), "Queue pause was not reflected in the QML controls");
        click(queueResume,"queue resume");
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
        const auto retrySource = QDir(QFileInfo(arguments[1]).absolutePath()).filePath("retry-source.mkv");
        require(QFile::copy(arguments[1],retrySource), "Could not create isolated retry fixture");
        taskQueue.addItems({QUrl::fromLocalFile(retrySource)},QUrl::fromLocalFile(queueFolder),"60",
            QUrl::fromLocalFile(arguments[3]),QUrl::fromLocalFile(arguments[4]),"libopenh264","mkv",false);
        waitUntil([&] { return !taskQueue.importing(); }, "Retry fixture import timed out");
        auto* rows = qobject_cast<TaskListModel*>(taskQueue.tasks());
        require(rows && rows->rowCount() == 3, "Retry fixture was not added to the task list");
        const auto retryOutput = rows->index(2,0).data(TaskListModel::OutputPath).toString();
        QFile collision(retryOutput);
        require(collision.open(QIODevice::WriteOnly) && collision.write("owned-existing-output") > 0, "Could not prepare a deterministic output collision");
        collision.close();
        click(queueStart,"queue start collision failure");
        waitUntil([&] { return !taskQueue.busy() && taskQueue.counts().value("failed").toInt() == 1; }, "Deterministic queue output failure was not recorded");
        require(QFile::remove(retryOutput), "Could not remove the test-owned collision file");
        const bool positionedAtEnd = QMetaObject::invokeMethod(queueList,"positionViewAtEnd",Qt::DirectConnection);
        queueList->setProperty("contentY",std::max(0.0,queueList->property("contentHeight").toReal() - queueList->property("height").toReal()));
        QEventLoop retryPaint; QTimer::singleShot(120,&retryPaint,&QEventLoop::quit); retryPaint.exec();
        auto* retryButton = findVisual(window->contentItem(),"retryTaskButton");
        if (!retryButton) throw std::runtime_error(std::string("Failed retry row was not instantiated in ListView; positionViewAtEnd=") + (positionedAtEnd ? "true" : "false"));
        require(retryButton->property("enabled").toBool(), "Failed task retry control is disabled");
        const auto retryId = rows->index(2,0).data(TaskListModel::TaskId).toString();
        taskQueue.retryTask(retryId);
        waitUntil([&] { return taskQueue.counts().value("queued").toInt() == 1 && taskQueue.counts().value("failed").toInt() == 0; }, "Failed task was not retried from its saved configuration",10000);
        click(queueStart,"queue start after retry");
        waitUntil([&] { return !taskQueue.busy() && taskQueue.counts().value("completed").toInt() == 3; }, "Retried task did not complete");
        const auto cancelSource = QDir(QFileInfo(arguments[1]).absolutePath()).filePath("cancel-source.mkv");
        require(QFile::copy(arguments[1],cancelSource), "Could not create isolated queued-cancel fixture");
        taskQueue.addItems({QUrl::fromLocalFile(cancelSource)},QUrl::fromLocalFile(queueFolder),"60",
            QUrl::fromLocalFile(arguments[3]),QUrl::fromLocalFile(arguments[4]),"libopenh264","mkv",false);
        waitUntil([&] { return !taskQueue.importing(); }, "Queued-cancel fixture import timed out");
        require(taskQueue.counts().value("queued").toInt() == 1, "Queued-cancel fixture was not pending");
        QMetaObject::invokeMethod(queueList,"positionViewAtEnd",Qt::DirectConnection);
        queueList->setProperty("contentY",std::max(0.0,queueList->property("contentHeight").toReal() - queueList->property("height").toReal()));
        QEventLoop cancelPaint; QTimer::singleShot(120,&cancelPaint,&QEventLoop::quit); cancelPaint.exec();
        auto* cancelButton = findVisual(window->contentItem(),"cancelTaskButton");
        require(cancelButton && cancelButton->property("enabled").toBool(), "Queued cancel control is unavailable in the task row");
        const auto cancelId = rows->index(3,0).data(TaskListModel::TaskId).toString();
        taskQueue.cancelTask(cancelId);
        waitUntil([&] { return taskQueue.counts().value("queued").toInt() == 0 && taskQueue.counts().value("cancelled").toInt() == 1; }, "Queued task cancellation was not persisted",10000);
        require(QFileInfo(queuePage->property("modelFolder").toUrl().toLocalFile()).absoluteFilePath() == QFileInfo(arguments[3]).absoluteFilePath() &&
                QFileInfo(queuePage->property("runtimeFolder").toUrl().toLocalFile()).absoluteFilePath() == QFileInfo(arguments[4]).absoluteFilePath(),
                "Queue settings changed after task operations");
        QEventLoop finalPaint; QTimer::singleShot(120,&finalPaint,&QEventLoop::quit); finalPaint.exec();
        require(window->grabWindow().save(arguments[6] + QStringLiteral(".queue.png")), "Final queue page screenshot failed");
        root->setProperty("currentPage",2);
        auto* modelCenter = root->findChild<QObject*>("modelCenterPage");
        require(modelCenter, "Offline model center page was not loaded");
        waitUntil([&] { return !taskQueue.verifyingModels() && !taskQueue.models().isEmpty(); }, "Local model center did not validate the saved default model",20000);
        require(taskQueue.models().size() == 1 && taskQueue.models().front().toMap().value("valid").toBool() &&
                taskQueue.models().front().toMap().value("id").toString() == "rife-4.25-fp32-sdr8",
                "Model center did not verify the active local RIFE pack");
        const auto localModelCopy = QDir(QFileInfo(arguments[1]).absolutePath()).filePath("local-model-copy");
        taskQueue.addLocalModel(QUrl::fromLocalFile(localModelCopy));
        waitUntil([&] { return !taskQueue.verifyingModels() && taskQueue.models().size() == 2; }, "Local model import or integrity verification failed",20000);
        const auto copiedModel = QUrl::fromLocalFile(localModelCopy);
        auto* setDefaultButton = findVisual(window->contentItem(),"setDefaultModelButton");
        require(setDefaultButton && setDefaultButton->property("enabled").toBool(), "A verified alternative model cannot be selected in the model center");
        click(setDefaultButton,"set verified default model");
        waitUntil([&] { return QFileInfo(taskQueue.defaults().value("model").toUrl().toLocalFile()).absoluteFilePath() == QFileInfo(localModelCopy).absoluteFilePath(); },
            "Selected local model was not saved as the next-run default",10000);
        require(QFileInfo(root->property("modelFolder").toUrl().toLocalFile()).absoluteFilePath() == QFileInfo(localModelCopy).absoluteFilePath(),
                "Studio did not receive the selected verified local model");
        QueueController catalogReload(QDir(QFileInfo(arguments[2]).absolutePath()).filePath("model-catalog-reload.sqlite"));
        waitUntil([&] { return catalogReload.available() || catalogReload.state() == "error"; }, "Model catalog persistence check did not initialize");
        require(catalogReload.available(), qPrintable(catalogReload.detail()));
        catalogReload.refreshModels();
        waitUntil([&] { return !catalogReload.verifyingModels(); }, "Saved model catalog re-verification did not finish",20000);
        if (catalogReload.models().size() != 2) {
            QSettings persisted(QDir(QFileInfo(arguments[2]).absolutePath()).filePath("preferences.ini"),QSettings::IniFormat);
            persisted.sync();
            const auto paths = persisted.value("modelCatalog/paths").toStringList();
            throw std::runtime_error(QString("Saved model catalog did not reload: entries=%1 paths=%2 detail=%3 selected=%4")
                .arg(catalogReload.models().size()).arg(paths.size()).arg(catalogReload.detail())
                .arg(catalogReload.defaults().value("model").toUrl().toLocalFile()).toStdString());
        }
        require(QFileInfo(catalogReload.defaults().value("model").toUrl().toLocalFile()).absoluteFilePath() == QFileInfo(localModelCopy).absoluteFilePath(),
                "Selected model default did not persist across controller restart");
        const auto corruptModel = QDir(QFileInfo(arguments[1]).absolutePath()).filePath("corrupt-model-copy");
        catalogReload.addLocalModel(QUrl::fromLocalFile(corruptModel));
        waitUntil([&] { return !catalogReload.verifyingModels(); }, "Corrupt model rejection did not finish",20000);
        require(catalogReload.state() == "error" && catalogReload.detail().contains("SHA-256 mismatch") && catalogReload.models().size() == 2,
                "Model center accepted a package whose ONNX hash no longer matches its manifest");
        catalogReload.refreshModels();
        waitUntil([&] { return !catalogReload.verifyingModels() && catalogReload.models().size() == 2; }, "Model catalog did not recover after a rejected package",20000);
        catalogReload.removeLocalModel(QUrl::fromLocalFile(arguments[3]));
        waitUntil([&] { return !catalogReload.verifyingModels() && catalogReload.models().size() == 1; }, "Removing an unused local model did not update the catalog",20000);
        QueueController finalReload(QDir(QFileInfo(arguments[2]).absolutePath()).filePath("model-catalog-final.sqlite"));
        waitUntil([&] { return finalReload.available() || finalReload.state() == "error"; }, "Final model catalog persistence check did not initialize");
        require(finalReload.available(), qPrintable(finalReload.detail()));
        finalReload.refreshModels();
        waitUntil([&] { return !finalReload.verifyingModels() && finalReload.models().size() == 1; }, "Removed model reappeared after controller restart",20000);
        require(QFileInfo(finalReload.defaults().value("model").toUrl().toLocalFile()).absoluteFilePath() == QFileInfo(localModelCopy).absoluteFilePath(),
                "Removing another pack changed the selected default model");
        QEventLoop modelPaint; QTimer::singleShot(120,&modelPaint,&QEventLoop::quit); modelPaint.exec();
        require(window->grabWindow().save(arguments[6] + QStringLiteral(".models.png")), "Local model center screenshot failed");
        QFile report(arguments[7]); require(report.open(QIODevice::WriteOnly | QIODevice::Truncate), "Could not write workflow report");
        report.write(QString("{\"success\":true,\"outputFrames\":36,\"aiFrames\":%1,\"responsiveTicks\":%2,\"existingOutputProtected\":true,\"cancelledWithoutOutput\":true,\"queueCompleted\":3,\"recursiveBatch\":true,\"duplicateInputsDeduplicated\":true,\"unsupportedHdrReported\":true,\"controllerRetry\":true,\"controllerQueuedCancel\":true,\"localModelShaValidated\":true,\"invalidModelRejected\":true,\"modelRemovalPersisted\":true,\"modelCatalogEntries\":1,\"defaultModelReloaded\":true}\n").arg(aiFrames).arg(responsiveTicks).toUtf8());
        std::cout << "Desktop QML studio, persistent queue and offline model catalog workflows passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
