#include "studio_controller.hpp"
#include "queue_controller.hpp"
#include "encoding_settings.hpp"
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
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <algorithm>
#include <cmath>

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
        const auto h264Settings = rifelove::desktop::makeVideoEncodingSettings(QStringLiteral("libopenh264"),
            {{"bitrateKbps",2500},{"qualityMode","bitrate"},{"quality",32},{"gopSize",60}});
        require(h264Settings.bitRate == 2500000 && h264Settings.gopSize == 60 && h264Settings.encoderOptions.empty(),
            "H.264 target bitrate/GOP did not map to the native encoder settings");
        const auto nvencSettings = rifelove::desktop::makeVideoEncodingSettings(QStringLiteral("h264_nvenc"),
            {{"qualityMode","cq"},{"quality",20},{"gopSize",60}});
        require(nvencSettings.bitRate == 0 && nvencSettings.encoderOptions.at("rc") == "vbr" &&
                nvencSettings.encoderOptions.at("cq") == "20",
            "NVENC constant-quality settings did not map to VBR + CQ");
        const auto hevcSettings = rifelove::desktop::makeVideoEncodingSettings(QStringLiteral("libkvazaar"),
            {{"bitrateKbps",8000},{"qualityMode","qp"},{"quality",28},{"gopSize",48}});
        require(hevcSettings.bitRate == 0 && hevcSettings.encoderOptions.at("kvazaar-params") == "qp=28",
            "HEVC constant-QP settings did not map to Kvazaar options");
        const auto av1Settings = rifelove::desktop::makeVideoEncodingSettings(QStringLiteral("libaom-av1"),
            {{"bitrateKbps",8000},{"qualityMode","crf"},{"quality",35},{"gopSize",48}});
        require(av1Settings.bitRate == 0 && av1Settings.encoderOptions.at("crf") == "35",
            "AV1 constant-quality settings did not map to libaom options");
        bool rejectedUnsupportedQualityMode = false;
        try {
            (void)rifelove::desktop::makeVideoEncodingSettings(QStringLiteral("libopenh264"),
                {{"bitrateKbps",8000},{"qualityMode","crf"},{"quality",35},{"gopSize",48}});
        } catch (const std::invalid_argument&) { rejectedUnsupportedQualityMode = true; }
        require(rejectedUnsupportedQualityMode, "The desktop accepted a quality mode the selected H.264 encoder does not support");
        bool rejectedNvencQualityForSoftwareH264 = false;
        try {
            (void)rifelove::desktop::makeVideoEncodingSettings(QStringLiteral("libopenh264"),
                {{"bitrateKbps",8000},{"qualityMode","cq"},{"quality",20},{"gopSize",48}});
        } catch (const std::invalid_argument&) { rejectedNvencQualityForSoftwareH264 = true; }
        require(rejectedNvencQualityForSoftwareH264, "The desktop accepted NVENC CQ for software H.264");
        const auto arguments = app.arguments();
        require(arguments.size() == 8, "Expected source, output, pack, runtime, QML, screenshot and report paths");
        StudioController studio;
        const auto queueFolder = QDir(QFileInfo(arguments[2]).absolutePath()).filePath("queue-output");
        require(QDir().mkpath(queueFolder), "Could not create isolated queue output folder");
        const auto queueDatabase = QDir(QFileInfo(arguments[2]).absolutePath()).filePath("desktop-queue.sqlite");
        QSettings initialSettings(QFileInfo(queueDatabase).dir().filePath("preferences.ini"),QSettings::IniFormat);
        initialSettings.setFallbacksEnabled(false);
        initialSettings.setValue("rate","240"); initialSettings.setValue("encoder","libopenh264"); initialSettings.setValue("container","mp4");
        initialSettings.setValue("encoding/bitrateKbps",4500); initialSettings.setValue("encoding/qualityMode","bitrate");
        initialSettings.setValue("encoding/quality",32); initialSettings.setValue("encoding/gopSize",60);
        initialSettings.setValue("model",QUrl::fromLocalFile(arguments[3])); initialSettings.setValue("runtime",QUrl::fromLocalFile(arguments[4]));
        initialSettings.setValue("modelCatalog/paths",QStringList{QFileInfo(arguments[3]).absoluteFilePath()});
        initialSettings.sync(); require(initialSettings.status() == QSettings::NoError, "Could not prepare saved first-run preferences");
        QueueController taskQueue(queueDatabase);
        waitUntil([&] { return taskQueue.available() || taskQueue.state() == "error"; }, "Local desktop queue did not initialize");
        require(taskQueue.available(), qPrintable(taskQueue.detail()));
        waitUntil([&] { return taskQueue.defaults().value("rate").toString() == "240"; }, "Saved local defaults did not load");
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("studio", &studio);
        engine.rootContext()->setContextProperty("taskQueue", &taskQueue);
        engine.rootContext()->setContextProperty("processingAvailable", true);
        engine.load(QUrl::fromLocalFile(arguments[5]));
        require(!engine.rootObjects().isEmpty(), "QML application did not load");
        auto* root = engine.rootObjects().front();
        auto* homePage = root->findChild<QObject*>("homePage");
        auto* homeStudio = root->findChild<QObject*>("homeStudioButton");
        auto* homeGpuValue = root->findChild<QObject*>("homeGpuNameValue");
        auto* homeModelValue = root->findChild<QObject*>("homeModelNameValue");
        auto* homeQueueCount = root->findChild<QObject*>("homeQueueCountValue");
        require(homePage && homeStudio && root->property("currentPage").toInt() == 0,
            "The desktop did not open its Home workspace first");
        require(homeGpuValue && homeModelValue && homeQueueCount && studio.deviceInfo().value("available").toBool() &&
                homeGpuValue->property("text").toString() == studio.deviceInfo().value("name").toString() &&
                homePage->property("deviceInfo").toMap().value("name").toString() == studio.deviceInfo().value("name").toString(),
            "Home did not display the detected local NVIDIA GPU");
        click(homeStudio,"Home open Studio");
        require(root->property("currentPage").toInt() == 1, "Home Studio shortcut did not open the interpolation workspace");
        root->setProperty("currentPage",0);
        auto* button = root->findChild<QObject*>("startButton");
        auto* statusLabel = root->findChild<QObject*>("studioStatusLabel");
        auto* targetRate = root->findChild<QObject*>("targetRate");
        auto* studioEncoder = root->findChild<QObject*>("studioEncoder");
        require(button && !button->property("enabled").toBool(), "Start should be disabled without input/model");
        require(statusLabel, "Studio status label is unavailable");
        require(targetRate && targetRate->property("editText").toString() == QStringLiteral("240") &&
                studioEncoder && studioEncoder->property("currentIndex").toInt() == 1,
                "Studio did not restore the saved target FPS and encoder");
        auto* studioBitrate = root->findChild<QObject*>("studioBitrateKbps");
        auto* studioGop = root->findChild<QObject*>("studioGopSize");
        auto* studioQualityMode = root->findChild<QObject*>("studioQualityMode");
        auto* studioQuality = root->findChild<QObject*>("studioQuality");
        require(studioBitrate && studioBitrate->property("value").toInt() == 4500 && studioGop && studioGop->property("value").toInt() == 60 &&
                studioQualityMode && studioQualityMode->property("currentIndex").toInt() == 0 && studioQuality,
                "Studio did not restore saved advanced encoder settings");
        // Confirm restoration above, then keep the synthetic workflow short.
        require(targetRate->setProperty("editText","60"), "Could not set the synthetic workflow FPS");
        root->setProperty("modelFolder", QUrl::fromLocalFile(arguments[3]));
        root->setProperty("runtimeFolder", QUrl::fromLocalFile(arguments[4]));
        studio.openSource(QUrl::fromLocalFile(arguments[1]));
        require(studio.busy(), "Inspection should run asynchronously");
        // Reentrant import is ignored while a worker owns the operation.
        studio.openSource(QUrl("https://example.invalid/video.mkv"));
        wait(studio);
        require(studio.status() == "inspected", "Source inspection failed");
        require(studio.media().value("width").toInt() == 96 && studio.media().value("height").toInt() == 64, "Wrong imported dimensions");
        require(studio.media().value("color").toString().contains("bt470bg") && studio.media().value("color").toString().contains("smpte170m"), "Legacy SDR color tags were not detected");
        const auto encoderCapabilities = studio.media().value("encoders").toMap();
        const bool preflightSoftwareFallback = !studio.media().value("nvencContextOpened").toBool() &&
            studio.media().value("softwareH264ContextOpened").toBool();
        require(encoderCapabilities.contains("h264_nvenc") && encoderCapabilities.contains("libopenh264") &&
                encoderCapabilities.contains("libkvazaar") && encoderCapabilities.contains("libaom-av1") &&
                studio.media().contains("nvencContextOpened") && studio.media().contains("softwareH264ContextOpened"),
            "Studio did not report runtime encoder initialization checks");
        if (!studio.media().value("nvencContextOpened").toBool() && studio.media().value("softwareH264ContextOpened").toBool())
            require(studio.detail().contains(QStringLiteral("软件 H.264")) && !studio.detail().contains(QStringLiteral("Function not implemented")),
                "Studio did not explain its preflight NVENC fallback in user-facing language");
        require(button->property("enabled").toBool() && statusLabel->property("text").toString() == QStringLiteral("可以开始"), "Supported BT.601 source readiness disagrees with the start control");
        const auto av1Probe = encoderCapabilities.value(QStringLiteral("libaom-av1")).toMap();
        if (!av1Probe.value(QStringLiteral("contextOpened")).toBool()) {
            require(studioEncoder->setProperty("currentIndex",3) && !button->property("enabled").toBool() &&
                    statusLabel->property("text").toString() == QStringLiteral("所选编码器不可用"),
                "Studio allowed processing with an encoder that failed its source-profile initialization check");
            require(studioEncoder->setProperty("currentIndex",0) && button->property("enabled").toBool(),
                "Studio did not restore automatic encoding after a rejected encoder selection");
        }
        const auto unsupportedHdr = QDir(QFileInfo(arguments[1]).absolutePath()).filePath("queue-input/unsupported-hdr.mkv");
        studio.openSource(QUrl::fromLocalFile(unsupportedHdr)); wait(studio);
        require(!studio.media().value("supportedColor").toBool() && !button->property("enabled").toBool() &&
                statusLabel->property("text").toString() == QStringLiteral("色彩格式暂不支持"), "Unsupported HDR status disagrees with the start control");
        studio.openSource(QUrl::fromLocalFile(arguments[1])); wait(studio);
        require(button->property("enabled").toBool() && statusLabel->property("text").toString() == QStringLiteral("可以开始"), "Studio did not recover readiness after selecting a supported source");
        studio.setOutput(QUrl::fromLocalFile(arguments[2]));
        require(studioEncoder->setProperty("currentIndex",0), "Could not select automatic hardware-first encoding for fallback verification");
        auto* progressLabel = root->findChild<QObject*>("studioProgressLabel");
        require(progressLabel, "Studio progress estimate label is unavailable");
        auto* timingLabel = root->findChild<QObject*>("studioTimingBreakdownLabel");
        require(timingLabel, "Studio stage timing label is unavailable");
        int responsiveTicks = 0;
        bool sawFallbackNotice = false;
        bool sawRawFallbackError = false;
        bool sawModeNotice = false;
        bool sawEta = false;
        QObject::connect(&studio, &StudioController::changed, [&] {
            if (studio.status() == "encoder_fallback" || studio.status() == "processing") {
                sawFallbackNotice = sawFallbackNotice || studio.detail().contains(QStringLiteral("软件 H.264"));
                sawRawFallbackError = sawRawFallbackError || studio.detail().contains(QStringLiteral("Function not implemented"));
            }
            if (studio.status() == "processing") {
                sawEta = sawEta || progressLabel->property("text").toString().contains(QStringLiteral("剩余约"));
                sawModeNotice = sawModeNotice || progressLabel->property("text").toString().contains(QStringLiteral("CPU H.264 编码 · CUDA RIFE 插帧"));
            }
        });
        QTimer heartbeat; heartbeat.setInterval(5);
        QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++responsiveTicks; });
        heartbeat.start();
        root->setProperty("currentPage",1);
        click(button,"studio start");
        require(studio.busy(), "QML start did not submit asynchronous work");
        if (preflightSoftwareFallback)
            require(studio.progress().value("encoderFallbackReason").toString().size() > 0 &&
                    studio.detail().contains(QStringLiteral("软件 H.264")) && studio.detail().contains(QStringLiteral("CPU")) &&
                    studio.detail().contains(QStringLiteral("CUDA")),
                "Studio did not explain that software encoding and CUDA interpolation are both active");
        require(studio.progress().value("estimatedFrames").toULongLong() == 36,
            "Studio did not estimate the 60 FPS output frame count from the probed source duration");
        require(progressLabel && progressLabel->property("text").toString().contains(QStringLiteral("约 36 帧")),
            "Studio did not display its estimated frame total");
        wait(studio);
        require(studio.status() == "completed", qPrintable(studio.detail()));
        require(QFileInfo(arguments[2]).isFile(), "Completed output was not published");
        require(studio.progress().value("frames").toULongLong() == 36, "Wrong output frame count");
        require(studio.progress().value("aiFrames").toULongLong() > 0, "Desktop did not generate AI frames");
        const auto timingBreakdown = studio.progress().value("timings").toMap();
        require(timingBreakdown.contains("inferenceWaitSeconds") && timingBreakdown.contains("frameEncodeSeconds") &&
                timingBreakdown.contains("outputValidationSeconds") && timingBreakdown.value("inferenceWaitSeconds").toDouble() > 0.0,
            "Desktop did not publish per-stage processing timings");
        require(timingLabel->property("visible").toBool() && timingLabel->property("text").toString().contains(QStringLiteral("累计耗时")),
            "Studio did not display the measured stage timings");
        if (!studio.progress().value("encoderFallbackReason").toString().isEmpty())
            require(sawFallbackNotice && !sawRawFallbackError && studio.progress().value("encoder").toString() == QStringLiteral("libopenh264"),
                "Recoverable NVENC fallback was presented as a processing error or did not use software H.264");
        if (!studio.progress().value("encoderFallbackReason").toString().isEmpty())
            require(sawModeNotice,"Studio did not label software encoding and CUDA interpolation in the progress display");
        require(sawEta, "Studio did not show a remaining-time estimate while generating frames");
        require(responsiveTicks > 5, "UI event loop did not stay responsive during processing");
        const auto aiFrames = studio.progress().value("aiFrames").toULongLong();
        // Run a real model-backed AV1 CRF job through the newly exposed desktop controls.
        auto* advancedToggle = root->findChild<QObject*>("studioAdvancedEncodingToggle");
        require(advancedToggle, "Advanced encoding controls are not present in Studio");
        click(advancedToggle,"open advanced encoding controls");
        require(studioEncoder->setProperty("currentIndex",3) && studioQualityMode->setProperty("currentIndex",1) &&
                studioQuality->setProperty("value",35) && studioGop->setProperty("value",48),
            "Could not select AV1 CRF controls");
        require(QMetaObject::invokeMethod(root,"persistLocalDefaults",Qt::DirectConnection),
            "Studio advanced settings could not be saved through the local preferences service");
        waitUntil([&] { return taskQueue.defaults().value("encoder").toString() == QStringLiteral("libaom-av1") &&
                taskQueue.defaults().value("qualityMode").toString() == QStringLiteral("crf") &&
                taskQueue.defaults().value("quality").toInt() == 35; },
            "AV1 CRF settings were not persisted as local defaults",10000);
        const auto av1Output = QDir(QFileInfo(arguments[1]).absolutePath()).filePath("desktop-av1-crf.mkv");
        studio.setOutput(QUrl::fromLocalFile(av1Output));
        click(button,"studio AV1 CRF start"); wait(studio);
        require(studio.status() == "completed" && QFileInfo(av1Output).isFile() &&
                studio.progress().value("encoder").toString() == QStringLiteral("libaom-av1"),
            "Studio did not complete its AV1 constant-quality workflow");
        require(studioEncoder->setProperty("currentIndex",2) && studioQualityMode->setProperty("currentIndex",1) &&
                studioQuality->setProperty("value",28) && studioGop->setProperty("value",48),
            "Could not select HEVC constant-QP controls");
        const auto hevcOutput = QDir(QFileInfo(arguments[1]).absolutePath()).filePath("desktop-hevc-qp.mkv");
        studio.setOutput(QUrl::fromLocalFile(hevcOutput));
        click(button,"studio HEVC QP start"); wait(studio);
        require(studio.status() == "completed" && QFileInfo(hevcOutput).isFile() &&
                studio.progress().value("encoder").toString() == QStringLiteral("libkvazaar"),
            "Studio did not complete its HEVC constant-QP workflow");
        studio.setOutput(QUrl::fromLocalFile(av1Output));
        // Exercise the local comparison viewer with the actual source/output pair.
        auto* comparisonButton = root->findChild<QObject*>("comparisonButton");
        click(comparisonButton,"comparison page navigation");
        waitUntil([&] { return root->findChild<QObject*>("comparisonPage") != nullptr; }, "Comparison viewer page did not load");
        auto* comparisonPage = root->findChild<QObject*>("comparisonPage");
        waitUntil([&] { return comparisonPage->property("originalReady").toBool() && comparisonPage->property("processedReady").toBool(); },
            "Comparison viewer could not open the real source and generated output",30000);
        const double comparisonRate = comparisonPage->property("frameRate").toReal();
        require(comparisonRate > 0.0 && comparisonRate < 1000.0, "Comparison viewer did not resolve a usable output frame rate");
        auto* nextFrame = root->findChild<QObject*>("comparisonNextFrame");
        const double beforeStep = comparisonPage->property("originalPosition").toReal();
        click(nextFrame,"comparison one-frame step");
        waitUntil([&] { return comparisonPage->property("originalPosition").toReal() > beforeStep &&
                comparisonPage->property("processedPosition").toReal() > beforeStep; },
            "Comparison frame-step did not seek both local videos",5000);
        const double steppedOriginal = comparisonPage->property("originalPosition").toReal();
        const double steppedProcessed = comparisonPage->property("processedPosition").toReal();
        require(std::abs(steppedOriginal - steppedProcessed) <= 100.0, "Comparison frame-step left source and output out of sync");
        auto* playback = root->findChild<QObject*>("comparisonPlayback");
        click(playback,"comparison playback start");
        waitUntil([&] { return comparisonPage->property("playbackActive").toBool() &&
                comparisonPage->property("originalPosition").toReal() >= steppedOriginal + 30.0 &&
                comparisonPage->property("processedPosition").toReal() >= steppedProcessed + 30.0; },
            "Synchronized comparison playback did not advance",5000);
        require(std::abs(comparisonPage->property("originalPosition").toReal() -
                comparisonPage->property("processedPosition").toReal()) <= 250.0,
            "Comparison playback drift exceeded the tested tolerance");
        click(playback,"comparison playback pause");
        waitUntil([&] { return !comparisonPage->property("playbackActive").toBool(); }, "Comparison playback did not pause",5000);
        QEventLoop comparisonPaint; QTimer::singleShot(100,&comparisonPaint,&QEventLoop::quit); comparisonPaint.exec();
        auto* comparisonWindow = qobject_cast<QQuickWindow*>(root);
        require(comparisonWindow && comparisonWindow->grabWindow().save(arguments[6] + QStringLiteral(".comparison.png")),
            "Comparison viewer screenshot failed");
        root->setProperty("currentPage",1);
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
        const QVariantMap queueEncoding{{"bitrateKbps",3200},{"qualityMode","bitrate"},{"quality",32},{"gopSize",48}};
        taskQueue.addItems({QUrl::fromLocalFile(batchFolder),QUrl::fromLocalFile(arguments[1]),QUrl::fromLocalFile(arguments[1])},QUrl::fromLocalFile(queueFolder),"60",
            QUrl::fromLocalFile(arguments[3]),QUrl::fromLocalFile(arguments[4]),"libopenh264","mkv",queueEncoding,true);
        waitUntil([&] { return !taskQueue.importing(); }, "Desktop queue import timed out");
        require(taskQueue.tasks()->rowCount() == 2, qPrintable(taskQueue.detail()));
        auto* importedRows = qobject_cast<TaskListModel*>(taskQueue.tasks());
        require(importedRows && importedRows->index(0,0).data(TaskListModel::EstimatedFrames).toULongLong() == 36,
            "Persistent queue did not retain the estimated frame count for its imported task");
        const auto recentTasks = taskQueue.recentTasks();
        require(recentTasks.size() == 2 && recentTasks.front().toMap().value("taskId").toString() ==
                importedRows->index(1,0).data(TaskListModel::TaskId).toString(),
            "Home recent tasks were not ordered newest first from the persistent queue");
        require(homeQueueCount->property("text").toString().contains(QStringLiteral("2")),
            "Home queue summary did not reflect the persistent task count");
        require(taskQueue.detail().contains(QStringLiteral("跳过 1 个不支持的文件")), "Unsupported HDR input was not reported as skipped");
        root->setProperty("currentPage",0);
        QEventLoop homePaint; QTimer::singleShot(100,&homePaint,&QEventLoop::quit); homePaint.exec();
        require(homePage->property("recentTasks").toList().size() == 2, "Home did not bind to the recent persistent queue entries");
        auto* homeWindow = qobject_cast<QQuickWindow*>(root);
        require(homeWindow && homeWindow->grabWindow().save(arguments[6] + QStringLiteral(".home.png")),
            "Home workspace screenshot failed");
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
        root->setProperty("currentPage",2);
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
                reopened.defaults().value("bitrateKbps").toInt() == 3200 && reopened.defaults().value("gopSize").toInt() == 48 &&
                QFileInfo(reopened.defaults().value("model").toUrl().toLocalFile()).absoluteFilePath() == QFileInfo(arguments[3]).absoluteFilePath() &&
                QFileInfo(reopened.defaults().value("runtime").toUrl().toLocalFile()).absoluteFilePath() == QFileInfo(arguments[4]).absoluteFilePath() &&
                QFileInfo(reopened.defaults().value("output").toUrl().toLocalFile()).absoluteFilePath() == QFileInfo(queueFolder).absoluteFilePath(),
                "Local queue defaults were not persisted for the next launch");
        const auto retrySource = QDir(QFileInfo(arguments[1]).absolutePath()).filePath("retry-source.mkv");
        require(QFile::copy(arguments[1],retrySource), "Could not create isolated retry fixture");
        taskQueue.addItems({QUrl::fromLocalFile(retrySource)},QUrl::fromLocalFile(queueFolder),"60",
            QUrl::fromLocalFile(arguments[3]),QUrl::fromLocalFile(arguments[4]),"libopenh264","mkv",queueEncoding,false);
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
            QUrl::fromLocalFile(arguments[3]),QUrl::fromLocalFile(arguments[4]),"libopenh264","mkv",queueEncoding,false);
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
        root->setProperty("currentPage",3);
        auto* modelCenter = root->findChild<QObject*>("modelCenterPage");
        require(modelCenter, "Offline model center page was not loaded");
        waitUntil([&] { return !taskQueue.verifyingModels() && !taskQueue.models().isEmpty(); }, "Local model center did not validate the saved default model",20000);
        QFile activeModelDescriptor(QDir(arguments[3]).filePath(QStringLiteral("model.json")));
        require(activeModelDescriptor.open(QIODevice::ReadOnly), "Could not read the active model descriptor in the integration fixture");
        const auto expectedModelId = QJsonDocument::fromJson(activeModelDescriptor.readAll()).object().value(QStringLiteral("id")).toString();
        require(taskQueue.models().size() == 1 && taskQueue.models().front().toMap().value("valid").toBool() &&
                !expectedModelId.isEmpty() && taskQueue.models().front().toMap().value("id").toString() == expectedModelId,
                "Model center did not verify the active local RIFE pack");
        const auto registeredModel = taskQueue.models().front().toMap();
        require(registeredModel.value("schemaVersion").toInt() == 2 &&
                registeredModel.value("metadataIntegrityVerified").toBool() &&
                registeredModel.value("licenseId").toString() == "MIT" &&
                registeredModel.value("sourceRepository").toString() == "https://github.com/hzwer/Practical-RIFE" &&
                registeredModel.value("numericalProfile").toString() == "sdr8" &&
                registeredModel.value("validatedBackends").toStringList().contains("onnxruntime_cpu") &&
                !registeredModel.value("hdrQualified").toBool(),
            "Model center did not expose verified v2 provenance, license and capability metadata");
        const auto localModelCopy = QDir(QFileInfo(arguments[1]).absolutePath()).filePath("local-model-copy");
        taskQueue.addLocalModel(QUrl::fromLocalFile(localModelCopy));
        waitUntil([&] { return !taskQueue.verifyingModels() && taskQueue.models().size() == 2; }, "Local model import or integrity verification failed",20000);
        const auto copiedModel = QUrl::fromLocalFile(localModelCopy);
        auto* setDefaultButton = findVisual(window->contentItem(),"setDefaultModelButton");
        require(setDefaultButton && setDefaultButton->property("enabled").toBool(), "A verified alternative model cannot be selected in the model center");
        click(setDefaultButton,"set verified default model");
        waitUntil([&] { return QFileInfo(taskQueue.defaults().value("model").toUrl().toLocalFile()).absoluteFilePath() == QFileInfo(localModelCopy).absoluteFilePath(); },
            "Selected local model was not saved as the next-run default",10000);
        QString copiedDisplayName;
        for (const auto& model : taskQueue.models()) {
            const auto entry = model.toMap();
            if (QFileInfo(entry.value("directory").toString()).absoluteFilePath() == QFileInfo(localModelCopy).absoluteFilePath())
                copiedDisplayName = entry.value("displayName").toString();
        }
        require(!copiedDisplayName.isEmpty(), "Verified local model has no display name for the Home workspace");
        const auto homeModelMessage = QStringLiteral("Home workspace did not update after selecting a verified local model: expected '%1', actual '%2'")
            .arg(copiedDisplayName,homeModelValue->property("text").toString() + QStringLiteral("; root='%1'; selected='%2'; models=%3")
                .arg(root->property("selectedModelDisplayName").toString(),taskQueue.defaults().value("model").toUrl().toLocalFile())
                .arg(taskQueue.models().size())).toUtf8();
        waitUntil([&] { return homeModelValue->property("text").toString() == copiedDisplayName; },
            homeModelMessage.constData(),10000);
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
