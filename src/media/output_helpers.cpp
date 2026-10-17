#include "media/output_helpers.hpp"
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

namespace rifelove::media::detail {
TemporaryOutput::TemporaryOutput(const std::filesystem::path& destination) : destination_(destination) {
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
TemporaryOutput::~TemporaryOutput() { std::error_code error; std::filesystem::remove(path, error); }
void TemporaryOutput::publish() {
#ifdef _WIN32
    if (!MoveFileExW(path.c_str(), destination_.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "publish output without replacement");
#else
    std::filesystem::create_hard_link(path, destination_);
    std::filesystem::remove(path);
#endif
}
OutputContext::~OutputContext() {
    if (value) {
        if (value->pb) avio_closep(&value->pb);
        avformat_free_context(value);
    }
}
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
        check(av_dict_copy(&chapter->metadata, input->chapters[i]->metadata, 0), "copy chapter metadata");
    }
}
std::string outputExtension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".mp4" && extension != ".mkv" && extension != ".mov") throw std::invalid_argument("output container must be MP4, MKV or MOV");
    return extension;
}
void writeOutputHeader(AVFormatContext* output, const std::string& path, const std::string& extension) {
    check(avio_open2(&output->pb, path.c_str(), AVIO_FLAG_WRITE, &output->interrupt_callback, nullptr), "open temporary output");
    AVDictionary* options = nullptr;
    if (extension == ".mp4" || extension == ".mov") {
        const int status = av_dict_set(&options, "movflags", "use_metadata_tags", 0);
        if (status < 0) { av_dict_free(&options); check(status, "configure metadata preservation"); }
    }
    const int header = avformat_write_header(output, &options);
    av_dict_free(&options);
    check(header, "write container header");
}
} // namespace rifelove::media::detail
