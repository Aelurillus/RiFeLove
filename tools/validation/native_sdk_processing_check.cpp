// Developer-only SDK regression driver. Never included in the desktop package.
// Arguments: source output model-pack runtime backend encoder pixel-format
#include "application/job_serialization.hpp"
#include <chrono>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 8) {
        std::cerr << "Usage: native-sdk-processing-check source output model-pack runtime backend encoder pixel-format\n";
        return 2;
    }
    try {
        using namespace rifelove;
        using namespace rifelove::application;
        const auto utf8 = [](const std::filesystem::path& path) {
            const auto value = path.u8string();
            return std::string(value.begin(),value.end());
        };
        ProcessingJob job;
        job.source = utf8(argv[1]); job.output = utf8(argv[2]);
        job.modelDirectory = argv[3]; job.runtimeDirectory = argv[4];
        job.settings.targetRate = Rational(48);
        job.settings.inferenceBackend = utf8(argv[5]);
        job.settings.encoding.encoder = utf8(argv[6]);
        job.settings.encoding.pixelFormat = utf8(argv[7]);
        job.settings.encoding.encoderOptions["threads"] = "2";
        pinModelDescriptor(job);
        auto reportPath = std::filesystem::path(argv[2]); reportPath += L".json";
        ProcessingReportFile report(reportPath);
        ProcessingReportContext context;
        context.sourcePath = job.source; context.outputPath = job.output;
        context.targetFrameRate = "48/1"; context.encoding = job.settings.encoding;
        context.modelVersion = job.expectedModelVersion; context.modelSha256 = job.expectedModelSha256;
        PauseGate pause;
        const auto start = std::chrono::steady_clock::now();
        std::string stage;
        auto result = executeProcessingJob(job,[&](ProcessingEvent event) {
            if (event.identity) context.device = event.identity->device;
            if (event.stage != stage) {
                stage = event.stage;
                std::cout << stage << " " << event.outputFrames << " frames\n" << std::flush;
            }
        },{},pause);
        context.endToEndElapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        report.publish(serializeProcessingReport(context,result));
        std::cout << serializeResult(result) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
