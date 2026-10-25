#include "comparison_preview.hpp"

#include <QDesktopServices>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
std::string utf8(const QString& value) {
    const auto bytes = value.toUtf8();
    return {bytes.constData(),static_cast<std::size_t>(bytes.size())};
}
QString providerUrl(const QString& token, const QString& side) {
    return QStringLiteral("image://comparison/%1-%2").arg(token,side);
}
const rifelove::media::StreamInfo& selectedVideo(const rifelove::media::MediaInfo& media) {
    const auto found = std::find_if(media.streams.begin(),media.streams.end(),[&](const auto& stream) {
        return stream.index == media.preferredVideoStream && stream.type == "video";
    });
    if (found == media.streams.end()) throw std::runtime_error("No video stream is available for exact preview");
    return *found;
}
QImage toImage(const rifelove::media::DecodedPreviewFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.rgba.size() !=
        static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 4U)
        throw std::runtime_error("FFmpeg returned an invalid exact-preview image");
    const QImage view(frame.rgba.data(),frame.width,frame.height,frame.width * 4,QImage::Format_RGBA8888);
    return view.copy();
}
double milliseconds(rifelove::Rational value) {
    const long double scaled = value.toLongDouble() * 1000.0L;
    if (scaled > static_cast<long double>(std::numeric_limits<double>::max()) ||
        scaled < -static_cast<long double>(std::numeric_limits<double>::max()))
        throw std::overflow_error("exact-preview timestamp is outside the supported range");
    return static_cast<double>(scaled);
}
}

ComparisonFrameImageProvider::ComparisonFrameImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage ComparisonFrameImageProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize) {
    QImage result;
    {
        QMutexLocker lock(&mutex_);
        result = images_.value(id);
    }
    if (size) *size = result.size();
    if (!result.isNull() && requestedSize.isValid() && result.size() != requestedSize)
        result = result.scaled(requestedSize,Qt::KeepAspectRatio,Qt::SmoothTransformation);
    return result;
}

void ComparisonFrameImageProvider::storePair(const QString& token, QImage original, QImage processed) {
    QMutexLocker lock(&mutex_);
    const QString originalId = token + QStringLiteral("-source");
    const QString processedId = token + QStringLiteral("-output");
    images_.insert(originalId,std::move(original));
    images_.insert(processedId,std::move(processed));
    insertionOrder_.append(originalId);
    insertionOrder_.append(processedId);
    while (insertionOrder_.size() > 4) images_.remove(insertionOrder_.takeFirst());
}

ComparisonPreviewController::ComparisonPreviewController(ComparisonFrameImageProvider* provider, QObject* parent,
    FolderOpener folderOpener)
    : QObject(parent),provider_(provider),folderOpener_(std::move(folderOpener)) {
    if (!provider_) throw std::invalid_argument("comparison preview image provider is required");
}

ComparisonPreviewController::~ComparisonPreviewController() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

void ComparisonPreviewController::requestFrames(const QUrl& original, const QUrl& processed, double positionMs) {
    if (!original.isLocalFile() || !processed.isLocalFile() || !std::isfinite(positionMs)) {
        errorString_ = tr("请选择本机视频，并输入有效的预览时间。");
        ready_ = false;
        emit changed();
        return;
    }
    originalPath_ = original.toLocalFile();
    processedPath_ = processed.toLocalFile();
    const auto microseconds = static_cast<std::int64_t>(std::llround(std::clamp(positionMs,0.0,9.0e12) * 1000.0));
    requestAt(rifelove::Rational(microseconds,1'000'000));
}

void ComparisonPreviewController::stepFrame(int direction) {
    if (direction == 0 || originalPath_.isEmpty() || processedPath_.isEmpty()) return;
    if (!processedFrameTimestamp_ || !processedStart_) {
        requestAt(rifelove::Rational(0));
        return;
    }
    const auto adjacent = direction > 0 ? processedNextFrameTimestamp_ : processedPreviousFrameTimestamp_;
    auto target = adjacent ? *adjacent - *processedStart_ : rifelove::Rational(0);
    auto step = processedFrameDuration_.value_or(processedRate_ ? rifelove::Rational(1) / *processedRate_ : rifelove::Rational(1,60));
    if (step <= rifelove::Rational(0)) step = processedRate_ ? rifelove::Rational(1) / *processedRate_ : rifelove::Rational(1,60);
    if (!adjacent) {
        target = *processedFrameTimestamp_ - *processedStart_ + (direction > 0 ? step : rifelove::Rational(0) - step);
    }
    if (target < rifelove::Rational(0)) target = rifelove::Rational(0);
    if (processedDuration_ && target >= *processedDuration_) {
        const auto period = processedRate_ ? rifelove::Rational(1) / *processedRate_ : step;
        target = *processedDuration_ > period ? *processedDuration_ - period : rifelove::Rational(0);
    }
    requestAt(target);
}

void ComparisonPreviewController::clear() {
    ++generation_;
    if (worker_.joinable()) worker_.request_stop();
    busy_ = false;
    ready_ = false;
    errorString_.clear();
    navigationErrorString_.clear();
    originalFrameUrl_ = QUrl();
    processedFrameUrl_ = QUrl();
    processedStart_.reset();
    processedRate_.reset();
    processedFrameTimestamp_.reset();
    processedFrameDuration_.reset();
    processedPreviousFrameTimestamp_.reset();
    processedNextFrameTimestamp_.reset();
    processedDuration_.reset();
    emit changed();
}

bool ComparisonPreviewController::openContainingFolder(const QUrl& fileUrl) {
    if (!fileUrl.isLocalFile()) {
        navigationErrorString_ = tr("只能打开本机文件的位置。");
        emit changed();
        return false;
    }
    const QFileInfo file(fileUrl.toLocalFile());
    if (!file.exists() || !file.isFile()) {
        navigationErrorString_ = tr("文件不存在，无法打开所在位置。");
        emit changed();
        return false;
    }
    const auto folderUrl = QUrl::fromLocalFile(file.absolutePath());
    const bool opened = folderOpener_ ? folderOpener_(folderUrl) : QDesktopServices::openUrl(folderUrl);
    navigationErrorString_ = opened ? QString{} : tr("无法打开此文件的位置。");
    emit changed();
    return opened;
}

void ComparisonPreviewController::requestAt(rifelove::Rational outputRelativeTime) {
    if (originalPath_.isEmpty() || processedPath_.isEmpty()) return;
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    const auto generation = ++generation_;
    const auto sourcePath = originalPath_;
    const auto outputPath = processedPath_;
    busy_ = true;
    errorString_.clear();
    emit changed();
    worker_ = std::jthread([this,generation,sourcePath,outputPath,outputRelativeTime](std::stop_token stop) {
        try {
            const auto sourceInfo = rifelove::media::MediaEngine::probe(utf8(sourcePath),stop);
            const auto outputInfo = rifelove::media::MediaEngine::probe(utf8(outputPath),stop);
            const auto& sourceStream = selectedVideo(sourceInfo);
            const auto& outputStream = selectedVideo(outputInfo);
            const auto sourceStart = sourceStream.start.value_or(rifelove::Rational(0));
            const auto outputStart = outputStream.start.value_or(rifelove::Rational(0));
            const auto sourceFrame = rifelove::media::MediaEngine::decodeFrameNearTime(
                utf8(sourcePath),sourceStart + outputRelativeTime,sourceStream.index,stop);
            if (stop.stop_requested()) return;
            const auto outputFrame = rifelove::media::MediaEngine::decodeFrameNearTime(
                utf8(outputPath),outputStart + outputRelativeTime,outputStream.index,stop);
            if (stop.stop_requested()) return;
            auto originalImage = toImage(sourceFrame);
            auto processedImage = toImage(outputFrame);
            const auto outputPosition = outputFrame.timestamp - outputStart;
            const auto token = QString::number(generation);
            const auto outputRate = outputStream.averageRate && *outputStream.averageRate > rifelove::Rational(0)
                ? outputStream.averageRate : outputStream.nominalRate;
            const auto outputDuration = outputStream.duration;
            const auto outputFrameTimestamp = outputFrame.timestamp;
            const auto outputFrameDuration = outputFrame.duration;
            const auto outputPreviousFrameTimestamp = outputFrame.previousTimestamp;
            const auto outputNextFrameTimestamp = outputFrame.nextTimestamp;
            QMetaObject::invokeMethod(this,[this,generation,token,outputPosition,outputFrameTimestamp,outputFrameDuration,
                outputPreviousFrameTimestamp,outputNextFrameTimestamp,
                outputStart,outputRate,outputDuration,originalImage = std::move(originalImage),processedImage = std::move(processedImage)]() mutable {
                if (generation != generation_) return;
                provider_->storePair(token,std::move(originalImage),std::move(processedImage));
                originalFrameUrl_ = QUrl(providerUrl(token,QStringLiteral("source")));
                processedFrameUrl_ = QUrl(providerUrl(token,QStringLiteral("output")));
                processedStart_ = outputStart;
                processedRate_ = outputRate;
                processedFrameTimestamp_ = outputFrameTimestamp;
                processedFrameDuration_ = outputFrameDuration;
                processedPreviousFrameTimestamp_ = outputPreviousFrameTimestamp;
                processedNextFrameTimestamp_ = outputNextFrameTimestamp;
                processedDuration_ = outputDuration;
                outputTimestampMs_ = milliseconds(outputPosition);
                busy_ = false;
                ready_ = true;
                errorString_.clear();
                emit changed();
                emit frameReady(milliseconds(outputPosition));
            },Qt::QueuedConnection);
        } catch (const std::exception& error) {
            if (stop.stop_requested()) return;
            const auto message = QString::fromUtf8(error.what());
            QMetaObject::invokeMethod(this,[this,generation,message] {
                if (generation != generation_) return;
                busy_ = false;
                ready_ = false;
                errorString_ = message;
                emit changed();
            },Qt::QueuedConnection);
        }
    });
}
