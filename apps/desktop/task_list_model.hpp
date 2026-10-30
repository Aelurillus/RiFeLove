#pragma once
#include <QAbstractListModel>
#include <QHash>
#include <vector>

struct UiTaskRow {
    QString id, source, output, state, targetRate, model, stage, error;
    qulonglong frames = 0, aiFrames = 0;
    qulonglong estimatedFrames = 0;
    double speed = 0;
    bool operator==(const UiTaskRow&) const = default;
};
class TaskListModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { TaskId = Qt::UserRole + 1, SourcePath, OutputPath, TaskState, TargetRate, ModelLabel, Stage, Error, Frames, AiFrames, EstimatedFrames, Speed, FileName };
    explicit TaskListModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int,QByteArray> roleNames() const override;
    void apply(std::vector<UiTaskRow> rows);
    void progress(const QString& id, const QString& stage, qulonglong frames, qulonglong aiFrames, double speed);
    QString stateForId(const QString& id) const;
private:
    void indexRows();
    std::vector<UiTaskRow> rows_;
    QHash<QString,int> index_;
};
