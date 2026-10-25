#pragma once
#include "task_list_model.hpp"
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <stop_token>

class QueueWorker;
class QueueController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* tasks READ tasks CONSTANT)
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool importing READ importing NOTIFY changed)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString detail READ detail NOTIFY changed)
    Q_PROPERTY(QVariantMap counts READ counts NOTIFY changed)
    Q_PROPERTY(QVariantList recentTasks READ recentTasks NOTIFY changed)
    Q_PROPERTY(QVariantMap defaults READ defaults NOTIFY defaultsChanged)
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY profilesChanged)
    Q_PROPERTY(QVariantList models READ models NOTIFY modelsChanged)
    Q_PROPERTY(bool verifyingModels READ verifyingModels NOTIFY changed)
public:
    explicit QueueController(QString database, QObject* parent = nullptr);
    ~QueueController() override;
    QAbstractItemModel* tasks() { return &model_; }
    bool available() const { return available_; }
    bool busy() const { return busy_; }
    bool importing() const { return importing_; }
    QString state() const { return state_; }
    QString detail() const { return detail_; }
    QVariantMap counts() const { return counts_; }
    QVariantList recentTasks() const { return recentTasks_; }
    QVariantMap defaults() const { return defaults_; }
    QVariantList profiles() const { return profiles_; }
    QVariantList models() const { return models_; }
    bool verifyingModels() const { return verifyingModels_; }
    Q_INVOKABLE void addItems(const QList<QUrl>& sources, const QUrl& outputFolder, const QString& rate,
        const QUrl& model, const QUrl& runtime, const QString& encoder, const QString& container,
        const QVariantMap& encodingOptions, bool recursive);
    Q_INVOKABLE void start();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void cancelTask(const QString& id);
    Q_INVOKABLE void retryTask(const QString& id);
    Q_INVOKABLE void resumeTask(const QString& id);
    Q_INVOKABLE void restartTask(const QString& id);
    Q_INVOKABLE void reconnect();
    Q_INVOKABLE void openOutputFolder(const QString& id);
    Q_INVOKABLE void exportReport(const QUrl& destination);
    Q_INVOKABLE void saveDefaults(const QString& rate, const QUrl& model, const QUrl& runtime, const QUrl& output,
        const QString& encoder, const QString& container, const QVariantMap& encodingOptions);
    Q_INVOKABLE void saveProfile(const QString& name, const QVariantMap& values);
    Q_INVOKABLE void removeProfile(const QString& id);
    Q_INVOKABLE void applyProfile(const QString& id);
    Q_INVOKABLE void exportProfile(const QString& id, const QUrl& destination);
    Q_INVOKABLE void importProfile(const QUrl& source);
    Q_INVOKABLE void refreshModels();
    Q_INVOKABLE void addLocalModel(const QUrl& directory);
    Q_INVOKABLE void removeLocalModel(const QUrl& directory);
    Q_INVOKABLE void setDefaultModel(const QUrl& directory);
    Q_INVOKABLE bool supportsModelInputProfile(const QUrl& directory, const QString& profile) const;
    Q_INVOKABLE QUrl localUrl(const QString& path) const { return QUrl::fromLocalFile(path); }
    Q_INVOKABLE QString localPath(const QUrl& url) const { return url.isLocalFile() ? url.toLocalFile() : QString(); }
signals:
    void changed();
    void defaultsChanged();
    void profilesChanged();
    void profileApplied(const QVariantMap& values);
    void profileOperationFinished(bool success, const QString& operation);
    void modelsChanged();
    void operationFinished(const QString& state);
private:
    friend class QueueWorker;
    void setError(const QString& text);
    void snapshot(std::vector<UiTaskRow> rows);
    QString database_;
    TaskListModel model_;
    QThread thread_;
    QueueWorker* worker_ = nullptr;
    bool available_ = false, busy_ = false, importing_ = false;
    QString state_ = "loading", detail_;
    QVariantMap counts_, defaults_;
    QVariantList profiles_;
    QVariantList recentTasks_;
    QVariantList models_;
    bool verifyingModels_ = false;
    std::stop_source importCancel_;
};
