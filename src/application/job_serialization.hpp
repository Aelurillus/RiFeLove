#pragma once
#include "application/processing_service.hpp"
#include <filesystem>
#include <string_view>

namespace rifelove::application {
struct ProcessingReportContext {
    std::string sourcePath;
    std::string outputPath;
    std::string targetFrameRate;
    std::string modelVersion;
    std::string modelSha256;
    double endToEndElapsedSeconds = 0;
    gpu::DeviceInfo device{0, {}, {}, 0, 0, 0, 0};
    media::VideoEncodingSettings encoding;
};

[[nodiscard]] std::string serializeJob(const ProcessingJob& job);
[[nodiscard]] ProcessingJob parseJob(const std::string& source, const std::string& output, const std::string& json);
[[nodiscard]] std::string serializeIdentity(const ProcessingIdentity& identity);
[[nodiscard]] std::string serializeProgress(const ProcessingEvent& event);
[[nodiscard]] std::string serializeResult(const PipelineResult& result);
[[nodiscard]] std::string serializeProcessingReport(const ProcessingReportContext& context, const PipelineResult& result);

// Reserves the report destination up front, then atomically publishes one JSON
// document without replacing an existing file. An unpublished partial is
// removed when the object is destroyed.
class ProcessingReportFile final {
public:
    explicit ProcessingReportFile(std::filesystem::path destination);
    ~ProcessingReportFile();
    ProcessingReportFile(const ProcessingReportFile&) = delete;
    ProcessingReportFile& operator=(const ProcessingReportFile&) = delete;
    void publish(std::string_view serializedReport);
private:
    std::filesystem::path destination_;
    std::filesystem::path temporary_;
    bool published_ = false;
};

// Pins the small model descriptor now; full graph/license/tensor verification
// still runs under the processing executor before any output is written.
void pinModelDescriptor(ProcessingJob& job);
void pinRestorationModelDescriptor(ProcessingJob& job);
void applyStoredIdentity(ProcessingJob& job, const std::string& identity);
} // namespace rifelove::application
