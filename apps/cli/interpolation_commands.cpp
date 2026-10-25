#include "interpolation_commands.hpp"
#include "local_profile.hpp"
#include "common/json.hpp"
#include "core/pipeline/video_pipeline.hpp"
#include "application/job_serialization.hpp"
#include "inference/onnx_cuda_backend.hpp"
#include "console_cancellation.hpp"
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
using namespace rifelove;
using rifelove::cli::ConsoleCancellation;
std::int64_t integer(const std::string& value) {
    std::int64_t number = 0;
    const auto [end,status] = std::from_chars(value.data(),value.data() + value.size(),number);
    if (status != std::errc{} || end != value.data() + value.size()) throw std::invalid_argument("Invalid integer option");
    return number;
}
std::filesystem::path path(const std::string& text) { return std::filesystem::path(std::u8string(text.begin(),text.end())); }
}
int runInterpolationCommand(const std::vector<std::string>& args) {
    using namespace rifelove;
    if (args.size() < 4) throw std::invalid_argument("interpolate requires INPUT OUTPUT --fps RATE --model-dir PACK --runtime-dir DLL_DIRECTORY");
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); };
    PipelineSettings settings;
    std::filesystem::path modelDirectory, runtimeDirectory;
    std::optional<std::string> reportPathText;
    bool hasRate = false, showProgress = false;
    int ordinal = 0;
    for (std::size_t i = 4; i < args.size(); ++i) {
        const auto& option = args[i];
        if (option == "--progress") { showProgress = true; continue; }
        if (i + 1 >= args.size()) throw std::invalid_argument("Missing value for " + option);
        const auto& value = args[++i];
        if (option == "--profile") {
            const auto profile = cli::loadLocalProfile(path(value));
            cli::applyLocalProfile(profile,settings,modelDirectory,runtimeDirectory);
            cli::validateLocalProfileOutput(profile,path(args[3]));
            hasRate = true;
        }
        else if (option == "--fps") { settings.targetRate = Rational::parse(value); hasRate = true; }
        else if (option == "--report") reportPathText = value;
        else if (option == "--model-dir") modelDirectory = path(value);
        else if (option == "--runtime-dir") runtimeDirectory = path(value);
        else if (option == "--encoder") cli::overrideEncoder(settings,value);
        else if (option == "--pixel-format") settings.encoding.pixelFormat = value;
        else if (option == "--bitrate") cli::overrideBitrate(settings,integer(value));
        else if (option == "--gop-size") {
            const auto number = integer(value);
            if (number < 0 || number > 1000) throw std::invalid_argument("GOP size must be between 0 and 1000");
            settings.encoding.gopSize = static_cast<int>(number);
        }
        else if (option == "--bframes") {
            const auto number = integer(value);
            if (number < 0 || number > 16) throw std::invalid_argument("B-frame count must be between 0 and 16");
            settings.encoding.maxBFrames = static_cast<int>(number);
        } else if (option == "--gpu" || option == "--stream") {
            const auto number = integer(value);
            if (number < 0 || number > std::numeric_limits<int>::max()) throw std::invalid_argument("Invalid GPU/stream index");
            if (option == "--gpu") ordinal = static_cast<int>(number); else settings.encoding.videoStreamIndex = static_cast<int>(number);
        } else if (option == "--option") {
            const auto equal = value.find('=');
            if (equal == std::string::npos || equal == 0) throw std::invalid_argument("Encoder option must be KEY=VALUE");
            settings.encoding.encoderOptions[value.substr(0,equal)] = value.substr(equal + 1);
        } else if (option == "--cut-threshold") {
            double threshold = 0;
            const auto [end,status] = std::from_chars(value.data(),value.data() + value.size(),threshold);
            if (status != std::errc{} || end != value.data() + value.size() || !std::isfinite(threshold) || threshold < 0 || threshold > 1)
                throw std::invalid_argument("Cut threshold must be a finite number from zero to one");
            settings.scene.cutMeanDifference = threshold;
        } else throw std::invalid_argument("Unknown interpolate option: " + option);
    }
    if (!hasRate || settings.targetRate <= Rational(0) || modelDirectory.empty() || runtimeDirectory.empty()) throw std::invalid_argument("A positive --fps, --model-dir and --runtime-dir are required (or use --profile)");
    if (std::filesystem::exists(path(args[3]))) throw std::invalid_argument("Output already exists; choose a new path");
    std::unique_ptr<application::ProcessingReportFile> reportFile;
    if (reportPathText) {
        if (reportPathText->empty()) throw std::invalid_argument("--report requires a non-empty JSON file path");
        const auto reportPath = path(*reportPathText);
        const auto absoluteReport = std::filesystem::absolute(reportPath).lexically_normal().native();
        const auto absoluteOutput = std::filesystem::absolute(path(args[3])).lexically_normal().native();
        const auto absoluteSource = std::filesystem::absolute(path(args[2])).lexically_normal().native();
        if (CompareStringOrdinal(absoluteReport.c_str(),static_cast<int>(absoluteReport.size()),absoluteOutput.c_str(),static_cast<int>(absoluteOutput.size()),TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(absoluteReport.c_str(),static_cast<int>(absoluteReport.size()),absoluteSource.c_str(),static_cast<int>(absoluteSource.size()),TRUE) == CSTR_EQUAL)
            throw std::invalid_argument("Report path must be different from the input and video output paths");
        reportFile = std::make_unique<application::ProcessingReportFile>(reportPath);
    }
    ConsoleCancellation cancellation;
    const auto token = cancellation.token();
    try {
        if (showProgress) std::cout << "{\"event\":\"interpolation_progress\",\"stage\":\"loading_model\",\"output_frames\":0}\n" << std::flush;
        const auto pack = inference::loadModelPack(modelDirectory);
        auto device = std::make_shared<gpu::CudaDevice>(ordinal);
        inference::OnnxCudaConfig config; config.runtimeDirectory = runtimeDirectory;
        inference::OnnxCudaBackend backend(device,config);
        backend.loadModel(pack);
        const auto result = interpolateVideo(args[2],args[3],settings,*device,backend,showProgress ? std::function<void(const PipelineProgress&)>([&](const PipelineProgress& current) {
            const double seconds = elapsed();
            std::cout << "{\"event\":\"interpolation_progress\",\"stage\":" << jsonString(current.stage)
                << ",\"output_frames\":" << current.outputFrames << ",\"ai_frames\":" << current.aiFrames
                << ",\"pts\":" << (current.timestamp ? jsonString(current.timestamp->toString()) : "null")
                << ",\"encoder\":" << jsonString(current.encoder)
                << ",\"elapsed_seconds\":" << seconds << ",\"processing_fps\":" << (seconds > 0 ? static_cast<double>(current.outputFrames) / seconds : 0)
                << ",\"detail\":" << jsonString(current.detail) << "}\n" << std::flush;
        }) : std::function<void(const PipelineProgress&)>(),token);
        const auto seconds = elapsed();
        if (reportFile) {
            application::ProcessingReportContext report;
            report.sourcePath = args[2]; report.outputPath = args[3]; report.targetFrameRate = settings.targetRate.toString();
            report.modelVersion = pack.version; report.modelSha256 = pack.sha256; report.endToEndElapsedSeconds = seconds;
            report.device = device->info(); report.encoding = settings.encoding;
            reportFile->publish(application::serializeProcessingReport(report,result));
        }
        std::cout << "{\"event\":\"interpolation_completed\",\"model\":" << jsonString(result.modelId) << ",\"model_sha256\":" << jsonString(pack.sha256)
            << ",\"backend\":" << jsonString(result.backend) << ",\"encoder\":" << jsonString(result.video.encoder)
            << ",\"model_adapter\":" << jsonString(result.adapterId) << ",\"model_adapter_version\":" << result.adapterVersion
            << ",\"inference_fallback_reason\":" << jsonString(result.inferenceFallbackReason)
            << ",\"encoder_fallback_reason\":" << jsonString(result.encoderFallbackReason) << ",\"target_fps\":" << jsonString(settings.targetRate.toString())
            << ",\"source_frames\":" << result.sourceFrames << ",\"output_frames\":" << result.video.framesSubmitted << ",\"ai_frames\":" << result.aiFrames
            << ",\"original_frames\":" << result.originalFrames << ",\"held_frames\":" << result.heldFrames << ",\"scene_cuts\":" << result.sceneCuts
            << ",\"duplicate_pairs\":" << result.duplicatePairs << ",\"abnormal_pairs\":" << result.abnormalPairs
            << ",\"recovered_timestamps\":" << result.recoveredTimestamps
            << ",\"first_pts\":" << jsonString(result.video.firstTimestamp.toString()) << ",\"end_exclusive\":" << jsonString(result.video.endExclusive.toString())
            << ",\"source_end_evidence\":" << jsonString(result.sourceEndEvidence)
            << ",\"timing_quantized\":" << (result.validation.quantized ? "true" : "false") << ",\"validation_time_base\":" << jsonString(result.validation.timeBase.toString())
            << ",\"maximum_pts_error\":" << jsonString(result.validation.maximumTimestampError.toString()) << ",\"end_error\":" << jsonString(result.validation.durationError.toString())
            << ",\"declared_end_error\":" << (result.validation.declaredEndError ? jsonString(result.validation.declaredEndError->toString()) : "null")
            << ",\"auxiliary_maximum_end_error\":" << jsonString(result.validation.auxiliaryMaximumEndError.toString())
            << ",\"elapsed_seconds\":" << seconds << ",\"processing_fps\":" << static_cast<double>(result.video.framesSubmitted) / seconds
            << ",\"report_path\":" << (reportPathText ? jsonString(*reportPathText) : "null") << "}\n";
        return 0;
    } catch (const std::exception&) {
        if (token.stop_requested()) { std::cout << "{\"event\":\"interpolation_cancelled\"}\n"; return 130; }
        throw;
    }
}
