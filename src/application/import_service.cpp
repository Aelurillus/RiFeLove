#include "application/import_service.hpp"
#include "application/job_serialization.hpp"
#include "media/rife_color.hpp"
#include "restoration/model_pack.hpp"
#include <algorithm>
#include <set>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rifelove::application {
namespace {
namespace fs = std::filesystem;
struct PathLess {
    bool operator()(const fs::path& a, const fs::path& b) const { return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE) == CSTR_LESS_THAN; }
};
fs::path normalized(const fs::path& value) { return fs::weakly_canonical(fs::absolute(value)); }
bool same(const fs::path& a, const fs::path& b) { return !PathLess{}(a,b) && !PathLess{}(b,a); }
bool child(const fs::path& candidate, const fs::path& parent) {
    auto prefix = parent.native(); if (!prefix.ends_with(L"\\")) prefix += L"\\";
    const auto& value = candidate.native();
    return value.size() > prefix.size() && CompareStringOrdinal(value.c_str(),static_cast<int>(prefix.size()),prefix.c_str(),static_cast<int>(prefix.size()),TRUE) == CSTR_EQUAL;
}
void checkStop(std::stop_token stop) { if (stop.stop_requested()) throw std::runtime_error("Import cancelled"); }
std::string utf8(const fs::path& value) { const auto bytes = value.u8string(); return {bytes.begin(),bytes.end()}; }
bool videoName(const fs::path& value) {
    auto extension = value.extension().native();
    for (auto& c : extension) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    static const std::set<std::wstring> names{L".mp4",L".mkv",L".mov",L".avi",L".webm",L".m4v",L".ts",L".m2ts",L".mts",L".mpeg",L".mpg",L".wmv",L".flv",L".mxf",L".ogv",L".vob",L".3gp",L".asf",L".gif"};
    return names.contains(extension);
}
fs::path outputPath(const fs::path& directory, const fs::path& source, const std::string& container, std::set<fs::path,PathLess>& reserved) {
    const auto stem = source.stem().native() + L"_RifeLove";
    for (int i = 1; i <= 100000; ++i) {
        const auto suffix = i == 1 ? std::wstring() : L"_" + std::to_wstring(i);
        const auto output = directory / (stem + suffix + L"." + fs::path(container).native());
        if (!fs::exists(output) && reserved.insert(output).second) return output;
    }
    throw std::runtime_error("Could not select a new output filename");
}
}
ImportResult importVideos(TaskQueue& queue, const ImportRequest& request, std::stop_token stop,
    const std::function<void(std::size_t,std::size_t)>& progress) {
    checkStop(stop);
    if (queue.busy()) throw std::logic_error("Stop the running queue before importing videos");
    if (request.inputs.empty() || request.settings.settings.targetRate <= Rational(0) ||
        (request.container != "mkv" && request.container != "mp4" && request.container != "mov")) throw std::invalid_argument("Invalid import selection, FPS or container");
    auto settings = request.settings; pinModelDescriptor(settings); pinRestorationModelDescriptor(settings);
    const auto modelPack = inference::loadModelPack(settings.modelDirectory);
    if (modelPack.sha256 != settings.expectedModelSha256 || modelPack.id != settings.expectedModelId || modelPack.version != settings.expectedModelVersion)
        throw std::invalid_argument("Selected model differs from the verified local model pack");
    std::optional<restoration::ModelPack> restorationPack;
    if (!settings.restorationModelDirectory.empty()) restorationPack = restoration::loadModelPack(settings.restorationModelDirectory);
    const auto first = normalized(request.inputs.front());
    ImportResult result; result.outputDirectory = request.outputDirectory.empty() ? (fs::is_directory(first) ? first : first.parent_path()) : normalized(request.outputDirectory);
    if (!fs::is_directory(result.outputDirectory)) throw std::invalid_argument("Output directory does not exist");
    std::set<fs::path,PathLess> seen, knownOutputs, reserved;
    for (const auto& row : queue.list()) { const auto value = normalized(fs::path(std::u8string(row.task.outputPath.begin(),row.task.outputPath.end()))); knownOutputs.insert(value); reserved.insert(value); }
    std::vector<fs::path> sources;
    for (const auto& selected : request.inputs) {
        checkStop(stop); if (selected.empty()) throw std::invalid_argument("Import path is empty"); const auto input = normalized(selected);
        if (fs::is_regular_file(input)) { if (seen.insert(input).second) sources.push_back(input); }
        else if (fs::is_directory(input)) {
            std::set<fs::path,PathLess> discovered;
            auto add = [&](const fs::directory_entry& entry) {
                checkStop(stop);
                if (!entry.is_regular_file() || entry.is_symlink() || !videoName(entry.path())) return;
                const auto value = normalized(entry.path()); if (!knownOutputs.contains(value)) discovered.insert(value);
                if (discovered.size() + sources.size() > 10000) throw std::invalid_argument("Import exceeds 10000 files; use smaller batches");
            };
            if (request.recursive) {
                const bool excludeOutput = child(result.outputDirectory,input);
                for (auto it = fs::recursive_directory_iterator(input,fs::directory_options::skip_permission_denied); it != fs::recursive_directory_iterator(); ++it) {
                    if (excludeOutput && it->is_directory() && same(normalized(it->path()),result.outputDirectory)) { it.disable_recursion_pending(); continue; }
                    add(*it);
                }
            } else for (const auto& entry : fs::directory_iterator(input)) add(entry);
            for (const auto& value : discovered) if (seen.insert(value).second) sources.push_back(value);
        } else throw std::invalid_argument("An import path does not exist");
        if (sources.size() > 10000) throw std::invalid_argument("Import exceeds 10000 files; use smaller batches");
    }
    std::vector<ProcessingJob> jobs;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        checkStop(stop); const auto& source = sources[index];
        try {
            const auto info = media::MediaEngine::probe(utf8(source),stop);
            const auto video = std::find_if(info.streams.begin(),info.streams.end(),[&](const auto& stream) { return stream.index == info.preferredVideoStream; });
            if (video == info.streams.end()) result.rejected.push_back({source,ImportRejection::noVideo,"No video stream"});
            else {
                bool valid = true;
                std::string profile;
                try { profile = media::rifeSdrInputProfile(video->color); }
                catch (const std::exception& error) { result.rejected.push_back({source,ImportRejection::unsupportedColor,error.what()}); valid = false; }
                if (valid && !inference::supportsInputProfile(modelPack,profile)) {
                    result.rejected.push_back({source,ImportRejection::unsupportedModelProfile,
                        "Selected model does not declare support for input profile " + profile});
                    valid = false;
                }
                if (valid && restorationPack && std::find(restorationPack->capabilities.supportedInputProfiles.begin(),
                    restorationPack->capabilities.supportedInputProfiles.end(),profile) == restorationPack->capabilities.supportedInputProfiles.end()) {
                    result.rejected.push_back({source,ImportRejection::unsupportedRestorationModelProfile,
                        "Selected super-resolution model does not declare support for input profile " + profile});
                    valid = false;
                }
                if (valid && restorationPack) {
                    const auto scale = restorationPack->capabilities.scaleNumerator;
                    if (video->width > 8192 / scale || video->height > 8192 / scale) {
                        result.rejected.push_back({source,ImportRejection::unsupportedRestorationModelProfile,
                            "Selected super-resolution model would exceed the 8192-pixel output limit"});
                        valid = false;
                    }
                }
                if (valid && settings.settings.encoding.encoder != "auto") {
                    const auto capability = media::MediaEngine::probeEncoder(settings.settings.encoding.encoder,*video,
                        settings.settings.targetRate,stop);
                    if (!capability.contextOpened) {
                        result.rejected.push_back({source,ImportRejection::unsupportedEncoder,capability.reason});
                        valid = false;
                    }
                }
                if (valid) {
                    auto job = settings; job.source = utf8(source); job.output = utf8(outputPath(result.outputDirectory,source,request.container,reserved));
                    if (video->duration && *video->duration > Rational(0)) job.sourceDurationEstimate = video->duration;
                    else if (video->declaredEnd && video->start && *video->declaredEnd > *video->start) job.sourceDurationEstimate = *video->declaredEnd - *video->start;
                    else if (info.duration && *info.duration > Rational(0)) job.sourceDurationEstimate = info.duration;
                    jobs.push_back(std::move(job));
                }
            }
        } catch (const std::exception& error) { checkStop(stop); result.rejected.push_back({source,ImportRejection::unreadable,error.what()}); }
        if (progress) progress(index + 1,sources.size());
    }
    checkStop(stop);
    if (!jobs.empty()) result.taskIds = queue.enqueue(std::move(jobs),stop);
    return result;
}
} // namespace rifelove::application
