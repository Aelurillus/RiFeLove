#include "task_list_model.hpp"
#include <QFileInfo>

int TaskListModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(rows_.size()); }
QVariant TaskListModel::data(const QModelIndex& item, int role) const {
    if (!item.isValid() || item.row() < 0 || item.row() >= rowCount()) return {};
    const auto& row = rows_[static_cast<std::size_t>(item.row())];
    switch (role) {
    case TaskId: return row.id; case SourcePath: return row.source; case OutputPath: return row.output;
    case TaskState: return row.state; case TargetRate: return row.targetRate; case ModelLabel: return row.model;
    case Stage: return row.stage; case Error: return row.error; case Frames: return QVariant::fromValue(row.frames);
    case AiFrames: return QVariant::fromValue(row.aiFrames); case EstimatedFrames: return QVariant::fromValue(row.estimatedFrames);
    case Speed: return row.speed; case FileName: return QFileInfo(row.source).fileName();
    default: return {};
    }
}
QHash<int,QByteArray> TaskListModel::roleNames() const {
    return {{TaskId,"taskId"},{SourcePath,"sourcePath"},{OutputPath,"outputPath"},{TaskState,"taskState"},{TargetRate,"targetRate"},
        {ModelLabel,"modelLabel"},{Stage,"stage"},{Error,"errorText"},{Frames,"frames"},{AiFrames,"aiFrames"},
        {EstimatedFrames,"estimatedFrames"},{Speed,"processingFps"},{FileName,"fileName"}};
}
void TaskListModel::indexRows() {
    index_.clear(); for (int i = 0; i < rowCount(); ++i) index_.insert(rows_[static_cast<std::size_t>(i)].id,i);
}
void TaskListModel::apply(std::vector<UiTaskRow> rows) {
    bool prefix = rows.size() >= rows_.size();
    for (std::size_t i = 0; prefix && i < rows_.size(); ++i) prefix = rows_[i].id == rows[i].id;
    if (!prefix) { beginResetModel(); rows_ = std::move(rows); indexRows(); endResetModel(); return; }
    const auto oldCount = rows_.size();
    for (std::size_t i = 0; i < oldCount; ++i) if (rows_[i] != rows[i]) {
        rows_[i] = std::move(rows[i]); emit dataChanged(index(static_cast<int>(i)),index(static_cast<int>(i)));
    }
    if (rows.size() > oldCount) {
        beginInsertRows({},static_cast<int>(oldCount),static_cast<int>(rows.size() - 1));
        for (auto i = oldCount; i < rows.size(); ++i) rows_.push_back(std::move(rows[i]));
        indexRows(); endInsertRows();
    }
}
void TaskListModel::progress(const QString& id, const QString& stage, qulonglong frames, qulonglong aiFrames, double speed) {
    const auto found = index_.constFind(id); if (found == index_.cend()) return;
    auto& row = rows_[static_cast<std::size_t>(*found)];
    row.stage = stage; row.frames = frames; row.aiFrames = std::max(row.aiFrames,aiFrames); row.speed = speed;
    row.state = stage == "paused" ? "paused" : "running"; row.error.clear();
    emit dataChanged(index(*found),index(*found),{Stage,Frames,AiFrames,Speed,TaskState,Error});
}
QString TaskListModel::stateForId(const QString& id) const {
    const auto found = index_.constFind(id); return found == index_.cend() ? QString() : rows_[static_cast<std::size_t>(*found)].state;
}
