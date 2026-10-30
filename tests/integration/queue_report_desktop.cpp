#include "queue_controller.hpp"
#include "database/task_store.hpp"
#include <nlohmann/json.hpp>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QUrl>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Predicate> bool waitFor(Predicate predicate, int timeoutMs = 10000) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents,25);
        QThread::msleep(5);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents,25);
    return predicate();
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);
    std::random_device random;
    const auto directory = std::filesystem::temp_directory_path() /
        ("rifelove-queue-report-" + std::to_string(random()) + "-" + std::to_string(random()));
    require(std::filesystem::create_directory(directory),"Could not reserve queue-report test directory");
    const auto database = directory / "queue.sqlite";
    const auto reportPath = directory / "queue-report.json";
    try {
        {
            rifelove::TaskStore writer(database.string());
            const auto completed = writer.createConfigured({{"source.mp4","output.mkv","{\"targetRate\":\"60\"}"}}).front();
            writer.transition(completed,rifelove::TaskState::running);
            writer.finish(completed,rifelove::TaskState::completed,
                "{\"outputFrames\":4,\"aiFrames\":2,\"processedFrames\":1,\"reusedFrames\":3,\"elapsedSeconds\":2.0,\"processingFps\":0.5,\"sceneCuts\":1}");
            const auto failed = writer.create("failed-source.mp4","failed-output.mkv");
            writer.transition(failed,rifelove::TaskState::running);
            writer.transition(failed,rifelove::TaskState::failed,"synthetic task failure");
        }
        {
            QueueController controller(QString::fromStdWString(database.wstring()));
            require(waitFor([&] { return controller.available(); }),"Desktop queue did not connect to its local database");
            bool exported = false;
            QString terminal;
            QObject::connect(&controller,&QueueController::operationFinished,[&](const QString& state) {
                if (state == "report_exported") exported = true;
                else if (state == "error") terminal = state;
            });
            controller.exportReport(QUrl::fromLocalFile(QString::fromStdWString(reportPath.wstring())));
            require(waitFor([&] { return exported || !terminal.isEmpty(); }),"Desktop queue report export did not finish");
            require(exported,"Desktop queue report export failed");
            std::ifstream input(reportPath,std::ios::binary);
            require(static_cast<bool>(input),"Desktop queue report file was not published");
            const auto report = nlohmann::json::parse(input);
            require(report.at("kind") == "rifelove_queue_report","Wrong report kind");
            require(report.at("summary").at("taskCount") == 2,"Report omitted a task");
            require(report.at("summary").at("states").at("completed") == 1,"Completed task count is wrong");
            require(report.at("summary").at("states").at("failed") == 1,"Failed task count is wrong");
            require(report.at("summary").at("completedOutputFrames") == 4,"Completed output frame total is wrong");
            require(report.at("summary").at("completedProcessedFrames") == 1,"Completed processed frame total is wrong");
            require(report.at("summary").at("completedReusedFrames") == 3,"Completed reused frame total is wrong");
            require(report.at("summary").at("aggregateCompletedProcessedFps") == 0.5,"Completed processed FPS is wrong");
            require(!report.at("qualityDiagnostics").at("perceptualMetricsMeasured").get<bool>(),
                "Report claimed unmeasured perceptual scores");
        }
        std::error_code error; std::filesystem::remove_all(directory,error);
        if (error) throw std::filesystem::filesystem_error("remove queue-report test data",directory,error);
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored; std::filesystem::remove_all(directory,ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
