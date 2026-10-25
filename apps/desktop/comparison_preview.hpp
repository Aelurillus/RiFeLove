#pragma once

#include "media/media_engine.hpp"

#include <QImage>
#include <QMutex>
#include <QObject>
#include <QQuickImageProvider>
#include <QHash>
#include <QStringList>
#include <QUrl>

#include <cstdint>
#include <functional>
#include <optional>
#include <thread>

class ComparisonFrameImageProvider final : public QQuickImageProvider {
public:
    ComparisonFrameImageProvider();
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
    void storePair(const QString& token, QImage original, QImage processed);
private:
    QMutex mutex_;
    QHash<QString,QImage> images_;
    QStringList insertionOrder_;
};

class ComparisonPreviewController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(QString errorString READ errorString NOTIFY changed)
    Q_PROPERTY(QString navigationErrorString READ navigationErrorString NOTIFY changed)
    Q_PROPERTY(QUrl originalFrameUrl READ originalFrameUrl NOTIFY changed)
    Q_PROPERTY(QUrl processedFrameUrl READ processedFrameUrl NOTIFY changed)
    Q_PROPERTY(double outputTimestampMs READ outputTimestampMs NOTIFY changed)
public:
    using FolderOpener = std::function<bool(const QUrl&)>;
    explicit ComparisonPreviewController(ComparisonFrameImageProvider* provider, QObject* parent = nullptr,
        FolderOpener folderOpener = {});
    ~ComparisonPreviewController() override;

    bool busy() const { return busy_; }
    bool ready() const { return ready_; }
    QString errorString() const { return errorString_; }
    QString navigationErrorString() const { return navigationErrorString_; }
    QUrl originalFrameUrl() const { return originalFrameUrl_; }
    QUrl processedFrameUrl() const { return processedFrameUrl_; }
    double outputTimestampMs() const { return outputTimestampMs_; }

    Q_INVOKABLE void requestFrames(const QUrl& original, const QUrl& processed, double positionMs);
    Q_INVOKABLE void stepFrame(int direction);
    Q_INVOKABLE void clear();
    Q_INVOKABLE bool openContainingFolder(const QUrl& fileUrl);

signals:
    void changed();
    void frameReady(double outputPositionMs);

private:
    void requestAt(rifelove::Rational outputRelativeTime);
    ComparisonFrameImageProvider* provider_{};
    FolderOpener folderOpener_;
    std::jthread worker_;
    std::uint64_t generation_ = 0;
    bool busy_ = false;
    bool ready_ = false;
    QString errorString_;
    QString navigationErrorString_;
    QUrl originalFrameUrl_;
    QUrl processedFrameUrl_;
    double outputTimestampMs_ = 0.0;
    QString originalPath_;
    QString processedPath_;
    std::optional<rifelove::Rational> processedStart_;
    std::optional<rifelove::Rational> processedRate_;
    std::optional<rifelove::Rational> processedFrameTimestamp_;
    std::optional<rifelove::Rational> processedFrameDuration_;
    std::optional<rifelove::Rational> processedPreviousFrameTimestamp_;
    std::optional<rifelove::Rational> processedNextFrameTimestamp_;
    std::optional<rifelove::Rational> processedDuration_;
};
