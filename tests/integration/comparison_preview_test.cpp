#include "comparison_preview.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QGuiApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void waitForPreview(ComparisonPreviewController& controller) {
    if (!controller.busy()) return;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&controller,&ComparisonPreviewController::changed,&loop,[&] {
        if (!controller.busy()) loop.quit();
    });
    QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);
    timeout.start(15000);
    loop.exec();
    require(!timeout.isActive() || !controller.busy(),"Exact frame preview timed out");
    require(!controller.busy(),"Exact frame preview worker remained busy after the timeout");
    require(controller.errorString().isEmpty(),"Exact frame preview returned an error");
}

void waitUntil(const std::function<bool()>& condition, const char* message) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition()) {
        require(elapsed.elapsed() < 15000,message);
        QEventLoop loop;
        QTimer::singleShot(10,&loop,&QEventLoop::quit);
        loop.exec();
    }
}

QObject* findVisualObject(QQuickItem* parent, const QString& name) {
    if (!parent) return nullptr;
    for (auto* child : parent->childItems()) {
        if (child->objectName() == name) return child;
        if (auto* found = findVisualObject(child,name)) return found;
    }
    return nullptr;
}

void createVideo(const QString& path, const QString& rate) {
    const QStringList arguments{
        QStringLiteral("-y"),QStringLiteral("-v"),QStringLiteral("error"),
        QStringLiteral("-f"),QStringLiteral("lavfi"),QStringLiteral("-i"),
        QStringLiteral("testsrc2=size=96x64:rate=%1:duration=1").arg(rate),
        QStringLiteral("-vf"),QStringLiteral("setparams=range=tv:color_primaries=bt709:color_trc=bt709:colorspace=bt709"),
        QStringLiteral("-color_primaries"),QStringLiteral("bt709"),QStringLiteral("-color_trc"),QStringLiteral("bt709"),
        QStringLiteral("-colorspace"),QStringLiteral("bt709"),QStringLiteral("-color_range"),QStringLiteral("tv"),
        QStringLiteral("-c:v"),QStringLiteral("libopenh264"),QStringLiteral("-pix_fmt"),QStringLiteral("yuv420p"),path
    };
    require(QProcess::execute(QStringLiteral("ffmpeg.exe"),arguments) == 0,"Could not create tagged comparison fixture");
}

void createVariableRateVideo(const QString& path) {
    const QStringList arguments{
        QStringLiteral("-y"),QStringLiteral("-v"),QStringLiteral("error"),
        QStringLiteral("-f"),QStringLiteral("lavfi"),QStringLiteral("-i"),
        QStringLiteral("testsrc2=size=96x64:rate=4:duration=1"),
        QStringLiteral("-vf"),QStringLiteral("select=not(eq(n\\,1)),setparams=range=tv:color_primaries=bt709:color_trc=bt709:colorspace=bt709"),
        QStringLiteral("-fps_mode"),QStringLiteral("vfr"),
        QStringLiteral("-color_primaries"),QStringLiteral("bt709"),QStringLiteral("-color_trc"),QStringLiteral("bt709"),
        QStringLiteral("-colorspace"),QStringLiteral("bt709"),QStringLiteral("-color_range"),QStringLiteral("tv"),
        QStringLiteral("-c:v"),QStringLiteral("libopenh264"),QStringLiteral("-pix_fmt"),QStringLiteral("yuv420p"),path
    };
    require(QProcess::execute(QStringLiteral("ffmpeg.exe"),arguments) == 0,"Could not create variable-rate comparison fixture");
}
}

int main(int argc, char** argv) {
    QQuickStyle::setStyle("Material");
    QGuiApplication app(argc,argv);
    try {
        require(argc == 2,"Expected the local ComparisonPage QML path");
        QTemporaryDir directory;
        require(directory.isValid(),"Could not create comparison-preview test directory");
        const auto source = directory.filePath(QStringLiteral("source-4fps.mp4"));
        const auto output = directory.filePath(QStringLiteral("output-8fps.mp4"));
        const auto variableOutput = directory.filePath(QStringLiteral("output-vfr.mp4"));
        createVideo(source,QStringLiteral("4"));
        createVideo(output,QStringLiteral("8"));
        createVariableRateVideo(variableOutput);

        QQmlApplicationEngine engine;
        auto* provider = new ComparisonFrameImageProvider();
        engine.addImageProvider("comparison",provider);
        QStringList openedFolders;
        ComparisonPreviewController controller(provider,nullptr,[&](const QUrl& folder) {
            openedFolders.push_back(folder.toLocalFile());
            return true;
        });
        engine.rootContext()->setContextProperty("comparisonPreview",&controller);
        engine.load(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
        require(!engine.rootObjects().isEmpty(),"Comparison QML page did not load");
        auto* page = engine.rootObjects().front();
        page->setProperty("originalSource",QUrl::fromLocalFile(source));
        page->setProperty("processedSource",QUrl::fromLocalFile(output));
        waitUntil([&] { return page->property("originalReady").toBool() && page->property("processedReady").toBool(); },
            "Comparison QML player did not open the local H.264 fixtures");
        waitUntil([&] { return controller.ready() && !controller.busy(); },
            "Comparison QML did not automatically show an exact first-frame preview");

        auto* visualPage = qobject_cast<QQuickItem*>(page);
        auto* openOriginalFolder = findVisualObject(visualPage,QStringLiteral("comparisonOpenOriginalFolder"));
        auto* openProcessedFolder = findVisualObject(visualPage,QStringLiteral("comparisonOpenProcessedFolder"));
        require(openOriginalFolder && openOriginalFolder->property("visible").toBool() && openOriginalFolder->property("enabled").toBool(),
            "Comparison QML did not enable navigation to the original video's folder");
        require(openProcessedFolder && openProcessedFolder->property("visible").toBool() && openProcessedFolder->property("enabled").toBool(),
            "Comparison QML did not enable navigation to the processed video's folder");
        require(QMetaObject::invokeMethod(openOriginalFolder,"clicked",Qt::DirectConnection) && openedFolders.size() == 1 &&
                openedFolders.back() == QFileInfo(source).absolutePath(),
            "Original-video navigation did not open its containing folder");
        require(QMetaObject::invokeMethod(openProcessedFolder,"clicked",Qt::DirectConnection) && openedFolders.size() == 2 &&
                openedFolders.back() == QFileInfo(output).absolutePath(),
            "Processed-video navigation did not open its containing folder");

        controller.requestFrames(QUrl::fromLocalFile(source),QUrl::fromLocalFile(output),370.0);
        waitForPreview(controller);
        require(controller.ready(),"Exact preview did not publish a source/output frame pair");
        require(std::abs(controller.outputTimestampMs() - 375.0) < 0.01,"Preview did not land on the nearest decoded 8 FPS output frame");
        const auto originalId = controller.originalFrameUrl().path().mid(1);
        const auto outputId = controller.processedFrameUrl().path().mid(1);
        QSize originalSize, outputSize;
        const auto originalImage = provider->requestImage(originalId,&originalSize,{});
        const auto outputImage = provider->requestImage(outputId,&outputSize,{});
        require(!originalImage.isNull() && originalSize == QSize(96,64),"Exact preview image provider did not return the decoded source frame");
        require(!outputImage.isNull() && outputSize == QSize(96,64),"Exact preview image provider did not return the decoded output frame");
        require(page->property("exactPreviewVisible").toBool(),"Comparison QML did not switch to the exact decoded frame images");
        auto* originalStill = page->findChild<QObject*>(QStringLiteral("exactOriginalFrameImage"));
        auto* outputStill = page->findChild<QObject*>(QStringLiteral("exactProcessedFrameImage"));
        require(originalStill && originalStill->property("visible").toBool() && outputStill && outputStill->property("visible").toBool(),
            "Comparison QML did not display both exact decoded still frames");

        auto* nextFrame = page->findChild<QObject*>(QStringLiteral("comparisonNextFrame"));
        require(nextFrame && QMetaObject::invokeMethod(nextFrame,"clicked",Qt::DirectConnection),
            "Comparison QML did not accept an exact frame-step action");
        waitForPreview(controller);
        require(std::abs(controller.outputTimestampMs() - 500.0) < 0.01,"Forward frame stepping did not follow the exact output cadence");
        require(page->property("exactPreviewVisible").toBool(),"Exact preview overlay disappeared after frame stepping");
        auto* previousFrame = page->findChild<QObject*>(QStringLiteral("comparisonPreviousFrame"));
        require(previousFrame && QMetaObject::invokeMethod(previousFrame,"clicked",Qt::DirectConnection),
            "Comparison QML did not accept a reverse exact frame-step action");
        waitForPreview(controller);
        require(std::abs(controller.outputTimestampMs() - 375.0) < 0.01,"Backward frame stepping did not return to the prior decoded output frame");

        controller.requestFrames(QUrl::fromLocalFile(source),QUrl::fromLocalFile(variableOutput),500.0);
        waitForPreview(controller);
        require(std::abs(controller.outputTimestampMs() - 500.0) < 0.01,"Variable-rate preview did not land on the requested decoded frame");
        controller.stepFrame(-1);
        waitForPreview(controller);
        require(std::abs(controller.outputTimestampMs()) < 0.01,"Variable-rate backward stepping did not select the actual previous PTS");
        controller.stepFrame(1);
        waitForPreview(controller);
        require(std::abs(controller.outputTimestampMs() - 500.0) < 0.01,"Variable-rate forward stepping did not select the actual next PTS");

        require(!controller.openContainingFolder(QUrl(QStringLiteral("https://example.invalid/video.mp4"))) &&
                !controller.navigationErrorString().isEmpty(),
            "Comparison preview did not reject non-local artifact navigation");
        auto* status = page->findChild<QObject*>(QStringLiteral("comparisonStatus"));
        require(status && status->property("text").toString() == controller.navigationErrorString(),
            "Comparison QML did not show a local navigation error");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
