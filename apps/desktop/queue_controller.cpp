#include "queue_controller.hpp"
#include "application/task_queue.hpp"
#include "application/job_serialization.hpp"
#include "application/import_service.hpp"
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
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
QJsonObject object(const std::string& value) { return QJsonDocument::fromJson(QByteArray::fromStdString(value)).object(); }
UiTaskRow uiRow(const ConfiguredTask& source) {
    UiTaskRow row; row.id = text(source.task.id); row.source = text(source.task.sourcePath); row.output = text(source.task.outputPath);
    row.state = text(toString(source.task.state)); row.error = text(source.task.error);
    const auto config = object(source.configuration), progress = object(source.progress), result = object(source.result);
    row.targetRate = config.value("targetRate").toString(); row.model = config.value("expectedModelId").toString();
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
public:
    QueueWorker(QueueController* target, QString database) : target_(target), database_(std::move(database)) {}
    void initialize() {
        if (queue_) return;
        try {
            if (!QDir().mkpath(QFileInfo(database_).absolutePath())) throw std::runtime_error("Cannot create local task directory");
            queue_ = std::make_unique<TaskQueue>(path(database_));
            settings_ = std::make_unique<QSettings>(QFileInfo(database_).dir().filePath("preferences.ini"),QSettings::IniFormat);
            settings_->setFallbacksEnabled(false);
            const auto defaults = readDefaults(); const auto recovered = queue_->recoveredCount();
            post([defaults,recovered](QueueController& c) {
                c.available_ = true; c.defaults_ = defaults; c.state_ = "ready";
                c.detail_ = recovered ? tr("发现 %1 个中断任务。可选择新文件名从头生成；分段续传仍在开发中。").arg(recovered) : QString();
                emit c.defaultsChanged(); emit c.changed(); emit c.operationFinished("ready");
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
    void saveDefaults(QString rate, QUrl model, QUrl runtime, QUrl output, QString encoder, QString container) {
        if (!settings_) return;
        const auto warning = writeDefaults(rate,model,runtime,output,encoder,container);
        if (!warning.isEmpty()) post([warning](QueueController& c) { c.detail_ = warning; emit c.changed(); });
    }
    void importItems(QList<QUrl> inputs, QUrl output, QString rate, QUrl model, QUrl runtime, QString encoder, QString container, bool recursive, std::stop_token stop) {
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
            request.settings.settings.targetRate = Rational::parse(utf8(rate)); request.settings.settings.encoding.encoder = utf8(encoder);
            auto last = std::chrono::steady_clock::now();
            const auto result = importVideos(*queue_,request,stop,[&](std::size_t done, std::size_t total) {
                const auto now = std::chrono::steady_clock::now();
                if (done != total && now - last < std::chrono::milliseconds(100)) return;
                last = now; post([done,total](QueueController& c) { c.detail_ = tr("正在检查视频 %1 / %2").arg(done).arg(total); emit c.changed(); });
            });
            QStringList skipped;
            for (const auto& rejected : result.rejected) {
                if (skipped.size() >= 50) break;
                const auto reason = rejected.reason == ImportRejection::unsupportedColor ? tr("当前模型仅支持明确标记的 BT.709 8-bit SDR。") :
                    rejected.reason == ImportRejection::noVideo ? tr("文件中没有视频轨。") : tr("无法读取媒体：") + text(rejected.detail);
                skipped.push_back(QString::fromStdWString(rejected.path.filename().wstring()) + ": " + reason);
            }
            if (result.taskIds.empty()) throw std::invalid_argument(utf8(tr("没有找到当前模型可处理的视频。\n") + skipped.join('\n')));
            output = QUrl::fromLocalFile(QString::fromStdWString(result.outputDirectory.wstring()));
            const auto warning = writeDefaults(rate,model,runtime,output,encoder,container);
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
                c.state_ = "idle"; c.detail_ = tr("队列已停止。已排队但未运行的任务会保留。中断任务需单独从头重启。");
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
        result["encoder"] = QStringList{"auto","libopenh264","libkvazaar","libaom-av1"}.contains(encoder) ? encoder : QString("auto");
        result["container"] = QStringList{"mkv","mp4","mov"}.contains(container) ? container : QString("mkv");
        return result;
    }
    QString writeDefaults(const QString& rate, const QUrl& model, const QUrl& runtime, const QUrl& output, const QString& encoder, const QString& container) {
        if (!settings_) return tr("默认设置未保存。");
        settings_->setValue("rate",rate); settings_->setValue("model",model); settings_->setValue("runtime",runtime);
        if (!output.isEmpty()) settings_->setValue("output",output);
        settings_->setValue("encoder",encoder); settings_->setValue("container",container); settings_->sync();
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
    counts_ = std::move(counts); model_.apply(std::move(rows)); emit changed();
}
void QueueController::reconnect() { if (available_ || busy_ || importing_) return; state_ = "loading"; emit changed(); QMetaObject::invokeMethod(worker_,[w=worker_] { w->initialize(); },Qt::QueuedConnection); }
void QueueController::addItems(const QList<QUrl>& sources, const QUrl& output, const QString& rate, const QUrl& model, const QUrl& runtime, const QString& encoder, const QString& container, bool recursive) {
    if (!available_ || busy_ || importing_) { setError(tr("队列忙碌或尚未连接，请等待当前操作结束。")); return; }
    importing_ = true; state_ = "importing"; detail_.clear(); importCancel_ = std::stop_source(); const auto stop = importCancel_.get_token(); emit changed();
    QMetaObject::invokeMethod(worker_,[w=worker_,sources,output,rate,model,runtime,encoder,container,recursive,stop] { w->importItems(sources,output,rate,model,runtime,encoder,container,recursive,stop); },Qt::QueuedConnection);
}
void QueueController::start() { if (!available_ || busy_ || importing_) return; busy_ = true; state_ = "running"; detail_.clear(); emit changed(); QMetaObject::invokeMethod(worker_,[w=worker_] { w->start(); },Qt::QueuedConnection); }
void QueueController::pause() { if (!busy_) return; state_ = "pausing"; emit changed(); QMetaObject::invokeMethod(worker_,[w=worker_] { w->pause(); },Qt::QueuedConnection); }
void QueueController::resume() { if (!busy_) return; state_ = "resuming"; emit changed(); QMetaObject::invokeMethod(worker_,[w=worker_] { w->resume(); },Qt::QueuedConnection); }
void QueueController::stop() { if (importing_) importCancel_.request_stop(); if (busy_) { state_ = "stopping"; emit changed(); QMetaObject::invokeMethod(worker_,[w=worker_] { w->stop(); },Qt::QueuedConnection); } }
void QueueController::cancelTask(const QString& id) { if (available_) QMetaObject::invokeMethod(worker_,[w=worker_,id] { w->cancel(id); },Qt::QueuedConnection); }
void QueueController::retryTask(const QString& id) { if (available_ && !importing_) QMetaObject::invokeMethod(worker_,[w=worker_,id] { w->retry(id); },Qt::QueuedConnection); }
void QueueController::restartTask(const QString& id) { if (available_ && !importing_) QMetaObject::invokeMethod(worker_,[w=worker_,id] { w->restart(id); },Qt::QueuedConnection); }
void QueueController::openOutputFolder(const QString& id) { if (available_) QMetaObject::invokeMethod(worker_,[w=worker_,id] { w->openFolder(id); },Qt::QueuedConnection); }
void QueueController::saveDefaults(const QString& rate, const QUrl& model, const QUrl& runtime, const QUrl& output, const QString& encoder, const QString& container) {
    if (available_) QMetaObject::invokeMethod(worker_,[w=worker_,rate,model,runtime,output,encoder,container] { w->saveDefaults(rate,model,runtime,output,encoder,container); },Qt::QueuedConnection);
}
