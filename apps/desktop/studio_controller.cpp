#include "studio_controller.hpp"
#include "encoding_settings.hpp"
#include "timeline/timeline.hpp"
#include <QFileInfo>
#include <QDir>
#include <QStringList>
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
    sourceDurationEstimate_.reset();
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
void StudioController::start(const QString& rate, const QUrl& modelDirectory, const QUrl& runtimeDirectory,
    const QString& encoder, const QVariantMap& encodingOptions) {
    if (busy_) return;
    try {
        if (source_.isEmpty() || media_.isEmpty()) throw std::invalid_argument("Select and inspect a source video first");
        const auto encoders = media_.value("encoders").toMap();
        if (encoder == QStringLiteral("auto")) {
            const auto nvenc = encoders.value(QStringLiteral("h264_nvenc")).toMap().value(QStringLiteral("contextOpened")).toBool();
            const auto software = encoders.value(QStringLiteral("libopenh264")).toMap().value(QStringLiteral("contextOpened")).toBool();
            if (!nvenc && !software) throw std::invalid_argument("Automatic H.264 is unavailable: neither NVENC nor software H.264 could initialize");
        } else if (!encoders.value(encoder).toMap().value(QStringLiteral("contextOpened")).toBool()) {
            throw std::invalid_argument("The selected video encoder did not initialize for this source profile");
        }
        if (output_.isEmpty() || QFileInfo::exists(output_)) throw std::invalid_argument("Output exists or is empty; select a new file path");
        const QString suffix = QFileInfo(output_).suffix().toLower();
        if (suffix != "mkv" && suffix != "mp4" && suffix != "mov") throw std::invalid_argument("Output must use .mkv, .mp4 or .mov");
        if (!modelDirectory.isLocalFile() || !runtimeDirectory.isLocalFile()) throw std::invalid_argument("Select local model and runtime directories");
        rifelove::application::ProcessingJob job;
        job.source = utf8(source_); job.output = utf8(output_);
        job.modelDirectory = path(modelDirectory); job.runtimeDirectory = path(runtimeDirectory);
        job.settings.targetRate = rifelove::Rational::parse(utf8(rate));
        if (job.settings.targetRate <= rifelove::Rational(0)) throw std::invalid_argument("Target FPS must be positive");
        job.sourceDurationEstimate = sourceDurationEstimate_;
        job.settings.encoding = rifelove::desktop::makeVideoEncodingSettings(encoder, encodingOptions);
        busy_ = true; status_ = "loading_model"; detail_.clear(); progress_.clear();
        if (const auto count = rifelove::estimateOutputFrameCount(job.sourceDurationEstimate,job.settings.targetRate))
            progress_["estimatedFrames"] = QVariant::fromValue<qulonglong>(*count);
        emit changed();
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
        if (event.stage == "encoder_fallback") progress_["encoderFallbackReason"] = text(event.detail);
        if (event.kind == Kind::prepared && !event.detail.empty()) progress_["inferenceFallbackReason"] = text(event.detail);
        QStringList notices;
        if (!progress_.value("inferenceFallbackReason").toString().isEmpty())
            notices.push_back(tr("TensorRT 当前不可用，已回退到 ONNX Runtime CUDA 推理。"));
        if (!progress_.value("encoderFallbackReason").toString().isEmpty())
            notices.push_back(tr("NVIDIA 视频编码器当前不可用，已切换到软件 H.264；补帧仍在进行。"));
        if (!notices.isEmpty()) detail_ = notices.join('\n');
        else if (event.stage != "encoder_fallback" && !event.detail.empty()) detail_ = text(event.detail);
        else detail_.clear();
    } else {
        busy_ = false;
        if (event.kind == Kind::inspected && event.media) {
            status_ = "inspected";
            sourceDurationEstimate_.reset();
            QVariantMap encoders;
            for (const auto& capability : event.encoderCapabilities) {
                QVariantMap value;
                value["registered"] = capability.registered;
                value["contextOpened"] = capability.contextOpened;
                value["reason"] = text(capability.reason);
                encoders[text(capability.encoder)] = value;
            }
            media_["encoders"] = encoders;
            const auto nvenc = encoders.value(QStringLiteral("h264_nvenc")).toMap();
            const auto softwareH264 = encoders.value(QStringLiteral("libopenh264")).toMap();
            media_["nvencContextOpened"] = nvenc.value("contextOpened").toBool();
            media_["softwareH264ContextOpened"] = softwareH264.value("contextOpened").toBool();
            media_["nvencReason"] = nvenc.value("reason").toString();
            bool hasVideo = false;
            QStringList notices;
            if (!event.detail.empty()) notices.push_back(text(event.detail));
            for (const auto& stream : event.media->streams) {
                if (stream.index != event.media->preferredVideoStream) continue;
                hasVideo = true;
                if (stream.duration && *stream.duration > rifelove::Rational(0)) sourceDurationEstimate_ = stream.duration;
                else if (stream.declaredEnd && stream.start && *stream.declaredEnd > *stream.start)
                    sourceDurationEstimate_ = *stream.declaredEnd - *stream.start;
                else if (event.media->duration && *event.media->duration > rifelove::Rational(0)) sourceDurationEstimate_ = event.media->duration;
                media_["width"] = stream.width; media_["height"] = stream.height;
                media_["codec"] = text(stream.codec);
                media_["rate"] = stream.averageRate ? text(stream.averageRate->toString()) : tr("未知");
                media_["color"] = text(stream.color.primaries) + " / " + text(stream.color.transfer) + " / " + text(stream.color.matrix) + " / " + text(stream.color.range);
                media_["depth"] = stream.color.bitDepth;
                media_["hdr"] = stream.color.hdrTransfer;
                media_["supportedColor"] = event.sourceColorSupported;
                if (stream.duration) media_["duration"] = seconds(*stream.duration);
                else if (event.media->duration) media_["duration"] = seconds(*event.media->duration);
                if (stream.start) media_["start"] = seconds(*stream.start);
            }
            if (event.device) media_["gpu"] = text(event.device->name);
            else media_["gpu"] = tr("NVIDIA GPU 不可用");
            media_["gpuAvailable"] = event.device.has_value();
            media_["streams"] = static_cast<int>(event.media->streams.size());
            if (hasVideo && !media_.value("nvencContextOpened").toBool()) {
                if (media_.value("softwareH264ContextOpened").toBool()) {
                    const auto reason = media_.value("nvencReason").toString();
                    const bool apiMismatch = reason.contains(QStringLiteral("Function not implemented"),Qt::CaseInsensitive) ||
                        reason.contains(QStringLiteral("minimum required Nvidia driver"),Qt::CaseInsensitive) ||
                        reason.contains(QStringLiteral("required nvenc API version"),Qt::CaseInsensitive);
                    notices.push_back(apiMismatch
                        ? tr("NVENC 与当前 NVIDIA 驱动不兼容；自动编码将使用软件 H.264，RIFE 插帧仍使用 GPU。")
                        : tr("NVENC 当前无法初始化；自动编码将使用软件 H.264，RIFE 插帧仍使用 GPU。"));
                } else {
                    notices.push_back(tr("NVENC 和软件 H.264 均未能初始化；请选择其他可用编码器或检查本机编码运行环境。"));
                }
            }
            if (hasVideo) {
                const auto av1 = encoders.value(QStringLiteral("libaom-av1")).toMap();
                const auto hevc = encoders.value(QStringLiteral("libkvazaar")).toMap();
                QStringList unavailable;
                if (!hevc.value(QStringLiteral("contextOpened")).toBool()) unavailable.push_back(tr("HEVC"));
                if (!av1.value(QStringLiteral("contextOpened")).toBool()) unavailable.push_back(tr("AV1"));
                if (!unavailable.isEmpty()) notices.push_back(tr("当前视频配置无法初始化：%1。选择这些编码器时不可开始。")
                    .arg(unavailable.join(QStringLiteral("、"))));
            }
            if (hasVideo && !media_.value("supportedColor").toBool())
                notices.push_back(tr("当前模型路径支持明确标记的 BT.709 或 BT.470BG/SMPTE 170M 8-bit SDR；HDR、10-bit 和未标记色彩需要后续色彩处理支持。"));
            detail_ = notices.join('\n');
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
            if (!result.inferenceFallbackReason.empty()) {
                progress_["inferenceFallbackReason"] = text(result.inferenceFallbackReason);
                detail_ += "\n" + tr("TensorRT 未能启动，已使用 ONNX Runtime CUDA 完成推理。");
            }
        } else if (event.kind == Kind::cancelled) { status_ = "cancelled"; detail_ = tr("操作已取消，未发布部分视频。"); }
        else { status_ = "failed"; detail_ = text(event.detail); }
        emit operationFinished(status_);
    }
    emit changed();
}
