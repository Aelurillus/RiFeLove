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
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#ifdef RIFELOVE_DESKTOP_PROCESSING
#include "studio_controller.hpp"
#include "queue_controller.hpp"
#endif

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle("Material");
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
    const QDir applicationDirectory(QCoreApplication::applicationDirPath());
    const QDir bundledModels(applicationDirectory.filePath("models"));
    QStringList bundledModelPaths;
    const auto modelDirectories = bundledModels.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& modelDirectory : modelDirectories) {
        if (!QFileInfo(QDir(modelDirectory.absoluteFilePath()).filePath("model.json")).isFile() ||
            !QFileInfo(QDir(modelDirectory.absoluteFilePath()).filePath("model.onnx")).isFile()) continue;
        bundledModelPaths.push_back(modelDirectory.absoluteFilePath());
    }
    QString bundledModel;
    QFile defaultModelMarker(applicationDirectory.filePath("models/default-model.txt"));
    if (defaultModelMarker.open(QIODevice::ReadOnly)) {
        const auto modelId = QString::fromUtf8(defaultModelMarker.readAll()).trimmed();
        if (QRegularExpression(QStringLiteral("^[A-Za-z0-9._-]{1,128}$")).match(modelId).hasMatch()) {
            const auto candidate = QDir(bundledModels.filePath(modelId)).absolutePath();
            if (bundledModelPaths.contains(candidate,Qt::CaseInsensitive)) bundledModel = candidate;
        }
    }
    if (bundledModel.isEmpty() && bundledModelPaths.size() == 1) bundledModel = bundledModelPaths.front();
    const auto bundledRuntime = applicationDirectory.filePath("runtime");
    const bool portable = QFileInfo(applicationDirectory.filePath("portable.flag")).isFile();
    const auto dataRoot = parser.isSet(dataDirectory) ? parser.value(dataDirectory) : portable ?
        applicationDirectory.filePath("data") : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!QDir().mkpath(dataRoot)) return 2;
    QSettings preferences(QDir(dataRoot).filePath("preferences.ini"),QSettings::IniFormat);
    preferences.setFallbacksEnabled(false);
    preferences.sync();
    if (preferences.status() == QSettings::NoError) {
        const auto modelSetting = preferences.value("model").toUrl();
        const auto selectedModel = modelSetting.isLocalFile() ? modelSetting.toLocalFile() : QString();
        const bool selectedModelAvailable = !selectedModel.isEmpty() && QFileInfo(QDir(selectedModel).filePath("model.json")).isFile() &&
            QFileInfo(QDir(selectedModel).filePath("model.onnx")).isFile();
        const bool bundledModelAvailable = !bundledModel.isEmpty() && QFileInfo(QDir(bundledModel).filePath("model.json")).isFile() &&
            QFileInfo(QDir(bundledModel).filePath("model.onnx")).isFile();
        if (!selectedModelAvailable && bundledModelAvailable) {
            const auto bundledModelUrl = QUrl::fromLocalFile(bundledModel);
            preferences.setValue("model",bundledModelUrl);
        }
        auto models = preferences.value("modelCatalog/paths").toStringList();
        for (const auto& path : bundledModelPaths) {
            const auto normalized = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
            if (!models.contains(normalized,Qt::CaseInsensitive)) models.push_back(normalized);
        }
        if (!models.isEmpty()) preferences.setValue("modelCatalog/paths",models);
        const auto runtimeSetting = preferences.value("runtime").toUrl();
        const auto selectedRuntime = runtimeSetting.isLocalFile() ? runtimeSetting.toLocalFile() : QString();
        const bool selectedRuntimeAvailable = !selectedRuntime.isEmpty() && QFileInfo(QDir(selectedRuntime).filePath("onnxruntime.dll")).isFile() &&
            QFileInfo(QDir(selectedRuntime).filePath("onnxruntime_providers_cuda.dll")).isFile();
        const bool bundledRuntimeAvailable = QFileInfo(QDir(bundledRuntime).filePath("onnxruntime.dll")).isFile() &&
            QFileInfo(QDir(bundledRuntime).filePath("onnxruntime_providers_cuda.dll")).isFile();
        if (!selectedRuntimeAvailable && bundledRuntimeAvailable)
            preferences.setValue("runtime",QUrl::fromLocalFile(bundledRuntime));
        if (preferences.value("rate").toString().isEmpty()) preferences.setValue("rate","60");
        if (preferences.value("encoder").toString().isEmpty()) preferences.setValue("encoder","auto");
        if (preferences.value("container").toString().isEmpty()) preferences.setValue("container","mkv");
        if (preferences.value("output").toUrl().isEmpty()) {
            auto outputRoot = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
            if (outputRoot.isEmpty()) outputRoot = dataRoot;
            const auto output = QDir(outputRoot).filePath("RifeLove");
            if (QDir().mkpath(output)) preferences.setValue("output",QUrl::fromLocalFile(output));
        }
        preferences.sync();
    }
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
