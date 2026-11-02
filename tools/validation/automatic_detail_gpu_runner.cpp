#include "application/task_queue.hpp"
#include "application/job_serialization.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace rifelove;
using namespace rifelove::application;
using namespace std::chrono_literals;

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(),value.end()};
}

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class TracingRestorationBackend final : public restoration::IVideoRestorationBackend {
public:
    TracingRestorationBackend(std::shared_ptr<restoration::IVideoRestorationBackend> backend,
                              std::ofstream& trace)
        : backend_(std::move(backend)),trace_(trace) {}
    restoration::Capabilities capabilities() const override { return backend_->capabilities(); }
    std::future<restoration::Result> restore(restoration::Request request) override {
        require(!request.context.empty() && request.centerIndex < request.context.size() &&
            request.context[request.centerIndex].timestamp.has_value(),
            "Direct dual-model trace needs a timestamped center frame");
        trace_ << request.context[request.centerIndex].timestamp->toString() << '\t'
               << backend_->capabilities().modelId << '\n';
        require(static_cast<bool>(trace_),"Could not persist direct restoration call trace");
        return backend_->restore(std::move(request));
    }
private:
    std::shared_ptr<restoration::IVideoRestorationBackend> backend_;
    std::ofstream& trace_;
};

ProcessingJob makeJob(const std::filesystem::path& source, const std::filesystem::path& output,
                      const std::filesystem::path& rife, const std::filesystem::path& runtime,
                      const std::filesystem::path& deblur, const std::filesystem::path& superres,
                      bool automatic) {
    ProcessingJob job;
    job.source = utf8(source);
    job.output = utf8(output);
    job.modelDirectory = rife;
    job.runtimeDirectory = runtime;
    job.settings.targetRate = Rational(60);
    job.settings.encoding.encoder = "ffv1";
    job.settings.inferenceBackend = "cuda";
    if (!superres.empty()) job.restorationModelDirectory = superres;
    if (automatic) {
        if (superres.empty()) job.restorationModelDirectory = deblur;
        else job.deblurModelDirectory = deblur;
        job.settings.automaticDetailRestoration.emplace();
        job.settings.automaticDetailRestoration->minimumConfidence = 0.2;
        job.settings.automaticDetailRestoration->maximumStrengthChangePerSecond = 10.0;
    }
    return job;
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 7 || argc == 8,
            "Usage: rifelove-automatic-detail-gpu-runner MODE ROOT SOURCE RIFE_PACK RUNTIME DEBLUR_PACK [SUPERRES_PACK]");
        const std::wstring mode(argv[1]);
        const bool dual = mode == L"dual-reference";
        const bool directDual = mode == L"dual-direct";
        const bool superresOnly = mode == L"superres-reference";
        require(mode == L"baseline" || mode == L"reference" || mode == L"hold" ||
            mode == L"recover" || dual || directDual || superresOnly,
            "Unsupported automatic-detail validation mode");
        require((dual || directDual || superresOnly) == (argc == 8),
            "The dual and super-resolution modes require a separate SUPERRES_PACK argument");
        const std::filesystem::path root(argv[2]), source(argv[3]), rife(argv[4]), runtime(argv[5]), deblur(argv[6]);
        const std::filesystem::path superres = argc == 8 ? std::filesystem::path(argv[7]) : std::filesystem::path();
        require(std::filesystem::is_directory(root) && std::filesystem::is_regular_file(source),
            "Use an existing isolated directory and source fixture");
        if (directDual) {
            auto job = makeJob(source,root / "dual-direct.mkv",rife,runtime,deblur,superres,true);
            pinModelDescriptor(job);
            pinRestorationModelDescriptor(job);
            std::ofstream trace(root / "direct-stage-order.tsv",std::ios::binary);
            require(static_cast<bool>(trace),"Could not open direct restoration call trace");
            PauseGate pause;
            const auto result = executeProcessingJob(job,{},std::stop_token{},pause,
                [&](const ProcessingIdentity&,PipelineSettings& settings) {
                    require(settings.deblurRestorationBackend && settings.restorationBackend,
                        "Direct dual-model run did not load both restoration backends");
                    settings.deblurRestorationBackend = std::make_shared<TracingRestorationBackend>(
                        settings.deblurRestorationBackend,trace);
                    settings.restorationBackend = std::make_shared<TracingRestorationBackend>(
                        settings.restorationBackend,trace);
                });
            trace.flush();
            require(static_cast<bool>(trace),"Could not finish direct restoration call trace");
            std::cout << serializeResult(result) << '\n';
            return 0;
        }
        const bool baseline = mode == L"baseline";
        const bool reference = mode == L"reference";
        const bool hold = mode == L"hold";
        const auto database = root / (baseline ? L"baseline.sqlite" : reference ? L"reference.sqlite" :
            dual ? L"dual.sqlite" : superresOnly ? L"superres.sqlite" : L"resume.sqlite");
        TaskQueue queue(database,2);
        if (mode == L"recover") {
            const auto rows = queue.list();
            require(rows.size() == 1 && rows.front().task.state == TaskState::interrupted,
                "Expected exactly one interrupted automatic-detail task");
            queue.resumeCheckpoint(rows.front().task.id);
        } else {
            const auto output = root / (baseline ? L"baseline.mkv" : reference ? L"reference.mkv" :
                dual ? L"dual.mkv" : superresOnly ? L"superres.mkv" : L"resumed.mkv");
            (void)queue.enqueue({makeJob(source,output,rife,runtime,deblur,superres,!baseline && !superresOnly)});
        }
        std::ofstream stageOrder;
        if (dual) {
            stageOrder.open(root / "stage-order.tsv",std::ios::binary);
            require(static_cast<bool>(stageOrder),"Could not open isolated restoration stage trace");
        }
        std::atomic<bool> paused{false};
        std::atomic<bool> drained{false};
        std::string queueError;
        require(queue.start([&](QueueEvent event) {
            if (dual && event.processing.stage == "restoration" && event.processing.timestamp)
                stageOrder << event.processing.timestamp->toString() << '\t' << event.processing.detail << '\n';
            if (hold && event.processing.stage == "processing" && event.processing.outputFrames >= 4 && !paused.load())
                queue.pause();
            if (hold && event.processing.stage == "paused") {
                std::ofstream marker(root / "paused.marker",std::ios::binary);
                marker << event.taskId;
                marker.close();
                paused.store(true);
            }
            if (event.drained) {
                queueError = event.queueError;
                drained.store(true);
            }
        }),"Could not start isolated queue");
        if (hold) {
            const auto deadline = std::chrono::steady_clock::now() + 180s;
            while (!paused.load() && !drained.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(25ms);
            require(paused.load(),"Automatic-detail worker did not pause after committed segments: " + queueError);
            require(queue.list().front().task.state == TaskState::paused,"Paused marker preceded durable task state");
            std::cout << "paused_after_committed_segments\n" << std::flush;
            for (;;) std::this_thread::sleep_for(1s); // The orchestrator terminates this process to exercise recovery.
        }
        queue.wait();
        if (dual) {
            stageOrder.flush();
            require(static_cast<bool>(stageOrder),"Could not persist restoration stage trace");
        }
        require(queueError.empty(),"Queue worker failed: " + queueError);
        const auto rows = queue.list();
        require(rows.size() == 1 && rows.front().task.state == TaskState::completed,
            "Isolated queue did not complete: " + (rows.empty() ? std::string("no row") : rows.front().task.error));
        require(!rows.front().result.empty(),"Completed queue result is missing");
        std::cout << rows.front().result << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
