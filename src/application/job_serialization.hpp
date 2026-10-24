#pragma once
#include "application/processing_service.hpp"

namespace rifelove::application {
[[nodiscard]] std::string serializeJob(const ProcessingJob& job);
[[nodiscard]] ProcessingJob parseJob(const std::string& source, const std::string& output, const std::string& json);
[[nodiscard]] std::string serializeIdentity(const ProcessingIdentity& identity);
[[nodiscard]] std::string serializeProgress(const ProcessingEvent& event);
[[nodiscard]] std::string serializeResult(const PipelineResult& result);
// Pins the small model descriptor now; full graph/license/tensor verification
// still runs under the processing executor before any output is written.
void pinModelDescriptor(ProcessingJob& job);
void applyStoredIdentity(ProcessingJob& job, const std::string& identity);
} // namespace rifelove::application
