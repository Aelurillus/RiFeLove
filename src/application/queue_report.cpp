#include "application/queue_report.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace rifelove::application {
namespace {
using Json = nlohmann::json;

Json payload(const std::string& value) { return value.empty() ? Json(nullptr) : Json::parse(value); }

std::uint64_t countValue(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_number_integer()) return 0;
    if (object.at(key).is_number_unsigned()) return object.at(key).get<std::uint64_t>();
    const auto value = object.at(key).get<std::int64_t>();
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

void add(std::uint64_t& total, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total)
        throw std::overflow_error("Queue report counter exceeds the supported range");
    total += value;
}

double finiteNumber(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_number()) return 0;
    const auto value = object.at(key).get<double>();
    return std::isfinite(value) && value > 0 ? value : 0;
}

} // namespace

std::string serializeQueueReport(const std::string& databasePath, const std::vector<ConfiguredTask>& tasks) {
    if (databasePath.empty()) throw std::invalid_argument("Queue report requires a database path");
    Json states = {{"queued",0},{"running",0},{"paused",0},{"interrupted",0},{"failed",0},{"cancelled",0},{"completed",0}};
    Json taskReports = Json::array();
    Json failures = Json::array();
    std::uint64_t currentOutputFrames = 0, currentAiFrames = 0;
    std::uint64_t completedOutputFrames = 0, completedAiFrames = 0;
    std::uint64_t sceneCuts = 0, duplicatePairs = 0, abnormalMotionPairs = 0;
    std::uint64_t timedCompletedFrames = 0;
    double timedCompletedSeconds = 0;

    for (const auto& task : tasks) {
        const auto state = toString(task.task.state);
        states[state] = states.at(state).get<std::uint64_t>() + 1;
        const auto configuration = payload(task.configuration);
        const auto identity = payload(task.identity);
        const auto progress = payload(task.progress);
        const auto result = payload(task.result);
        const bool completed = task.task.state == TaskState::completed;
        const auto frames = completed ? countValue(result,"outputFrames") : countValue(progress,"frames");
        const auto aiFrames = completed ? countValue(result,"aiFrames") : countValue(progress,"aiFrames");
        add(currentOutputFrames,frames); add(currentAiFrames,aiFrames);
        if (completed) {
            add(completedOutputFrames,frames); add(completedAiFrames,aiFrames);
            add(sceneCuts,countValue(result,"sceneCuts"));
            add(duplicatePairs,countValue(result,"duplicatePairs"));
            add(abnormalMotionPairs,countValue(result,"abnormalPairs"));
            const auto elapsed = finiteNumber(result,"elapsedSeconds");
            if (elapsed > 0 && frames > 0) {
                add(timedCompletedFrames,frames);
                timedCompletedSeconds += elapsed;
            }
        }
        taskReports.push_back({
            {"id",task.task.id},{"source",task.task.sourcePath},{"output",task.task.outputPath},
            {"state",state},{"createdAt",task.task.createdAt},{"updatedAt",task.task.updatedAt},{"error",task.task.error},
            {"configuration",configuration},{"identity",identity},{"progress",progress},{"result",result}
        });
        if ((task.task.state == TaskState::failed || task.task.state == TaskState::interrupted) && !task.task.error.empty())
            failures.push_back({{"id",task.task.id},{"state",state},{"error",task.task.error}});
    }

    const double aggregateFps = timedCompletedSeconds > 0
        ? static_cast<double>(timedCompletedFrames) / timedCompletedSeconds : 0;
    const Json report{
        {"schemaVersion",1},{"kind","rifelove_queue_report"},{"sourceDatabase",databasePath},
        {"summary",{{"taskCount",tasks.size()},{"states",states},
            {"currentAttemptOutputFrames",currentOutputFrames},{"currentAttemptAiFrames",currentAiFrames},
            {"completedOutputFrames",completedOutputFrames},{"completedAiFrames",completedAiFrames},
            {"completedProcessingSeconds",timedCompletedSeconds},{"aggregateCompletedOutputFps",aggregateFps},
            {"sceneCuts",sceneCuts},{"duplicatePairs",duplicatePairs},{"abnormalMotionPairs",abnormalMotionPairs}}},
        {"qualityDiagnostics",{{"perceptualMetricsMeasured",false},
            {"note","No reference videos are attached to queue tasks; PSNR, SSIM and LPIPS are not measured."}}},
        {"failures",failures},{"tasks",taskReports}
    };
    return report.dump(2) + "\n";
}

} // namespace rifelove::application
