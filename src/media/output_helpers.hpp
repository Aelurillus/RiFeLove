#pragma once
#include "media/ffmpeg_helpers.hpp"

namespace rifelove::media::detail {
class TemporaryOutput final {
public:
    explicit TemporaryOutput(const std::filesystem::path& destination);
    ~TemporaryOutput();
    TemporaryOutput(const TemporaryOutput&) = delete;
    TemporaryOutput& operator=(const TemporaryOutput&) = delete;
    void publish();
    std::filesystem::path path;
private:
    std::filesystem::path destination_;
};
struct OutputContext {
    OutputContext() = default;
    ~OutputContext();
    OutputContext(const OutputContext&) = delete;
    OutputContext& operator=(const OutputContext&) = delete;
    AVFormatContext* value{};
};
void copyChapters(const AVFormatContext* input, AVFormatContext* output);
void copyStreamMetadata(AVDictionary** destination, const AVDictionary* source, const std::string& extension);
[[nodiscard]] std::string outputExtension(const std::filesystem::path& path);
void writeOutputHeader(AVFormatContext* output, const std::string& temporaryPath, const std::string& extension);
} // namespace rifelove::media::detail
