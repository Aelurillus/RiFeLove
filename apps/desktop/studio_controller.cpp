#include "studio_controller.hpp"
#include "application/job_serialization.hpp"
#include "encoding_settings.hpp"
#include "gpu/cuda_device.hpp"
#include "common/logging.hpp"
#include "timeline/timeline.hpp"
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>
#include <QStringList>
#include <cmath>
#include <limits>
#include <numeric>
#include <QUrl>

namespace {
std::string utf8(const QString& text) { return text.toUtf8().toStdString(); }
QString text(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
std::filesystem::path path(const QUrl& url) { return std::filesystem::path(url.toLocalFile().toStdWString()); }
double seconds(rifelove::Rational value) { return static_cast<double>(value.toLongDouble()); }
QString automaticSoftwareEncoder(const QVariantMap& encoders) {
    for (const auto& candidate : {QStringLiteral("libopenh264"),QStringLiteral("libkvazaar"),QStringLiteral("libaom-av1")})
        if (encoders.value(candidate).toMap().value(QStringLiteral("contextOpened")).toBool()) return candidate;
    return {};
}
QString softwareEncoderLabel(const QString& encoder) {
    if (encoder == QStringLiteral("libopenh264")) return QCoreApplication::translate("StudioController","软件 H.264");
    if (encoder == QStringLiteral("libkvazaar")) return QCoreApplication::translate("StudioController","软件 HEVC");
    if (encoder == QStringLiteral("libaom-av1")) return QCoreApplication::translate("StudioController","软件 AV1");
    return QCoreApplication::translate("StudioController","兼容的软件编码器");
}
QString encoderFallbackNotice(const QString& reason, const QString& encoder) {
    const auto label = softwareEncoderLabel(encoder);
    if (reason.contains(QStringLiteral("required nvenc API version"),Qt::CaseInsensitive)) {
        const QRegularExpression apiPattern(QStringLiteral("Required:\\s*([0-9.]+)\\s+Found:\\s*([0-9.]+)"),QRegularExpression::CaseInsensitiveOption);
        const QRegularExpression driverPattern(QStringLiteral("minimum required Nvidia driver for nvenc is\\s+([0-9.]+)"),QRegularExpression::CaseInsensitiveOption);
        const auto api = apiPattern.match(reason);
        const auto driver = driverPattern.match(reason);
        if (api.hasMatch() && driver.hasMatch())
            return QCoreApplication::translate("StudioController",
                "NVENC 驱动接口不兼容：当前提供 API %1，FFmpeg 需要 API %2（驱动 %3 或更新）。现在由 CPU 使用%4编码，RIFE 仍由 CUDA 插帧。")
                .arg(api.captured(2),api.captured(1),driver.captured(1),label);
    }
    if (reason.contains(QStringLiteral("Function not implemented"),Qt::CaseInsensitive))
        return QCoreApplication::translate("StudioController",
            "当前 NVIDIA 驱动无法初始化此 FFmpeg 的 NVENC 接口。现在由 CPU 使用%1编码，RIFE 仍由 CUDA 插帧。")
            .arg(label);
    return QCoreApplication::translate("StudioController",
        "NVENC 未能启动，现在由 CPU 使用%1编码；RIFE 插帧仍由 CUDA 运行。")
        .arg(label);
}
QVariantMap timingMap(const rifelove::PipelineTimingBreakdown& value) {
    return {{QStringLiteral("sourceProbeSeconds"),value.sourceProbeSeconds},
        {QStringLiteral("encoderSetupSeconds"),value.encoderSetupSeconds},
        {QStringLiteral("sourceDecodeSeconds"),value.sourceDecodeSeconds},
        {QStringLiteral("sourcePreparationSeconds"),value.sourcePreparationSeconds},
        {QStringLiteral("sceneAnalysisSeconds"),value.sceneAnalysisSeconds},
        {QStringLiteral("inputUploadSeconds"),value.inputUploadSeconds},
        {QStringLiteral("inferenceWaitSeconds"),value.inferenceWaitSeconds},
        {QStringLiteral("outputDownloadSeconds"),value.outputDownloadSeconds},
        {QStringLiteral("colorRestoreSeconds"),value.colorRestoreSeconds},
        {QStringLiteral("restorationSeconds"),value.restorationSeconds},
        {QStringLiteral("frameEncodeSeconds"),value.frameEncodeSeconds},
        {QStringLiteral("outputFinalizeSeconds"),value.outputFinalizeSeconds},
        {QStringLiteral("outputValidationSeconds"),value.outputValidationSeconds},
        {QStringLiteral("pipelineWallSeconds"),value.pipelineWallSeconds}};
}
}
StudioController::StudioController(QObject* parent) : QObject(parent) {
    progressWatchdogTimer_.setInterval(1000);
    connect(&progressWatchdogTimer_,&QTimer::timeout,this,[this] {
        const bool stalled = busy_ && status_ == QStringLiteral("processing") &&
            progressWatchdog_.stalled(rifelove::desktop::ProgressWatchdog::Clock::now(),std::chrono::seconds(60));
        if (stalled != progressStalled_) { progressStalled_ = stalled; emit changed(); }
    });
    progressWatchdogTimer_.start();
    try {
        rifelove::gpu::CudaDevice device;
        const auto& info = device.info();
        const auto memory = device.memory();
        deviceInfo_["available"] = true;
        deviceInfo_["name"] = text(info.name);
        deviceInfo_["computeCapability"] = QStringLiteral("%1.%2").arg(info.computeMajor).arg(info.computeMinor);
        deviceInfo_["driverCudaApiVersion"] = info.driverCudaApiVersion;
        deviceInfo_["totalBytes"] = QVariant::fromValue<qulonglong>(memory.totalBytes);
        deviceInfo_["freeBytes"] = QVariant::fromValue<qulonglong>(memory.freeBytes);
    } catch (const std::exception& error) {
        deviceInfo_["available"] = false;
        deviceInfo_["error"] = text(error.what());
    }
}
StudioController::~StudioController() { service_.cancel(); }
QUrl StudioController::localUrl(const QString& value) const { return QUrl::fromLocalFile(value); }
void StudioController::fail(const QString& detail) {
    busy_ = false; progressStalled_ = false; progressWatchdog_.reset(); status_ = "failed"; detail_ = detail;
    emit changed(); emit operationFinished(status_);
}
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
    inspectedMedia_.reset();
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
    const QString& encoder, const QVariantMap& encodingOptions, const QString& inferenceBackend, double cutMeanDifference,
    const QUrl& restorationModelDirectory) {
    if (busy_) return;
    try {
        if (source_.isEmpty() || media_.isEmpty() || !inspectedMedia_) throw std::invalid_argument("Select and inspect a source video first");
        const auto targetRate = rifelove::Rational::parse(utf8(rate));
        if (targetRate <= rifelove::Rational(0)) throw std::invalid_argument("Target frame rate must be positive");
        const auto selectedStream = std::find_if(inspectedMedia_->streams.begin(),inspectedMedia_->streams.end(),[&](const auto& stream) {
            return stream.index == inspectedMedia_->preferredVideoStream && stream.type == "video";
        });
        if (selectedStream == inspectedMedia_->streams.end())
            throw std::invalid_argument("Selected video stream is unavailable; inspect the video again");
        QString resolvedEncoder = encoder;
        QString preflightEncoderFallback;
        if (output_.isEmpty() || QFileInfo::exists(output_)) throw std::invalid_argument("Output exists or is empty; select a new file path");
        const QString suffix = QFileInfo(output_).suffix().toLower();
        if (suffix != "mkv" && suffix != "mp4" && suffix != "mov") throw std::invalid_argument("Output must use .mkv, .mp4 or .mov");
        if (!modelDirectory.isLocalFile() || !runtimeDirectory.isLocalFile()) throw std::invalid_argument("Select local model and runtime directories");
        if (!restorationModelDirectory.isEmpty() && !restorationModelDirectory.isLocalFile())
            throw std::invalid_argument("Select a local restoration model directory");
        rifelove::application::ProcessingJob job;
        job.source = utf8(source_); job.output = utf8(output_);
        job.modelDirectory = path(modelDirectory); job.runtimeDirectory = path(runtimeDirectory);
        if (!restorationModelDirectory.isEmpty()) {
            if (!restorationModelDirectory.isLocalFile()) throw std::invalid_argument("Restoration model must be local");
            job.restorationModelDirectory = path(restorationModelDirectory);
            rifelove::application::pinRestorationModelDescriptor(job);
        }
        job.sourceDurationEstimate = sourceDurationEstimate_;
        job.settings.encoding = rifelove::desktop::makeVideoEncodingSettings(resolvedEncoder, encodingOptions);
        job.settings.targetRate = targetRate;
        job.settings.inferenceBackend = utf8(inferenceBackend);
        if (!std::isfinite(cutMeanDifference) || cutMeanDifference < 0.0 || cutMeanDifference > 1.0)
            throw std::invalid_argument("Scene-cut threshold must be a finite value from zero to one");
        job.settings.scene.cutMeanDifference = cutMeanDifference;
        job.settings.encoding.videoStreamIndex = inspectedMedia_->preferredVideoStream;
        job.settings.encoding.frameRate = targetRate;
        const auto sourceBaseDenominator = selectedStream->timeBase.denominator();
        if (sourceBaseDenominator <= 0) throw std::invalid_argument("Selected video stream has no valid time base");
        const auto timeBaseFactor = sourceBaseDenominator / std::gcd(sourceBaseDenominator,targetRate.numerator());
        if (timeBaseFactor <= 0 || targetRate.numerator() > std::numeric_limits<std::int32_t>::max() / timeBaseFactor)
            throw std::invalid_argument("Source and target times need an encoder time base beyond 32-bit range");
        const auto outputTimeBase = rifelove::Rational(1,timeBaseFactor * targetRate.numerator());
        job.settings.encoding.timeBase = outputTimeBase;
        const auto probeSettingsFor = [&](const rifelove::media::VideoEncodingSettings& settings) {
            rifelove::media::EncoderProbeSettings probe;
            probe.pixelFormat = settings.pixelFormat;
            probe.timeBase = outputTimeBase;
            probe.frameRate = targetRate;
            probe.bitRate = settings.bitRate;
            probe.gopSize = settings.gopSize;
            probe.maxBFrames = settings.maxBFrames;
            probe.encoderOptions = settings.encoderOptions;
            return probe;
        };
        if (encoder == QStringLiteral("auto")) {
            const auto nvenc = rifelove::media::MediaEngine::probeEncoderWithSettings(
                "h264_nvenc",*selectedStream,probeSettingsFor(job.settings.encoding));
            if (nvenc.contextOpened) {
                job.settings.encoding.encoder = "auto";
                preflightEncoderFallback.clear();
            } else {
                preflightEncoderFallback = QString::fromStdString(nvenc.reason);
                bool foundSoftware = false;
                for (const auto* candidate : {"libopenh264","libkvazaar","libaom-av1"}) {
                    auto candidateSettings = rifelove::desktop::makeVideoEncodingSettings(QString::fromLatin1(candidate),encodingOptions);
                    candidateSettings.videoStreamIndex = inspectedMedia_->preferredVideoStream;
                    candidateSettings.frameRate = targetRate;
                    candidateSettings.timeBase = outputTimeBase;
                    const auto capability = rifelove::media::MediaEngine::probeEncoderWithSettings(
                        candidate,*selectedStream,probeSettingsFor(candidateSettings));
                    if (capability.contextOpened) {
                        resolvedEncoder = QString::fromLatin1(candidate);
                        job.settings.encoding = std::move(candidateSettings);
                        foundSoftware = true;
                        break;
                    }
                    preflightEncoderFallback += "; " + QString::fromStdString(capability.encoder + ": " + capability.reason);
                }
                if (!foundSoftware)
                    throw std::invalid_argument("Automatic encoding found no software encoder compatible with the requested output settings: " + utf8(preflightEncoderFallback));
            }
        } else {
            const auto capability = rifelove::media::MediaEngine::probeEncoderWithSettings(
                utf8(encoder),*selectedStream,probeSettingsFor(job.settings.encoding));
            if (!capability.contextOpened)
                throw std::invalid_argument("The selected encoder could not open for the requested output frame rate and settings: " + capability.reason);
        }
        progressWatchdog_.reset(); progressStalled_ = false;
        busy_ = true; status_ = "loading_model"; detail_.clear(); progress_.clear();
        if (!preflightEncoderFallback.isEmpty()) {
            progress_["encoderFallbackReason"] = preflightEncoderFallback;
            progress_["encoder"] = resolvedEncoder;
            detail_ = encoderFallbackNotice(preflightEncoderFallback,resolvedEncoder);
            rifelove::logging::write(rifelove::logging::Level::warning,"studio","encoder.fallback",
                "NVENC initialization failed; automatic encoding selected a software encoder",
                {{"reason",utf8(preflightEncoderFallback)},{"encoder",utf8(resolvedEncoder)}});
        }
        if (const auto count = rifelove::estimateOutputFrameCount(job.sourceDurationEstimate,job.settings.targetRate))
            progress_["estimatedFrames"] = QVariant::fromValue<qulonglong>(*count);
        emit changed();
        if (!service_.submit(std::move(job), callback())) fail(tr("请等待当前操作结束。"));
    } catch (const std::exception& error) { fail(text(error.what())); }
}
void StudioController::cancel() {
    if (busy_) { progressStalled_ = false; status_ = "cancelling"; emit changed(); service_.cancel(); }
}
void StudioController::receive(rifelove::application::ProcessingEvent event) {
    using Kind = rifelove::application::ProcessingEvent::Kind;
    if (event.kind == Kind::progress || event.kind == Kind::prepared) {
        if (status_ != "cancelling") status_ = text(event.stage);
        if (event.stage == "processing") {
            if (progressWatchdog_.observe(event.outputFrames)) progressStalled_ = false;
        } else progressStalled_ = false;
        progress_["frames"] = QVariant::fromValue<qulonglong>(event.outputFrames);
        progress_["aiFrames"] = QVariant::fromValue<qulonglong>(event.aiFrames);
        progress_["fps"] = event.processingFps;
        progress_["timings"] = timingMap(event.timings);
        if (event.timestamp) progress_["timestamp"] = seconds(*event.timestamp);
        if (event.stage == "encoder_fallback") {
            progress_["encoderFallbackReason"] = text(event.detail);
            if (!event.encoder.empty()) progress_["encoder"] = text(event.encoder);
        }
        if (event.kind == Kind::prepared && !event.detail.empty()) progress_["inferenceFallbackReason"] = text(event.detail);
        QStringList notices;
        if (!progress_.value("inferenceFallbackReason").toString().isEmpty())
            notices.push_back(tr("TensorRT 当前不可用，已回退到 ONNX Runtime CUDA 推理。"));
        if (!progress_.value("encoderFallbackReason").toString().isEmpty())
            notices.push_back(encoderFallbackNotice(progress_.value("encoderFallbackReason").toString(),
                progress_.value("encoder").toString()));
        if (!notices.isEmpty()) detail_ = notices.join('\n');
        else if (event.stage != "encoder_fallback" && !event.detail.empty()) detail_ = text(event.detail);
        else detail_.clear();
    } else {
        busy_ = false; progressStalled_ = false; progressWatchdog_.reset();
        if (event.kind == Kind::inspected && event.media) {
            inspectedMedia_ = *event.media;
            status_ = "inspected";
            sourceDurationEstimate_.reset();
            const auto durationText = event.media->duration
                ? tr("%1 秒").arg(QString::number(seconds(*event.media->duration),'f',2)) : tr("未知");
            const auto totalBitRate = event.media->bitRate > 0 ? tr("%1 kb/s").arg(event.media->bitRate / 1000) : tr("未知");
            media_["containerSummary"] = tr("容器 %1 · 时长 %2 · 总码率 %3 · %4 条轨道 · %5 个章节")
                .arg(text(event.media->format),durationText,totalBitRate)
                .arg(event.media->streams.size()).arg(event.media->chapters.size());
            QVariantList streamDetails;
            streamDetails.reserve(static_cast<qsizetype>(event.media->streams.size()));
            for (const auto& stream : event.media->streams) {
                QString typeLabel;
                if (stream.type == "video") typeLabel = tr("视频");
                else if (stream.type == "audio") typeLabel = tr("音频");
                else if (stream.type == "subtitle") typeLabel = tr("字幕");
                else if (stream.type == "data") typeLabel = tr("数据");
                else if (stream.type == "attachment") typeLabel = tr("附件");
                else typeLabel = tr("其他");
                QString heading = tr("轨道 %1 · %2 · %3").arg(stream.index).arg(typeLabel,text(stream.codec));
                if (stream.defaultTrack) heading += QStringLiteral(" · ") + tr("默认");
                if (stream.forcedTrack) heading += QStringLiteral(" · ") + tr("强制");

                QStringList summary;
                QString colorSummary;
                if (stream.type == "video") {
                    const auto averageRate = stream.averageRate ? text(stream.averageRate->toString()) : tr("未知");
                    const auto nominalRate = stream.nominalRate ? text(stream.nominalRate->toString()) : tr("未知");
                    summary.push_back(tr("%1 × %2 · 平均 %3 FPS · 标称 %4 FPS · %5 · %6-bit")
                        .arg(stream.width).arg(stream.height).arg(averageRate,nominalRate)
                        .arg(text(stream.color.pixelFormat)).arg(stream.color.bitDepth));
                    colorSummary = tr("色彩：%1 / %2 / %3 / %4")
                        .arg(text(stream.color.primaries),text(stream.color.transfer),text(stream.color.matrix),text(stream.color.range));
                    if (stream.color.hdrTransfer) colorSummary += QStringLiteral(" · ") + tr("HDR 传递函数");
                    if (stream.color.masteringDisplay) {
                        QStringList mastering;
                        if (stream.color.masteringDisplay->maxLuminance)
                            mastering.push_back(tr("峰值亮度 %1 cd/m²").arg(text(stream.color.masteringDisplay->maxLuminance->toString())));
                        if (stream.color.masteringDisplay->minLuminance)
                            mastering.push_back(tr("最低亮度 %1 cd/m²").arg(text(stream.color.masteringDisplay->minLuminance->toString())));
                        if (!mastering.isEmpty()) colorSummary += QStringLiteral(" · ") + tr("显示器元数据：%1").arg(mastering.join(QStringLiteral("，")));
                    }
                    if (stream.color.contentLight)
                        colorSummary += QStringLiteral(" · ") + tr("内容亮度 MaxCLL %1 / MaxFALL %2 cd/m²")
                            .arg(stream.color.contentLight->maxContentLight).arg(stream.color.contentLight->maxFrameAverageLight);
                    if (stream.rotationDegrees)
                        colorSummary += QStringLiteral(" · ") + tr("显示旋转 %1°").arg(QString::number(*stream.rotationDegrees,'f',2));
                    if (stream.displayMatrix) colorSummary += QStringLiteral(" · ") + tr("含显示变换矩阵");
                } else if (stream.type == "audio") {
                    summary.push_back(tr("%1 Hz · %2 声道 · %3 · %4")
                        .arg(stream.sampleRate > 0 ? QString::number(stream.sampleRate) : tr("未知"))
                        .arg(stream.channels > 0 ? QString::number(stream.channels) : tr("未知"))
                        .arg(text(stream.channelLayout).isEmpty() ? tr("声道布局未知") : text(stream.channelLayout))
                        .arg(text(stream.sampleFormat).isEmpty() ? tr("采样格式未知") : text(stream.sampleFormat)));
                }
                if (stream.bitRate > 0) summary.push_back(tr("码率 %1 kb/s").arg(stream.bitRate / 1000));
                QStringList metadataSummary;
                if (const auto language = stream.metadata.find("language"); language != stream.metadata.end())
                    metadataSummary.push_back(tr("语言：%1").arg(text(language->second)));
                if (const auto title = stream.metadata.find("title"); title != stream.metadata.end())
                    metadataSummary.push_back(tr("标题：%1").arg(text(title->second)));
                QVariantMap detail{{"index",stream.index},{"heading",heading},{"summary",summary.join(QStringLiteral(" · "))},
                    {"color",colorSummary},{"metadata",metadataSummary.join(QStringLiteral(" · "))}};
                streamDetails.push_back(detail);
            }
            media_["streamDetails"] = streamDetails;
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
            const auto softwareFallback = automaticSoftwareEncoder(encoders);
            media_["nvencContextOpened"] = nvenc.value("contextOpened").toBool();
            media_["softwareH264ContextOpened"] = softwareH264.value("contextOpened").toBool();
            media_["automaticSoftwareEncoder"] = softwareFallback;
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
                media_["inputProfile"] = text(event.sourceInputProfile);
                if (stream.duration) media_["duration"] = seconds(*stream.duration);
                else if (event.media->duration) media_["duration"] = seconds(*event.media->duration);
                if (stream.start) media_["start"] = seconds(*stream.start);
            }
            if (event.device) media_["gpu"] = text(event.device->name);
            else media_["gpu"] = tr("NVIDIA GPU 不可用");
            media_["gpuAvailable"] = event.device.has_value();
            media_["streams"] = static_cast<int>(event.media->streams.size());
            if (hasVideo && !media_.value("nvencContextOpened").toBool()) {
                if (!softwareFallback.isEmpty()) {
                    const auto reason = media_.value("nvencReason").toString();
                    const bool apiMismatch = reason.contains(QStringLiteral("Function not implemented"),Qt::CaseInsensitive) ||
                        reason.contains(QStringLiteral("minimum required Nvidia driver"),Qt::CaseInsensitive) ||
                        reason.contains(QStringLiteral("required nvenc API version"),Qt::CaseInsensitive);
                    notices.push_back(apiMismatch
                        ? encoderFallbackNotice(reason,softwareFallback)
                        : tr("NVENC 当前无法初始化；自动编码将使用%1，RIFE 插帧仍使用 GPU。")
                            .arg(softwareEncoderLabel(softwareFallback)));
                } else {
                    notices.push_back(tr("NVENC 和兼容的软件编码器均未能初始化；请检查本机视频编码运行环境。"));
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
                notices.push_back(tr("当前模型路径支持明确标记的 BT.709 或 BT.470BG/SMPTE 170M 8/10-bit SDR；HDR、其他位深和未标记色彩需要单独验证。"));
            detail_ = notices.join('\n');
            if (!hasVideo) { media_.clear(); status_ = "failed"; detail_ = tr("文件中没有可处理的视频轨。 "); }
        } else if (event.kind == Kind::completed && event.result) {
            status_ = "completed";
            const auto& result = *event.result;
            rifelove::logging::write(rifelove::logging::Level::info,"studio","processing.completed",
                "Interpolation output passed media and timeline validation",
                {{"encoder",result.video.encoder},{"output_frames",static_cast<std::uint64_t>(result.video.framesSubmitted)},
                    {"rife_frames",static_cast<std::uint64_t>(result.aiFrames)},{"processing_fps",result.processingFps}});
            progress_["frames"] = QVariant::fromValue<qulonglong>(result.video.framesSubmitted);
            progress_["aiFrames"] = QVariant::fromValue<qulonglong>(result.aiFrames);
            progress_["fps"] = result.processingFps;
            progress_["timings"] = timingMap(result.timings);
            progress_["encoder"] = text(result.video.encoder);
            detail_ = tr("已生成 %1 帧，其中 %2 帧由 RIFE 生成。输出已通过解码和时间轴检查。")
                .arg(result.video.framesSubmitted).arg(result.aiFrames);
            if (!result.encoderFallbackReason.empty()) {
                progress_["encoderFallbackReason"] = text(result.encoderFallbackReason);
            }
            if (!progress_.value("encoderFallbackReason").toString().isEmpty()) {
                detail_ += "\n" + encoderFallbackNotice(progress_.value("encoderFallbackReason").toString(),
                    text(result.video.encoder));
            }
            if (!result.inferenceFallbackReason.empty()) {
                progress_["inferenceFallbackReason"] = text(result.inferenceFallbackReason);
                detail_ += "\n" + tr("TensorRT 未能启动，已使用 ONNX Runtime CUDA 完成推理。");
            }
        } else if (event.kind == Kind::cancelled) { status_ = "cancelled"; detail_ = tr("操作已取消，未发布部分视频。"); }
        else {
            rifelove::logging::write(rifelove::logging::Level::error,"studio","processing.failed",event.detail);
            status_ = "failed"; detail_ = text(event.detail);
        }
        emit operationFinished(status_);
    }
    emit changed();
}
