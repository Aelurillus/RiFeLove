#include "studio_controller.hpp"
#include <QFileInfo>
#include <QDir>
#include <QUrl>

namespace {
std::string utf8(const QString& text) { return text.toUtf8().toStdString(); }
QString text(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
std::filesystem::path path(const QUrl& url) { return std::filesystem::path(url.toLocalFile().toStdWString()); }
double seconds(rifelove::Rational value) { return static_cast<double>(value.toLongDouble()); }
}
StudioController::StudioController(QObject* parent) : QObject(parent) {}
StudioController::~StudioController() { service_.cancel(); }
QUrl StudioController::localUrl(const QString& value) const { return QUrl::fromLocalFile(value); }
void StudioController::fail(const QString& detail) { busy_ = false; status_ = "failed"; detail_ = detail; emit changed(); emit operationFinished(status_); }
rifelove::application::ProcessingService::Callback StudioController::callback() {
    return [this](rifelove::application::ProcessingEvent event) {
        QMetaObject::invokeMethod(this, [this, event = std::move(event)]() mutable { receive(std::move(event)); }, Qt::QueuedConnection);
    };
}
void StudioController::openSource(const QUrl& url) {
    if (busy_) return;
    if (!url.isLocalFile() || !QFileInfo(url.toLocalFile()).isFile()) { fail(tr("请选择本机视频文件。")); return; }
    source_ = QDir::toNativeSeparators(QFileInfo(url.toLocalFile()).absoluteFilePath());
    const QFileInfo source(source_);
    output_ = QDir::toNativeSeparators(source.dir().filePath(source.completeBaseName() + "_RifeLove.mkv"));
    media_.clear(); progress_.clear(); detail_.clear(); status_ = "inspecting"; busy_ = true; emit changed();
    try { if (!service_.inspect(utf8(source_), callback())) fail(tr("请等待当前操作结束。")); }
    catch (const std::exception& error) { fail(text(error.what())); }
}
void StudioController::setOutput(const QUrl& url) {
    if (busy_) return;
    if (!url.isLocalFile() || url.toLocalFile().isEmpty()) { fail(tr("请选择本机输出位置。")); return; }
    output_ = QDir::toNativeSeparators(QFileInfo(url.toLocalFile()).absoluteFilePath()); emit changed();
}
void StudioController::start(const QString& rate, const QUrl& modelDirectory, const QUrl& runtimeDirectory, const QString& encoder) {
    if (busy_) return;
    try {
        if (source_.isEmpty() || media_.isEmpty()) throw std::invalid_argument("Select and inspect a source video first");
        if (output_.isEmpty() || QFileInfo::exists(output_)) throw std::invalid_argument("Output exists or is empty; select a new file path");
        const QString suffix = QFileInfo(output_).suffix().toLower();
        if (suffix != "mkv" && suffix != "mp4" && suffix != "mov") throw std::invalid_argument("Output must use .mkv, .mp4 or .mov");
        if (!modelDirectory.isLocalFile() || !runtimeDirectory.isLocalFile()) throw std::invalid_argument("Select local model and runtime directories");
        rifelove::application::ProcessingJob job;
        job.source = utf8(source_); job.output = utf8(output_);
        job.modelDirectory = path(modelDirectory); job.runtimeDirectory = path(runtimeDirectory);
        job.settings.targetRate = rifelove::Rational::parse(utf8(rate));
        if (job.settings.targetRate <= rifelove::Rational(0)) throw std::invalid_argument("Target FPS must be positive");
        job.settings.encoding.encoder = utf8(encoder);
        busy_ = true; status_ = "loading_model"; detail_.clear(); progress_.clear(); emit changed();
        if (!service_.submit(std::move(job), callback())) fail(tr("请等待当前操作结束。"));
    } catch (const std::exception& error) { fail(text(error.what())); }
}
void StudioController::cancel() { if (busy_) { status_ = "cancelling"; emit changed(); service_.cancel(); } }
void StudioController::receive(rifelove::application::ProcessingEvent event) {
    using Kind = rifelove::application::ProcessingEvent::Kind;
    if (event.kind == Kind::progress || event.kind == Kind::prepared) {
        if (status_ != "cancelling") status_ = text(event.stage);
        progress_["frames"] = QVariant::fromValue<qulonglong>(event.outputFrames);
        progress_["aiFrames"] = QVariant::fromValue<qulonglong>(event.aiFrames);
        progress_["fps"] = event.processingFps;
        if (event.timestamp) progress_["timestamp"] = seconds(*event.timestamp);
        if (!event.detail.empty()) detail_ = text(event.detail);
    } else {
        busy_ = false;
        if (event.kind == Kind::inspected && event.media) {
            status_ = "inspected";
            bool hasVideo = false;
            for (const auto& stream : event.media->streams) {
                if (stream.index != event.media->preferredVideoStream) continue;
                hasVideo = true;
                media_["width"] = stream.width; media_["height"] = stream.height;
                media_["codec"] = text(stream.codec);
                media_["rate"] = stream.averageRate ? text(stream.averageRate->toString()) : tr("未知");
                media_["color"] = text(stream.color.primaries) + " / " + text(stream.color.transfer) + " / " + text(stream.color.matrix);
                media_["depth"] = stream.color.bitDepth;
                media_["hdr"] = stream.color.hdrTransfer;
                media_["supportedColor"] = stream.color.bitDepth == 8 && !stream.color.hdrTransfer &&
                    !stream.color.masteringDisplay && !stream.color.contentLight && stream.color.primaries == "bt709" &&
                    (stream.color.transfer == "bt709" || stream.color.transfer == "iec61966-2-1") &&
                    (stream.color.matrix == "bt709" || stream.color.matrix == "gbr") &&
                    (stream.color.range == "tv" || stream.color.range == "pc");
                if (stream.duration) media_["duration"] = seconds(*stream.duration);
                else if (event.media->duration) media_["duration"] = seconds(*event.media->duration);
                if (stream.start) media_["start"] = seconds(*stream.start);
            }
            if (event.device) media_["gpu"] = text(event.device->name);
            else media_["gpu"] = tr("NVIDIA GPU 不可用");
            media_["gpuAvailable"] = event.device.has_value();
            media_["streams"] = static_cast<int>(event.media->streams.size());
            detail_ = text(event.detail);
            if (hasVideo && !media_.value("supportedColor").toBool()) detail_ += tr("当前模型路径仅支持明确标记的 BT.709 8-bit SDR；此视频需要后续色彩处理支持。");
            if (!hasVideo) { media_.clear(); status_ = "failed"; detail_ = tr("文件中没有可处理的视频轨。 "); }
        } else if (event.kind == Kind::completed && event.result) {
            status_ = "completed";
            const auto& result = *event.result;
            progress_["frames"] = QVariant::fromValue<qulonglong>(result.video.framesSubmitted);
            progress_["aiFrames"] = QVariant::fromValue<qulonglong>(result.aiFrames);
            progress_["fps"] = result.processingFps;
            progress_["encoder"] = text(result.video.encoder);
            detail_ = tr("已生成 %1 帧，其中 %2 帧由 RIFE 生成。输出已通过解码和时间轴检查。")
                .arg(result.video.framesSubmitted).arg(result.aiFrames);
            if (!result.encoderFallbackReason.empty()) {
                progress_["encoderFallbackReason"] = text(result.encoderFallbackReason);
                detail_ += "\n" + tr("GPU 编码器无法在当前环境启动，已使用软件 H.264；RIFE 插帧仍由 GPU 运行。");
            }
        } else if (event.kind == Kind::cancelled) { status_ = "cancelled"; detail_ = tr("操作已取消，未发布部分视频。"); }
        else { status_ = "failed"; detail_ = text(event.detail); }
        emit operationFinished(status_);
    }
    emit changed();
}
