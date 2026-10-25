#include <QGuiApplication>
#include <QDebug>
#include <QQmlApplicationEngine>
#include <QUrl>
#include <QCommandLineParser>
#include <QQuickWindow>
#include <QTimer>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QLocale>
#include <QTranslator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <filesystem>
#include <string>
#include <vector>
#include "common/logging.hpp"
#include "language_settings.hpp"
#ifdef RIFELOVE_DESKTOP_PROCESSING
#include "studio_controller.hpp"
#include "queue_controller.hpp"
#include "comparison_preview.hpp"
#endif

namespace {
std::filesystem::path nativePath(const QString& value) {
#if defined(_WIN32)
    return std::filesystem::path(value.toStdWString());
#else
    const auto utf8 = value.toUtf8();
    return std::filesystem::u8path(utf8.constData(),utf8.constData() + utf8.size());
#endif
}

void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    try {
        const auto utf8Message = message.toUtf8();
        rifelove::logging::Level level = rifelove::logging::Level::info;
        switch (type) {
        case QtDebugMsg: level = rifelove::logging::Level::debug; break;
        case QtInfoMsg: level = rifelove::logging::Level::info; break;
        case QtWarningMsg: level = rifelove::logging::Level::warning; break;
        case QtCriticalMsg: level = rifelove::logging::Level::error; break;
        case QtFatalMsg: level = rifelove::logging::Level::critical; break;
        }
        const std::string category = context.category ? context.category : "default";
        rifelove::logging::write(level,"qt","message",
            std::string(utf8Message.constData(),static_cast<std::size_t>(utf8Message.size())),{{"category",category}});
    } catch (...) {
    }
}
} // namespace

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
    const QDir applicationDirectory(QCoreApplication::applicationDirPath());
    const bool portable = QFileInfo(applicationDirectory.filePath("portable.flag")).isFile();
    const auto dataRoot = parser.isSet(dataDirectory) ? parser.value(dataDirectory) : portable ?
        applicationDirectory.filePath("data") : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!QDir().mkpath(dataRoot)) return 2;

    LanguageSettings appLanguage(QDir(dataRoot).filePath("language.ini"), QLocale::system().name(), &app);
    const auto languageCode = appLanguage.language();
    const auto locale = languageCode == QStringLiteral("en") ? QLocale(QLocale::English, QLocale::UnitedStates)
        : languageCode == QStringLiteral("ja") ? QLocale(QLocale::Japanese, QLocale::Japan)
        : languageCode == QStringLiteral("zh_TW") ? QLocale(QLocale::Chinese, QLocale::Taiwan)
        : QLocale(QLocale::Chinese, QLocale::China);
    QLocale::setDefault(locale);
    QTranslator translator;
    if (languageCode != QStringLiteral("zh_CN")) {
        const auto translationPath = QStringLiteral(":/rifelove/i18n/rifelove_%1.qm").arg(languageCode);
        if (!translator.load(translationPath)) {
            qCritical().noquote() << "Could not load local translation catalog" << translationPath;
            return 2;
        }
        app.installTranslator(&translator);
    }
#ifdef RIFELOVE_DESKTOP_PROCESSING
    StudioController studio;
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
    const auto logPath = nativePath(QDir(dataRoot).filePath(QStringLiteral("logs/rifelove.jsonl")));
    if (rifelove::logging::initialize(logPath)) {
        qInstallMessageHandler(qtMessageHandler);
        rifelove::logging::write(rifelove::logging::Level::info,"desktop","application.start",
            "RifeLove desktop started",{{"portable",portable}});
    }
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
    auto* comparisonProvider = new ComparisonFrameImageProvider();
    engine.addImageProvider("comparison",comparisonProvider);
    ComparisonPreviewController comparisonPreview(comparisonProvider);
    engine.rootContext()->setContextProperty("studio", &studio);
    engine.rootContext()->setContextProperty("taskQueue", &taskQueue);
    engine.rootContext()->setContextProperty("comparisonPreview", &comparisonPreview);
    engine.rootContext()->setContextProperty("appLanguage", &appLanguage);
    engine.rootContext()->setContextProperty("processingAvailable", true);
#else
    engine.rootContext()->setContextProperty("studio", QVariant());
    engine.rootContext()->setContextProperty("taskQueue", QVariant());
    engine.rootContext()->setContextProperty("comparisonPreview", QVariant());
    engine.rootContext()->setContextProperty("appLanguage", &appLanguage);
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
    const int exitCode = app.exec();
    qInstallMessageHandler(nullptr);
    rifelove::logging::shutdown();
    return exitCode;
}
