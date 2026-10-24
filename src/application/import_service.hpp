#pragma once
#include "application/task_queue.hpp"

namespace rifelove::application {
struct ImportRequest {
    std::vector<std::filesystem::path> inputs;
    std::filesystem::path outputDirectory; // Empty follows the first selection.
    ProcessingJob settings; // Source/output are assigned per imported file.
    std::string container = "mkv";
    bool recursive = false;
};
enum class ImportRejection { noVideo, unsupportedColor, unsupportedModelProfile, unreadable };
struct RejectedImport { std::filesystem::path path; ImportRejection reason; std::string detail; };
struct ImportResult {
    std::vector<std::string> taskIds;
    std::vector<RejectedImport> rejected;
    std::filesystem::path outputDirectory;
};
// Run from the queue's owner thread. Discovery/probing performs no inference;
// accepted records are committed as one cancellable transaction. Explicit file
// selections may reprocess an output, while folder discovery skips known outputs.
[[nodiscard]] ImportResult importVideos(TaskQueue& queue, const ImportRequest& request, std::stop_token stop = {},
    const std::function<void(std::size_t,std::size_t)>& progress = {});
} // namespace rifelove::application
