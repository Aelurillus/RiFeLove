#include "queue_controller.hpp"
#include "encoding_settings.hpp"
#include "application/task_queue.hpp"
#include "application/job_serialization.hpp"
#include "application/queue_report.hpp"
#include "application/import_service.hpp"
#include "inference/model_pack.hpp"
#include "timeline/timeline.hpp"
#include "profile_store.hpp"
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStringList>
#include <set>
#include <algorithm>
#include <chrono>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
using namespace rifelove;
using namespace rifelove::application;
QString text(const std::string& value) { return QString::fromUtf8(value.data(),static_cast<qsizetype>(value.size())); }
std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }
std::filesystem::path path(const QString& value) { return std::filesystem::path(value.toStdWString()); }
QString canonical(const QString& value) { return QDir::toNativeSeparators(QFileInfo(value).absoluteFilePath()); }
struct PathLess {
    bool operator()(const QString& a, const QString& b) const {
        return CompareStringOrdinal(reinterpret_cast<LPCWCH>(a.utf16()),a.size(),reinterpret_cast<LPCWCH>(b.utf16()),b.size(),TRUE) == CSTR_LESS_THAN;
    }
};
bool samePath(const QString& a, const QString& b) {
    const auto left = canonical(a), right = canonical(b);
    return !PathLess{}(left,right) && !PathLess{}(right,left);
}
QJsonObject object(const std::string& value) { return QJsonDocument::fromJson(QByteArray::fromStdString(value)).object(); }
UiTaskRow uiRow(const ConfiguredTask& source) {
    UiTaskRow row; row.id = text(source.task.id); row.source = text(source.task.sourcePath); row.output = text(source.task.outputPath);
    row.state = text(toString(source.task.state)); row.error = text(source.task.error);
    const auto config = object(source.configuration), progress = object(source.progress), result = object(source.result);
    row.targetRate = config.value("targetRate").toString(); row.model = config.value("expectedModelId").toString();
    const auto duration = config.value("sourceDurationEstimate").toString();
    if (!duration.isEmpty() && !row.targetRate.isEmpty()) {
        try {
            if (const auto count = estimateOutputFrameCount(Rational::parse(utf8(duration)),Rational::parse(utf8(row.targetRate))))
                row.estimatedFrames = static_cast<qulonglong>(*count);
        } catch (const std::exception&) {}
    }
    row.stage = progress.value("stage").toString(); row.frames = progress.value("frames").toVariant().toULongLong();
    row.aiFrames = progress.value("aiFrames").toVariant().toULongLong(); row.speed = progress.value("processingFps").toDouble();
    if (source.task.state == TaskState::completed) {
        row.frames = result.value("outputFrames").toVariant().toULongLong(); row.aiFrames = result.value("aiFrames").toVariant().toULongLong();
        row.speed = result.value("processingFps").toDouble();
    }
    return row;
}
}

class QueueWorker final : public QObject {
    Q_OBJECT
public:
    QueueWorker(QueueController* target, QString database) : target_(target), database_(std::move(database)) {}
    void initialize() {
        if (queue_) return;
        try {
            if (!QDir().mkpath(QFileInfo(database_).absolutePath())) throw std::runtime_error("Cannot create local task directory");
            queue_ = std::make_unique<TaskQueue>(path(database_));
            settings_ = std::make_unique<QSettings>(QFileInfo(database_).dir().filePath("preferences.ini"),QSettings::IniFormat);
            settings_->setFallbacksEnabled(false);
            const auto defaults = readDefaults();
            const auto profiles = rifelove::desktop::LocalProfileStore(*settings_).list();
            const auto recovered = queue_->recoveredCount();
            const auto recoveredPublished = queue_->recoveredPublishedOutputCount();
            post([defaults,profiles,recovered,recoveredPublished](QueueController& c) {
                c.available_ = true; c.defaults_ = defaults; c.profiles_ = profiles; c.state_ = "ready";
                QStringList recoveryMessages;
                if (recoveredPublished)
                    recoveryMessages.push_back(tr("已校验并自动恢复 %1 个已发布结果。此处的恢复不需要重新补帧。").arg(recoveredPublished));
                if (recovered)
                    recoveryMessages.push_back(tr("发现 %1 个仍中断的任务。可校验已完成片段后继续，也可选择新文件名从头生成。").arg(recovered));
                c.detail_ = recoveryMessages.join('\n');
                emit c.defaultsChanged(); emit c.profilesChanged(); emit c.changed(); emit c.operationFinished("ready");
            });
            snapshot();
        } catch (const std::exception& error) {
            post([message=text(error.what())](QueueController& c) { c.available_ = false; c.setError(tr("任务库暂不可用：") + message); });
        }
    }
    void shutdown() {
        closing_ = true;
        if (queue_) { queue_->stop(); queue_->wait(); queue_.reset(); }
    }
    void start() { perform([&] {
        requireQueue();
        if (!queue_->start([this](QueueEvent event) {
            QMetaObject::invokeMethod(this,[this,event=std::move(event)]() mutable { receive(std::move(event)); },Qt::QueuedConnection);
        })) throw std::runtime_error("Queue is already running");
        post([](QueueController& c) { c.busy_ = true; c.state_ = "running"; c.detail_.clear(); emit c.changed(); });
    }); }
    void pause() { if (queue_ && queue_->busy()) queue_->pause(); }
    void resume() { if (queue_ && queue_->busy()) queue_->resume(); }
    void stop() { if (queue_) queue_->stop(); }
    void cancel(const QString& id) { perform([&] { requireQueue(); queue_->cancel(utf8(id)); snapshot(); }); }
    void retry(const QString& id) { perform([&] { requireQueue(); queue_->retry(utf8(id)); snapshot(); post([](QueueController& c) { c.state_ = "ready"; c.detail_ = tr("任务已重新加入队列。"); emit c.changed(); }); }); }
    void resumeCheckpoint(const QString& id) { perform([&] { requireQueue(); queue_->resumeCheckpoint(utf8(id)); snapshot(); post([](QueueController& c) { c.state_ = "ready"; c.detail_ = tr("已校验检查点并加入队列。开始后会从未完成片段继续。"); emit c.changed(); }); }); }
    void restart(const QString& id) { perform([&] {
        requireQueue();
        const auto rows = queue_->list();
        const auto row = std::find_if(rows.begin(),rows.end(),[&](const auto& value) { return value.task.id == utf8(id); });
        if (row == rows.end()) throw std::invalid_argument("Task no longer exists");
        const QFileInfo old(text(row->task.outputPath));
        std::set<QString,PathLess> used; for (const auto& item : rows) used.insert(canonical(text(item.task.outputPath)));
        const auto output = allocateOutput(old.dir(),old.completeBaseName() + "_restart",old.suffix(),used);
        (void)queue_->restart(utf8(id),utf8(output)); snapshot();
        post([output](QueueController& c) { c.state_ = "ready"; c.detail_ = tr("已加入从头生成的任务：") + output; emit c.changed(); });
    }); }
    void openFolder(const QString& id) { perform([&] {
        requireQueue(); for (const auto& row : queue_->list()) if (row.task.id == utf8(id)) {
            const auto folder = QUrl::fromLocalFile(QFileInfo(text(row.task.outputPath)).absolutePath());
            post([folder](QueueController&) { QDesktopServices::openUrl(folder); }); return;
        }
    }); }
    void exportReport(const QUrl& destination) { perform([&] {
        requireQueue();
        if (!destination.isLocalFile() || destination.toLocalFile().isEmpty())
            throw std::invalid_argument("Choose a local JSON report file");
        TaskStore store(utf8(database_),TaskStore::OpenMode::readOnly);
        const auto serialized = serializeQueueReport(utf8(database_),store.reportSnapshot());
        ProcessingReportFile report(path(destination.toLocalFile()));
        report.publish(serialized);
        const auto saved = QDir::toNativeSeparators(destination.toLocalFile());
        const bool running = queue_->busy();
        post([saved,running](QueueController& c) {
            if (!running) c.state_ = "ready";
            c.detail_ = tr("队列报告已保存：") + saved;
            emit c.changed(); emit c.operationFinished("report_exported");
        });
    }); }
    void saveDefaults(QString rate, QUrl model, QUrl runtime, QUrl output, QString encoder, QString container, QVariantMap encodingOptions) {
        if (!settings_) return;
        const auto warning = writeDefaults(rate,model,runtime,output,encoder,container,encodingOptions);
        if (!warning.isEmpty()) post([warning](QueueController& c) { c.detail_ = warning; emit c.changed(); });
    }
    void saveProfile(QString name, QVariantMap values) {
        try {
            if (!settings_) throw std::runtime_error("Local settings are unavailable");
            rifelove::desktop::LocalProfileStore store(*settings_);
            (void)store.save(name,values);
            const auto profiles = store.list();
            post([profiles](QueueController& c) { c.profiles_ = profiles; emit c.profilesChanged(); emit c.profileOperationFinished(true,QStringLiteral("save")); });
        } catch (const std::exception&) {
            post([](QueueController& c) { emit c.profileOperationFinished(false,QStringLiteral("save")); });
        }
    }
    void removeProfile(QString id) {
        try {
            if (!settings_) throw std::runtime_error("Local settings are unavailable");
            rifelove::desktop::LocalProfileStore store(*settings_);
            if (!store.remove(id)) throw std::invalid_argument("Local profile was not found");
            const auto profiles = store.list();
            post([profiles](QueueController& c) { c.profiles_ = profiles; emit c.profilesChanged(); emit c.profileOperationFinished(true,QStringLiteral("remove")); });
        } catch (const std::exception&) {
            post([](QueueController& c) { emit c.profileOperationFinished(false,QStringLiteral("remove")); });
        }
    }
    void exportProfile(QString id, QUrl destination) {
        try {
            if (!settings_) throw std::runtime_error("Local settings are unavailable");
            if (!destination.isLocalFile() || destination.toLocalFile().isEmpty())
                throw std::invalid_argument("Choose a local JSON profile file");
            rifelove::desktop::LocalProfileStore(*settings_).exportToFile(id,destination.toLocalFile());
            post([](QueueController& c) { emit c.profileOperationFinished(true,QStringLiteral("export")); });
        } catch (const std::exception&) {
            post([](QueueController& c) { emit c.profileOperationFinished(false,QStringLiteral("export")); });
        }
    }
    void importProfile(QUrl source) {
        try {
            if (!settings_) throw std::runtime_error("Local settings are unavailable");
            if (!source.isLocalFile() || source.toLocalFile().isEmpty())
                throw std::invalid_argument("Choose a local JSON profile file");
            rifelove::desktop::LocalProfileStore store(*settings_);
            (void)store.importFromFile(source.toLocalFile());
            const auto profiles = store.list();
            post([profiles](QueueController& c) {
                c.profiles_ = profiles;
                emit c.profilesChanged();
                emit c.profileOperationFinished(true,QStringLiteral("import"));
            });
        } catch (const std::exception&) {
            post([](QueueController& c) { emit c.profileOperationFinished(false,QStringLiteral("import")); });
        }
    }
    void refreshModels() {
        try {
            if (!settings_) throw std::runtime_error("Local model catalog is unavailable");
            settings_->sync();
            if (settings_->status() != QSettings::NoError) throw std::runtime_error("Could not read local model catalog settings");
            const auto storedPaths = settings_->value("modelCatalog/paths").toStringList();
            QStringList paths;
            for (const auto& value : storedPaths) {
                const auto normalized = canonical(value);
                if (std::none_of(paths.begin(),paths.end(),[&](const QString& path) { return samePath(path,normalized); })) paths.push_back(normalized);
            }
            const auto selected = settings_->value("model").toUrl();
            if (selected.isLocalFile()) {
                const auto selectedPath = canonical(selected.toLocalFile());
                if (std::none_of(paths.begin(),paths.end(),[&](const QString& path) { return samePath(path,selectedPath); })) paths.push_back(selectedPath);
            }
            if (paths != storedPaths) {
                settings_->setValue("modelCatalog/paths",paths); settings_->sync();
                if (settings_->status() != QSettings::NoError) throw std::runtime_error("Could not save the current model in the local catalog");
            }
            QVariantList entries;
            for (const auto& value : paths) entries.push_back(inspectModel(value));
            post([entries](QueueController& c) { c.models_ = entries; c.verifyingModels_ = false; c.state_ = "ready"; c.detail_.clear(); emit c.modelsChanged(); emit c.changed(); });
        } catch (const std::exception& error) {
            post([message=text(error.what())](QueueController& c) { c.verifyingModels_ = false; c.setError(message); });
        }
    }
    void addLocalModel(QString directory) {
        try {
            if (!settings_) throw std::runtime_error("Local model catalog is unavailable");
            settings_->sync();
            if (settings_->status() != QSettings::NoError) throw std::runtime_error("Could not read local model catalog settings");
            const auto normalizedPath = canonical(directory);
            auto entry = inspectModel(normalizedPath);
            if (!entry.value("valid").toBool()) throw std::invalid_argument(utf8(entry.value("error").toString()));
            auto paths = settings_->value("modelCatalog/paths").toStringList();
            for (auto& value : paths) value = canonical(value);
            if (std::none_of(paths.begin(),paths.end(),[&](const QString& value) { return samePath(value,normalizedPath); })) paths.push_back(normalizedPath);
            settings_->setValue("modelCatalog/paths",paths); settings_->sync();
            if (settings_->status() != QSettings::NoError) throw std::runtime_error("Could not save local model catalog");
            paths = settings_->value("modelCatalog/paths").toStringList();
            QVariantList entries; for (const auto& value : paths) entries.push_back(inspectModel(value));
            post([entries,id=entry.value("id").toString()](QueueController& c) {
                c.models_ = entries; c.verifyingModels_ = false; c.state_ = "ready"; c.detail_ = tr("已校验并加入本地模型：") + id;
                emit c.modelsChanged(); emit c.changed(); emit c.operationFinished("model_added");
            });
        } catch (const std::exception& error) {
            post([message=text(error.what())](QueueController& c) { c.verifyingModels_ = false; c.setError(message); });
        }
    }
    void removeLocalModel(QString directory) {
        try {
            if (!settings_) throw std::runtime_error("Local model catalog is unavailable");
            settings_->sync();
            if (settings_->status() != QSettings::NoError) throw std::runtime_error("Could not read local model catalog settings");
            const auto storedPaths = settings_->value("modelCatalog/paths").toStringList();
            QStringList paths;
            for (const auto& value : storedPaths) {
                const auto normalized = canonical(value);
                if (std::none_of(paths.begin(),paths.end(),[&](const QString& path) { return samePath(path,normalized); })) paths.push_back(normalized);
            }
            paths.erase(std::remove_if(paths.begin(),paths.end(),[&](const QString& value) { return samePath(value,directory); }),paths.end());
            settings_->setValue("modelCatalog/paths",paths); settings_->sync();
            if (settings_->status() != QSettings::NoError) throw std::runtime_error("Could not update local model catalog");
            QVariantList entries; for (const auto& value : paths) entries.push_back(inspectModel(value));
            const auto selected = settings_->value("model").toUrl();
            if (selected.isLocalFile() && std::none_of(paths.begin(),paths.end(),[&](const QString& value) { return samePath(value,selected.toLocalFile()); }))
                entries.push_back(inspectModel(selected.toLocalFile()));
            post([entries](QueueController& c) { c.models_ = entries; c.verifyingModels_ = false; c.state_ = "ready"; emit c.modelsChanged(); emit c.changed(); });
        } catch (const std::exception& error) {
            post([message=text(error.what())](QueueController& c) { c.verifyingModels_ = false; c.setError(message); });
        }
    }
    void importItems(QList<QUrl> inputs, QUrl output, QString rate, QUrl model, QUrl runtime, QString encoder, QString container,
        QVariantMap encodingOptions, bool recursive, std::stop_token stop) {
        try {
            requireQueue();
            if (!model.isLocalFile() || !runtime.isLocalFile() || (!output.isEmpty() && !output.isLocalFile()))
                throw std::invalid_argument("Select local model, runtime and output directories");
            ImportRequest request;
            for (const auto& input : inputs) {
                if (!input.isLocalFile()) throw std::invalid_argument("Only local files/folders can be imported");
                request.inputs.push_back(path(input.toLocalFile()));
            }
            if (!output.isEmpty()) request.outputDirectory = path(output.toLocalFile());
            request.container = utf8(container); request.recursive = recursive;
            request.settings.modelDirectory = path(model.toLocalFile()); request.settings.runtimeDirectory = path(runtime.toLocalFile());
            request.settings.settings.targetRate = Rational::parse(utf8(rate));
            request.settings.settings.encoding = desktop::makeVideoEncodingSettings(encoder,encodingOptions);
            auto last = std::chrono::steady_clock::now();
            const auto result = importVideos(*queue_,request,stop,[&](std::size_t done, std::size_t total) {
                const auto now = std::chrono::steady_clock::now();
                if (done != total && now - last < std::chrono::milliseconds(100)) return;
                last = now; post([done,total](QueueController& c) { c.detail_ = tr("正在检查视频 %1 / %2").arg(done).arg(total); emit c.changed(); });
            });
            QStringList skipped;
            for (const auto& rejected : result.rejected) {
                if (skipped.size() >= 50) break;
                const auto reason = rejected.reason == ImportRejection::unsupportedColor ? tr("视频色彩标签、像素格式或位深不在支持范围内。") :
                    rejected.reason == ImportRejection::unsupportedModelProfile ? tr("当前模型未声明支持这个视频输入配置。") :
                    rejected.reason == ImportRejection::unsupportedEncoder ? tr("所选视频编码器无法初始化：") + text(rejected.detail) :
                    rejected.reason == ImportRejection::noVideo ? tr("文件中没有视频轨。") : tr("无法读取媒体：") + text(rejected.detail);
                skipped.push_back(QString::fromStdWString(rejected.path.filename().wstring()) + ": " + reason);
            }
            if (result.taskIds.empty()) throw std::invalid_argument(utf8(tr("没有找到当前模型可处理的视频。\n") + skipped.join('\n')));
            output = QUrl::fromLocalFile(QString::fromStdWString(result.outputDirectory.wstring()));
            const auto warning = writeDefaults(rate,model,runtime,output,encoder,container,encodingOptions);
            snapshot();
            post([count=result.taskIds.size(),rejected=result.rejected.size(),skipped,warning](QueueController& c) {
                c.importing_ = false; c.state_ = "ready"; c.detail_ = tr("已加入 %1 个任务。输出使用新文件名。").arg(count);
                if (rejected) c.detail_ += tr("\n跳过 %1 个不支持的文件：\n").arg(rejected) + skipped.join('\n');
                if (rejected > 50) c.detail_ += tr("\n仅显示前 50 条详情。");
                if (!warning.isEmpty()) c.detail_ += "\n" + warning;
                emit c.changed(); emit c.operationFinished("imported");
            });
        } catch (const std::exception& error) {
            const bool cancelled = stop.stop_requested();
            post([cancelled,message=text(error.what())](QueueController& c) { c.importing_ = false; if (cancelled) { c.state_ = "ready"; c.detail_ = tr("导入已取消，未加入部分任务。"); emit c.changed(); emit c.operationFinished("import_cancelled"); } else c.setError(message); });
        }
    }

private:
    template<class F> void post(F callback) {
        auto* target = target_;
        QMetaObject::invokeMethod(target,[target,callback=std::move(callback)]() mutable { callback(*target); },Qt::QueuedConnection);
    }
    template<class F> void perform(F operation) {
        try { operation(); }
        catch (const std::exception& error) {
            const bool running = queue_ && queue_->busy();
            post([message=text(error.what()),running](QueueController& c) {
                c.busy_ = running;
                if (running) { c.detail_ = message; emit c.changed(); emit c.operationFinished("error"); }
                else c.setError(message);
            });
        }
    }
    void requireQueue() { if (!queue_) throw std::runtime_error("Local queue is not available"); }
    static QString allocateOutput(const QDir& folder, const QString& stem, const QString& extension, std::set<QString,PathLess>& used) {
        for (int index = 1; index <= 100000; ++index) {
            const auto name = stem + (index == 1 ? QString() : "_" + QString::number(index)) + "." + extension;
            const auto output = canonical(folder.filePath(name));
            if (!QFileInfo::exists(output) && used.insert(output).second) return output;
        }
        throw std::runtime_error("Could not allocate a new output filename");
    }
    static QVariantMap inspectModel(const QString& directory) {
        QVariantMap entry{{"directory",directory},{"valid",false}};
        try {
            const auto model = inference::loadModelPack(path(directory));
            QStringList validatedBackends;
            for (const auto& backend : model.validatedBackends) validatedBackends.push_back(text(backend));
            QStringList supportedProfiles;
            for (const auto& profile : model.supportedInputProfiles) supportedProfiles.push_back(text(profile));
            entry["valid"] = true; entry["id"] = text(model.id); entry["displayName"] = text(model.displayName);
            entry["family"] = text(model.family); entry["version"] = text(model.version); entry["sha256"] = text(model.sha256);
            entry["schemaVersion"] = model.schemaVersion; entry["licenseId"] = text(model.licenseId);
            entry["licenseSha256"] = text(model.licenseSha256); entry["numericalProfile"] = text(model.numericalProfile);
            entry["validatedBackends"] = validatedBackends; entry["supportedInputProfiles"] = supportedProfiles;
            entry["hdrQualified"] = model.hdrQualified; entry["metadataIntegrityVerified"] = model.metadataIntegrityVerified;
            entry["sourceRepository"] = text(model.sourceRepository); entry["upstreamCommit"] = text(model.upstreamCommit);
            entry["licenseScopeEvidence"] = text(model.licenseScopeEvidence);
            entry["sourceManifestSha256"] = text(model.sourceManifestSha256); entry["validationSha256"] = text(model.validationSha256);
            entry["directory"] = QString::fromStdWString(model.directory.wstring());
            entry["error"] = QString();
        } catch (const std::exception& error) { entry["error"] = text(error.what()); }
        return entry;
    }
    void snapshot() {
        std::vector<UiTaskRow> rows; if (queue_) for (const auto& row : queue_->list()) rows.push_back(uiRow(row));
        post([rows=std::move(rows)](QueueController& c) mutable { c.snapshot(std::move(rows)); });
    }
    void receive(QueueEvent event) {
        if (closing_) return;
        if (event.drained) {
            queue_->wait(); snapshot();
            post([error=text(event.queueError)](QueueController& c) {
                c.busy_ = false;
                if (!error.isEmpty()) { c.setError(error); return; }
                c.state_ = "idle"; c.detail_ = tr("队列已停止。已排队但未运行的任务会保留；中断任务可校验片段后继续或从头重启。");
                emit c.changed(); emit c.operationFinished("drained");
            }); return;
        }
        const auto& value = event.processing;
        if (value.kind == ProcessingEvent::Kind::completed || value.kind == ProcessingEvent::Kind::failed || value.kind == ProcessingEvent::Kind::cancelled) { snapshot(); return; }
        post([id=text(event.taskId),stage=text(value.stage),frames=value.outputFrames,ai=value.aiFrames,speed=value.processingFps](QueueController& c) {
            const auto previous = c.model_.stateForId(id);
            if (previous != "queued" && previous != "running" && previous != "paused") return;
            const auto state = stage == "paused" ? QString("paused") : QString("running");
            if (!previous.isEmpty() && previous != state) {
                c.counts_[previous] = std::max(0,c.counts_.value(previous).toInt() - 1);
                c.counts_[state] = c.counts_.value(state).toInt() + 1;
            }
            c.model_.progress(id,stage,frames,ai,speed);
            for (qsizetype index = 0; index < c.recentTasks_.size(); ++index) {
                auto item = c.recentTasks_[index].toMap();
                if (item.value("taskId").toString() != id) continue;
                item["taskState"] = state;
                item["stage"] = stage;
                item["frames"] = QVariant::fromValue<qulonglong>(frames);
                item["aiFrames"] = QVariant::fromValue<qulonglong>(ai);
                item["processingFps"] = speed;
                item["errorText"] = QString();
                c.recentTasks_[index] = item;
                break;
            }
            if (stage == "paused") c.state_ = "paused";
            else if (stage == "resumed") c.state_ = "running";
            emit c.changed();
        });
    }
    QVariantMap readDefaults() const {
        QVariantMap result;
        auto rate = settings_->value("rate","60").toString();
        try { if (Rational::parse(utf8(rate)) <= Rational(0)) rate = "60"; } catch (...) { rate = "60"; }
        result["rate"] = rate;
        for (const auto& key : {"model","runtime","output"}) { const auto url = settings_->value(key).toUrl(); result[key] = url.isLocalFile() ? url : QUrl(); }
        const auto encoder = settings_->value("encoder","auto").toString(), container = settings_->value("container","mkv").toString();
        result["encoder"] = QStringList{"auto","libopenh264","libkvazaar","libaom-av1","h264_nvenc"}.contains(encoder) ? encoder : QString("auto");
        result["container"] = QStringList{"mkv","mp4","mov"}.contains(container) ? container : QString("mkv");
        QVariantMap encodingOptions{{"bitrateKbps",settings_->value("encoding/bitrateKbps",8000)},
            {"qualityMode",settings_->value("encoding/qualityMode","bitrate")},
            {"quality",settings_->value("encoding/quality",32)}, {"gopSize",settings_->value("encoding/gopSize",120)}};
        try { (void)desktop::makeVideoEncodingSettings(result["encoder"].toString(),encodingOptions); }
        catch (const std::exception&) { encodingOptions = desktop::videoEncodingDefaults(); }
        for (auto it = encodingOptions.cbegin(); it != encodingOptions.cend(); ++it) result[it.key()] = it.value();
        return result;
    }
    QString writeDefaults(const QString& rate, const QUrl& model, const QUrl& runtime, const QUrl& output,
        const QString& encoder, const QString& container, const QVariantMap& encodingOptions) {
        if (!settings_) return tr("默认设置未保存。");
        try { (void)desktop::makeVideoEncodingSettings(encoder,encodingOptions); }
        catch (const std::exception&) { return tr("高级编码参数超出范围或与当前编码器不兼容，默认设置未保存。"); }
        settings_->setValue("rate",rate); settings_->setValue("model",model); settings_->setValue("runtime",runtime);
        if (!output.isEmpty()) settings_->setValue("output",output);
        settings_->setValue("encoder",encoder); settings_->setValue("container",container);
        for (auto it = encodingOptions.cbegin(); it != encodingOptions.cend(); ++it) settings_->setValue(QStringLiteral("encoding/") + it.key(),it.value());
        settings_->sync();
        if (settings_->status() != QSettings::NoError) return tr("任务可继续使用，但默认设置未能保存。");
        const auto values = readDefaults(); post([values](QueueController& c) { c.defaults_ = values; emit c.defaultsChanged(); }); return {};
    }
    QueueController* target_;
    QString database_;
    std::unique_ptr<TaskQueue> queue_;
    std::unique_ptr<QSettings> settings_;
    bool closing_ = false;
};

QueueController::QueueController(QString database, QObject* parent) : QObject(parent), database_(std::move(database)), model_(this) {
    worker_ = new QueueWorker(this,database_); worker_->moveToThread(&thread_);
    connect(&thread_,&QThread::finished,worker_,&QObject::deleteLater); thread_.start();
    reconnect();
}
QueueController::~QueueController() {
    importCancel_.request_stop();
    QMetaObject::invokeMethod(worker_,[worker=worker_] { worker->shutdown(); },Qt::BlockingQueuedConnection);
    thread_.quit(); thread_.wait();
}
void QueueController::setError(const QString& value) { state_ = "error"; detail_ = value; emit changed(); emit operationFinished("error"); }
void QueueController::snapshot(std::vector<UiTaskRow> rows) {
    QVariantMap counts{{"total",static_cast<int>(rows.size())}};
    for (const auto& row : rows) counts[row.state] = counts.value(row.state).toInt() + 1;
    QVariantList recent;
    const auto first = rows.size() > 4 ? rows.size() - 4 : 0;
    for (auto index = rows.size(); index > first; --index) {
        const auto& row = rows[index - 1];
        recent.push_back(QVariantMap{{"taskId",row.id},{"fileName",QFileInfo(row.source).fileName()},
            {"taskState",row.state},{"targetRate",row.targetRate},{"stage",row.stage},
            {"frames",QVariant::fromValue<qulonglong>(row.frames)},
            {"estimatedFrames",QVariant::fromValue<qulonglong>(row.estimatedFrames)},
            {"processingFps",row.speed},{"errorText",row.error}});
    }
    recentTasks_ = std::move(recent);
    counts_ = std::move(counts); model_.apply(std::move(rows)); emit changed();
}
void QueueController::reconnect() { if (available_ || busy_ || importing_) return; state_ = "loading"; emit changed(); QMetaObject::invokeMethod(worker_,[w=worker_] { w->initialize(); },Qt::QueuedConnection); }
void QueueController::addItems(const QList<QUrl>& sources, const QUrl& output, const QString& rate, const QUrl& model,
    const QUrl& runtime, const QString& encoder, const QString& container, const QVariantMap& encodingOptions, bool recursive) {
    if (!available_ || busy_ || importing_) { setError(tr("队列忙碌或尚未连接，请等待当前操作结束。")); return; }
    importing_ = true; state_ = "importing"; detail_.clear(); importCancel_ = std::stop_source(); const auto stop = importCancel_.get_token(); emit changed();
    QMetaObject::invokeMethod(worker_,[w=worker_,sources,output,rate,model,runtime,encoder,container,encodingOptions,recursive,stop] {
        w->importItems(sources,output,rate,model,runtime,encoder,container,encodingOptions,recursive,stop);
    },Qt::QueuedConnection);
}
void QueueController::start() { if (!available_ || busy_ || importing_) return; busy_ = true; state_ = "running"; detail_.clear(); emit changed(); QMetaObject::invokeMethod(worker_,[w=worker_] { w->start(); },Qt::QueuedConnection); }
void QueueController::pause() { if (!busy_) return; state_ = "pausing"; emit changed(); QMetaObject::invokeMethod(worker_,[w=worker_] { w->pause(); },Qt::QueuedConnection); }
void QueueController::resume() { if (!busy_) return; state_ = "resuming"; emit changed(); QMetaObject::invokeMethod(worker_,[w=worker_] { w->resume(); },Qt::QueuedConnection); }
void QueueController::stop() { if (importing_) importCancel_.request_stop(); if (busy_) { state_ = "stopping"; emit changed(); QMetaObject::invokeMethod(worker_,[w=worker_] { w->stop(); },Qt::QueuedConnection); } }
void QueueController::cancelTask(const QString& id) { if (available_) QMetaObject::invokeMethod(worker_,[w=worker_,id] { w->cancel(id); },Qt::QueuedConnection); }
void QueueController::retryTask(const QString& id) { if (available_ && !importing_) QMetaObject::invokeMethod(worker_,[w=worker_,id] { w->retry(id); },Qt::QueuedConnection); }
void QueueController::resumeTask(const QString& id) { if (available_ && !importing_) QMetaObject::invokeMethod(worker_,[w=worker_,id] { w->resumeCheckpoint(id); },Qt::QueuedConnection); }
void QueueController::restartTask(const QString& id) { if (available_ && !importing_) QMetaObject::invokeMethod(worker_,[w=worker_,id] { w->restart(id); },Qt::QueuedConnection); }
void QueueController::openOutputFolder(const QString& id) { if (available_) QMetaObject::invokeMethod(worker_,[w=worker_,id] { w->openFolder(id); },Qt::QueuedConnection); }
void QueueController::exportReport(const QUrl& destination) {
    if (!available_) { setError(tr("本地任务队列尚未连接，无法导出报告。")); return; }
    QMetaObject::invokeMethod(worker_,[w=worker_,destination] { w->exportReport(destination); },Qt::QueuedConnection);
}
void QueueController::saveDefaults(const QString& rate, const QUrl& model, const QUrl& runtime, const QUrl& output,
    const QString& encoder, const QString& container, const QVariantMap& encodingOptions) {
    if (available_) QMetaObject::invokeMethod(worker_,[w=worker_,rate,model,runtime,output,encoder,container,encodingOptions] {
        w->saveDefaults(rate,model,runtime,output,encoder,container,encodingOptions);
    },Qt::QueuedConnection);
}
void QueueController::saveProfile(const QString& name, const QVariantMap& values) {
    if (!available_ || busy_ || importing_) { emit profileOperationFinished(false,QStringLiteral("save")); return; }
    QMetaObject::invokeMethod(worker_,[w=worker_,name,values] { w->saveProfile(name,values); },Qt::QueuedConnection);
}
void QueueController::removeProfile(const QString& id) {
    if (!available_ || busy_ || importing_) { emit profileOperationFinished(false,QStringLiteral("remove")); return; }
    QMetaObject::invokeMethod(worker_,[w=worker_,id] { w->removeProfile(id); },Qt::QueuedConnection);
}
void QueueController::applyProfile(const QString& id) {
    if (!available_ || busy_ || importing_) { emit profileOperationFinished(false,QStringLiteral("apply")); return; }
    const auto found = std::find_if(profiles_.cbegin(),profiles_.cend(),[&](const QVariant& value) {
        const auto profile = value.toMap();
        return profile.value("id").toString() == id && profile.value("valid").toBool();
    });
    if (found == profiles_.cend()) { emit profileOperationFinished(false,QStringLiteral("apply")); return; }
    const auto profile = found->toMap();
    QVariantMap options{{"bitrateKbps",profile.value("bitrateKbps",8000)},
        {"qualityMode",profile.value("qualityMode","bitrate")},
        {"quality",profile.value("quality",32)}, {"gopSize",profile.value("gopSize",120)}};
    saveDefaults(profile.value("rate","60").toString(),profile.value("model").toUrl(),
        profile.value("runtime").toUrl(),profile.value("output").toUrl(),
        profile.value("encoder","auto").toString(),profile.value("container","mkv").toString(),options);
    emit profileApplied(profile);
    emit profileOperationFinished(true,QStringLiteral("apply"));
}
void QueueController::exportProfile(const QString& id, const QUrl& destination) {
    if (!available_ || busy_ || importing_) { emit profileOperationFinished(false,QStringLiteral("export")); return; }
    QMetaObject::invokeMethod(worker_,[w=worker_,id,destination] { w->exportProfile(id,destination); },Qt::QueuedConnection);
}
void QueueController::importProfile(const QUrl& source) {
    if (!available_ || busy_ || importing_) { emit profileOperationFinished(false,QStringLiteral("import")); return; }
    QMetaObject::invokeMethod(worker_,[w=worker_,source] { w->importProfile(source); },Qt::QueuedConnection);
}
void QueueController::refreshModels() {
    if (!available_ || busy_ || importing_ || verifyingModels_) return;
    verifyingModels_ = true; state_ = "verifying_models"; detail_ = tr("正在校验本地模型及 SHA-256…"); emit changed();
    QMetaObject::invokeMethod(worker_,[w=worker_] { w->refreshModels(); },Qt::QueuedConnection);
}
void QueueController::addLocalModel(const QUrl& directory) {
    if (!available_ || busy_ || importing_ || verifyingModels_) { setError(tr("队列忙碌或本地模型校验正在运行。")); return; }
    if (!directory.isLocalFile() || directory.toLocalFile().isEmpty()) { setError(tr("只能添加本机目录中的模型包。")); return; }
    verifyingModels_ = true; state_ = "verifying_models"; detail_ = tr("正在验证模型描述、许可证和 ONNX SHA-256…"); emit changed();
    QMetaObject::invokeMethod(worker_,[w=worker_,path=directory.toLocalFile()] { w->addLocalModel(path); },Qt::QueuedConnection);
}
void QueueController::removeLocalModel(const QUrl& directory) {
    if (!available_ || busy_ || importing_ || verifyingModels_ || !directory.isLocalFile()) return;
    verifyingModels_ = true; state_ = "updating_models"; detail_ = tr("正在更新本地模型列表…"); emit changed();
    QMetaObject::invokeMethod(worker_,[w=worker_,path=directory.toLocalFile()] { w->removeLocalModel(path); },Qt::QueuedConnection);
}
void QueueController::setDefaultModel(const QUrl& directory) {
    if (!directory.isLocalFile() || directory.toLocalFile().isEmpty()) { setError(tr("默认模型必须位于本机目录。")); return; }
    const auto target = QFileInfo(directory.toLocalFile()).absoluteFilePath();
    const bool verified = std::any_of(models_.begin(),models_.end(),[&](const QVariant& value) {
        const auto entry = value.toMap();
        return entry.value("valid").toBool() && QFileInfo(entry.value("directory").toString()).absoluteFilePath().compare(target,Qt::CaseInsensitive) == 0;
    });
    if (!verified) { setError(tr("请先校验该模型包，再将其设为默认模型。")); return; }
    saveDefaults(defaults_.value("rate","60").toString(),directory,defaults_.value("runtime").toUrl(),
        defaults_.value("output").toUrl(),defaults_.value("encoder","auto").toString(),defaults_.value("container","mkv").toString(),
        {{"bitrateKbps",defaults_.value("bitrateKbps",8000)}, {"qualityMode",defaults_.value("qualityMode","bitrate")},
         {"quality",defaults_.value("quality",32)}, {"gopSize",defaults_.value("gopSize",120)}});
}
bool QueueController::supportsModelInputProfile(const QUrl& directory, const QString& profile) const {
    if (!directory.isLocalFile() || directory.toLocalFile().isEmpty() || profile.isEmpty()) return false;
    const auto selectedPath = directory.toLocalFile();
    for (const auto& value : models_) {
        const auto entry = value.toMap();
        if (!entry.value("valid").toBool() || !samePath(entry.value("directory").toString(),selectedPath)) continue;
        const auto profiles = entry.value("supportedInputProfiles").toStringList();
        if (profiles.contains(profile)) return true;
        return profile.endsWith(QStringLiteral("-sdr8")) && profiles.contains(QStringLiteral("legacy-unverified"));
    }
    return false;
}

#include "queue_controller.moc"
