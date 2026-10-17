#include "media/ffmpeg_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <random>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace rifelove::media {
namespace {

class TemporaryOutput final {
public:
    explicit TemporaryOutput(const std::filesystem::path& destination) : destination_(destination) {
        if (std::filesystem::exists(destination_)) throw std::invalid_argument("output already exists; choose a new path");
        std::random_device random;
        for (int attempt = 0; attempt < 10; ++attempt) {
            path = destination_;
            path += ".rifelove-" + std::to_string(random()) + "-" + std::to_string(random()) + ".partial";
#ifdef _WIN32
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); return; }
            const auto code = GetLastError();
            if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) throw std::system_error(static_cast<int>(code), std::system_category(), "reserve output file");
#else
            int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (file >= 0) { ::close(file); return; }
            if (errno != EEXIST) throw std::system_error(errno, std::generic_category(), "reserve output file");
#endif
        }
        throw std::runtime_error("could not reserve a temporary output file");
    }
    ~TemporaryOutput() { std::error_code error; std::filesystem::remove(path, error); }
    void publish() {
#ifdef _WIN32
        if (!MoveFileExW(path.c_str(), destination_.c_str(), MOVEFILE_WRITE_THROUGH))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "publish output without replacement");
#else
        // Same-directory hard link provides no-replace publication on the POSIX developer path.
        std::filesystem::create_hard_link(path, destination_);
        std::filesystem::remove(path);
#endif
    }
    std::filesystem::path path;
private:
    std::filesystem::path destination_;
};

struct OutputContext {
    AVFormatContext* value{};
    ~OutputContext() {
        if (value) {
            if (value->pb) avio_closep(&value->pb);
            avformat_free_context(value);
        }
    }
};

void copyChapters(const AVFormatContext* input, AVFormatContext* output) {
    if (input->nb_chapters == 0) return;
    output->chapters = static_cast<AVChapter**>(av_calloc(input->nb_chapters, sizeof(AVChapter*)));
    if (!output->chapters) throw std::bad_alloc();
    for (unsigned i = 0; i < input->nb_chapters; ++i) {
        auto* chapter = static_cast<AVChapter*>(av_mallocz(sizeof(AVChapter)));
        if (!chapter) throw std::bad_alloc();
        output->chapters[output->nb_chapters++] = chapter;
        chapter->id = input->chapters[i]->id;
        chapter->start = input->chapters[i]->start;
        chapter->end = input->chapters[i]->end;
        chapter->time_base = input->chapters[i]->time_base;
        detail::check(av_dict_copy(&chapter->metadata, input->chapters[i]->metadata, 0), "copy chapter metadata");
    }
}

} // namespace

RemuxResult MediaEngine::remux(const std::string& source, const std::string& destination, std::stop_token stop) {
    detail::checkStopped(stop);
    const auto destinationPath = detail::pathFromUtf8(destination);
    auto extension = destinationPath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".mp4" && extension != ".mkv" && extension != ".mov") throw std::invalid_argument("output container must be MP4, MKV or MOV");
    detail::Input input(source, stop);
    TemporaryOutput temporary(destinationPath);
    OutputContext output;
    detail::check(avformat_alloc_output_context2(&output.value, nullptr, nullptr, destination.c_str()), "select output container");
    if (!output.value) throw std::bad_alloc();
    output.value->interrupt_callback = input.context->interrupt_callback;
    detail::check(av_dict_copy(&output.value->metadata, input.context->metadata, 0), "copy container metadata");
    copyChapters(input.context, output.value);
    for (unsigned i = 0; i < input.context->nb_streams; ++i) {
        auto* incoming = input.context->streams[i];
        if (avformat_query_codec(output.value->oformat, incoming->codecpar->codec_id, FF_COMPLIANCE_NORMAL) == 0)
            throw std::runtime_error("output container does not support stream " + std::to_string(i) + " (" + avcodec_get_name(incoming->codecpar->codec_id) + "); choose a compatible container");
        auto* outgoing = avformat_new_stream(output.value, nullptr);
        if (!outgoing) throw std::bad_alloc();
        detail::check(avcodec_parameters_copy(outgoing->codecpar, incoming->codecpar), "copy stream parameters");
        outgoing->codecpar->codec_tag = 0;
        outgoing->time_base = incoming->time_base;
        outgoing->avg_frame_rate = incoming->avg_frame_rate;
        outgoing->sample_aspect_ratio = incoming->sample_aspect_ratio;
        outgoing->disposition = incoming->disposition;
        detail::check(av_dict_copy(&outgoing->metadata, incoming->metadata, 0), "copy stream metadata");
    }
    const auto temporaryPath = detail::pathToUtf8(temporary.path);
    detail::check(avio_open2(&output.value->pb, temporaryPath.c_str(), AVIO_FLAG_WRITE, &output.value->interrupt_callback, nullptr), "open temporary output");
    AVDictionary* options = nullptr;
    if (extension == ".mp4" || extension == ".mov") {
        const int status = av_dict_set(&options, "movflags", "use_metadata_tags", 0);
        if (status < 0) { av_dict_free(&options); detail::check(status, "configure metadata preservation"); }
    }
    const int header = avformat_write_header(output.value, &options);
    av_dict_free(&options);
    detail::check(header, "write container header");
    auto freePacket = [](AVPacket* p) { av_packet_free(&p); };
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
    if (!packet) throw std::bad_alloc();
    std::size_t written = 0;
    for (;;) {
        detail::checkStopped(stop);
        const int read = av_read_frame(input.context, packet.get());
        if (read == AVERROR_EOF) break;
        detail::check(read, "read packet for stream copy");
        const auto* incoming = input.context->streams[packet->stream_index];
        const auto* outgoing = output.value->streams[packet->stream_index];
        av_packet_rescale_ts(packet.get(), incoming->time_base, outgoing->time_base);
        packet->pos = -1;
        detail::check(av_interleaved_write_frame(output.value, packet.get()), "write compressed packet");
        ++written;
        av_packet_unref(packet.get());
    }
    detail::check(av_write_trailer(output.value), "finalize output container");
    detail::check(avio_closep(&output.value->pb), "close output file");
    detail::checkStopped(stop);

    const auto verified = probe(temporaryPath, stop);
    std::map<std::pair<std::string, std::string>, unsigned> streamCounts;
    for (const auto& stream : verified.streams) ++streamCounts[{stream.type, stream.codec}];
    for (unsigned i = 0; i < input.context->nb_streams; ++i) {
        const auto* parameters = input.context->streams[i]->codecpar;
        const auto* kind = av_get_media_type_string(parameters->codec_type);
        auto& count = streamCounts[{kind ? kind : "unknown", avcodec_get_name(parameters->codec_id)}];
        if (count == 0) throw std::runtime_error("output validation found a missing stream");
        --count;
    }
    if (verified.chapters.size() < input.context->nb_chapters) throw std::runtime_error("output container did not preserve all chapters");
    temporary.publish();
    return {input.context->nb_streams, written};
}

} // namespace rifelove::media
