#include <QTranslator>
#include <QStringList>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::invalid_argument("Expected English, Traditional Chinese, and Japanese catalogs");
        const QStringList expectedWorkspace{
            QStringLiteral("RIFELOVE · LOCAL WORKSPACE"),
            QStringLiteral("RIFELOVE · 本機工作區"),
            QStringLiteral("RIFELOVE · ローカル ワークスペース")};
        for (int index = 0; index < 3; ++index) {
            QTranslator translator;
            require(translator.load(QString::fromLocal8Bit(argv[index + 1])), "Could not load a compiled local translation catalog");
            require(translator.translate("HomePage", "RIFELOVE · 本机工作区") == expectedWorkspace[index],
                "A compiled catalog did not translate the Home page context");
            const auto fallback = translator.translate("StudioController",
                "NVENC 未能启动，现在由 CPU 使用%1编码；RIFE 插帧仍由 CUDA 运行。");
            require(fallback.contains(QStringLiteral("%1")) && !fallback.contains(QStringLiteral("未能启动")),
                "A compiled catalog lost the fallback placeholder or untranslated source text");
            require(translator.translate("QueueWorker", "任务已重新加入队列。") != QStringLiteral("任务已重新加入队列。"),
                "A compiled catalog did not translate the queue worker context");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
