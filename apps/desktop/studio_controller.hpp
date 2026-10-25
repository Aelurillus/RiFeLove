#pragma once
#include "application/processing_service.hpp"
#include "progress_watchdog.hpp"
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

class StudioController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString source READ source NOTIFY changed)
    Q_PROPERTY(QString output READ output NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString detail READ detail NOTIFY changed)
    Q_PROPERTY(bool progressStalled READ progressStalled NOTIFY changed)
    Q_PROPERTY(QVariantMap deviceInfo READ deviceInfo CONSTANT)
    Q_PROPERTY(QVariantMap media READ media NOTIFY changed)
    Q_PROPERTY(QVariantMap progress READ progress NOTIFY changed)
public:
    explicit StudioController(QObject* parent = nullptr);
    ~StudioController() override;
    bool busy() const { return busy_; }
    QString source() const { return source_; }
    QString output() const { return output_; }
    QString status() const { return status_; }
    QString detail() const { return detail_; }
    bool progressStalled() const { return progressStalled_; }
    QVariantMap deviceInfo() const { return deviceInfo_; }
    QVariantMap media() const { return media_; }
    QVariantMap progress() const { return progress_; }
    Q_INVOKABLE void openSource(const QUrl& url);
    Q_INVOKABLE void setOutput(const QUrl& url);
    Q_INVOKABLE void start(const QString& rate, const QUrl& modelDirectory, const QUrl& runtimeDirectory,
        const QString& encoder, const QVariantMap& encodingOptions, const QString& inferenceBackend = QStringLiteral("auto"),
        double cutMeanDifference = 0.25);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE QUrl localUrl(const QString& path) const;
signals:
    void changed();
    void operationFinished(const QString& state);
private:
    void receive(rifelove::application::ProcessingEvent event);
    rifelove::application::ProcessingService::Callback callback();
    void fail(const QString& detail);
    bool busy_ = false;
    QString source_, output_, status_ = "ready", detail_;
    std::optional<rifelove::Rational> sourceDurationEstimate_;
    std::optional<rifelove::media::MediaInfo> inspectedMedia_;
    QVariantMap deviceInfo_, media_, progress_;
    rifelove::application::ProcessingService service_;
    rifelove::desktop::ProgressWatchdog progressWatchdog_;
    QTimer progressWatchdogTimer_;
    bool progressStalled_ = false;
};
