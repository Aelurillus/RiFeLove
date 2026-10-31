#include "application/checkpoint_artifacts.hpp"
#include "application/job_serialization.hpp"
#include "inference/model_pack.hpp"
#include "processing_pipeline_identity.hpp"
#include "restoration/stdf_tile_policy.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rifelove::application {
namespace {
using Json = nlohmann::json;

std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()),text.size()};
}

std::filesystem::path pathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(),text.end()));
}

std::wstring widenAscii(const std::string& value) {
    std::wstring result;
    result.reserve(value.size());
    for (const auto character : value) {
        if (static_cast<unsigned char>(character) > 0x7f) throw std::invalid_argument("task identifier must be ASCII");
        result.push_back(static_cast<wchar_t>(character));
    }
    return result;
}

std::string segmentPrefix(std::uint64_t index) {
    std::ostringstream prefix;
    prefix << "segment-" << std::setw(6) << std::setfill('0') << index << '-';
    return prefix.str();
}

bool isSegmentName(const std::filesystem::path& path, std::uint64_t index) {
    if (path.has_root_path() || path.has_parent_path() || path.extension() != ".mkv") return false;
    const auto name = path.filename().wstring();
    const auto prefix = widenAscii(segmentPrefix(index));
    if (name.size() != prefix.size() + 16 + 4 ||
        CompareStringOrdinal(name.data(),static_cast<int>(prefix.size()),prefix.data(),static_cast<int>(prefix.size()),TRUE) != CSTR_EQUAL ||
        CompareStringOrdinal(name.data() + name.size() - 4,4,L".mkv",4,TRUE) != CSTR_EQUAL) return false;
    return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
        name.begin() + static_cast<std::ptrdiff_t>(prefix.size() + 16),[](wchar_t value) { return std::iswxdigit(value) != 0; });
}

std::optional<std::uint64_t> segmentIndex(const std::wstring& name) {
    constexpr std::wstring_view prefix = L"segment-";
    if (name.size() < prefix.size() + 8 ||
        CompareStringOrdinal(name.data(),static_cast<int>(prefix.size()),prefix.data(),static_cast<int>(prefix.size()),TRUE) != CSTR_EQUAL)
        return {};
    std::size_t end = prefix.size();
    std::uint64_t value = 0;
    std::size_t digits = 0;
    while (end < name.size() && name[end] >= L'0' && name[end] <= L'9') {
        const auto digit = static_cast<std::uint64_t>(name[end] - L'0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return {};
        value = value * 10 + digit;
        ++end;
        ++digits;
    }
    if (digits < 6 || end >= name.size() || name[end] != L'-') return {};
    return value;
}

bool isSegmentTemporary(const std::wstring& name, std::uint64_t index) {
    constexpr std::wstring_view marker = L".rifelove-";
    constexpr std::wstring_view suffix = L".partial";
    const std::wstring_view view(name);
    if (view.size() <= suffix.size() || view.substr(view.size() - suffix.size()) != suffix) return false;
    const auto markerPosition = view.rfind(marker);
    if (markerPosition == std::wstring_view::npos) return false;
    const auto segment = std::filesystem::path(std::wstring(view.substr(0,markerPosition)));
    if (!isSegmentName(segment,index)) return false;
    const auto randoms = view.substr(markerPosition + marker.size(),view.size() - markerPosition - marker.size() - suffix.size());
    const auto separator = randoms.find(L'-');
    if (separator == std::wstring_view::npos || separator == 0 || separator + 1 >= randoms.size()) return false;
    return std::all_of(randoms.begin(),randoms.begin() + static_cast<std::ptrdiff_t>(separator),[](wchar_t value) { return value >= L'0' && value <= L'9'; }) &&
        std::all_of(randoms.begin() + static_cast<std::ptrdiff_t>(separator + 1),randoms.end(),[](wchar_t value) { return value >= L'0' && value <= L'9'; });
}

bool reparsePoint(const std::filesystem::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
        throw std::runtime_error("Could not inspect checkpoint artifact path (Windows error " + std::to_string(error) + ")");
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

void ensurePlainPath(const std::filesystem::path& path) {
    if (reparsePoint(path)) throw std::runtime_error("Checkpoint artifacts must not use symlinks or reparse points");
}

bool sameWindowsPath(const std::filesystem::path& left, const std::filesystem::path& right) {
    const auto a = std::filesystem::weakly_canonical(left).wstring();
    const auto b = std::filesystem::weakly_canonical(right).wstring();
    return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE) == CSTR_EQUAL;
}

std::optional<std::string> optionalString(const Json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || found->is_null()) return {};
    if (!found->is_string()) throw std::invalid_argument(std::string("invalid checkpoint contract field: ") + key);
    return found->get<std::string>();
}

bool sha256Text(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(),value.end(),[](const char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

bool validCompressionRuntime(const Json& manifest) {
    bool tiled = false;
    const Json* configuredJob = nullptr;
    bool compressionEnabled = false;
    if (manifest.contains("job") && manifest.at("job").is_object()) {
        const auto& job = manifest.at("job");
        const auto schema = job.value("schemaVersion",0);
        if (schema >= 10) {
            configuredJob = &job;
            if (!job.contains("compressionRestorationModelDirectory") || !job.at("compressionRestorationModelDirectory").is_string() ||
                !job.contains("expectedCompressionRestorationDescriptorSha256") || !job.at("expectedCompressionRestorationDescriptorSha256").is_string() ||
                !job.contains("compressionRestorationStrength") || !job.at("compressionRestorationStrength").is_number()) return false;
            const auto directory = job.at("compressionRestorationModelDirectory").get<std::string>();
            const auto descriptor = job.at("expectedCompressionRestorationDescriptorSha256").get<std::string>();
            const auto strength = job.at("compressionRestorationStrength").get<double>();
            compressionEnabled = !directory.empty() && sha256Text(descriptor) && std::isfinite(strength) && strength > 0.0 && strength <= 1.0;
            if (!compressionEnabled && (!directory.empty() || !descriptor.empty() || strength != 0.0)) return false;
        }
        if (schema >= 11) {
            if (schema != 11 || !job.contains("compressionRestorationTiled") || !job.at("compressionRestorationTiled").is_boolean()) return false;
            tiled = job.at("compressionRestorationTiled").get<bool>();
            if (!job.contains("compressionRestorationPolicy") ||
                job.at("compressionRestorationPolicy").dump() != serializeCompressionRestorationPolicy(tiled)) return false;
            if (tiled && !compressionEnabled) return false;
        } else if ((job.contains("compressionRestorationTiled") &&
            (!job.at("compressionRestorationTiled").is_boolean() || job.at("compressionRestorationTiled").get<bool>())) ||
            (job.contains("compressionRestorationPolicy") && job.at("compressionRestorationPolicy").dump() != serializeCompressionRestorationPolicy(false))) {
            return false;
        }
    }
    const auto found = manifest.find("compressionRestorationRuntime");
    if (found == manifest.end()) return !tiled;
    if (!configuredJob || !found->is_object()) return false;
    const auto& runtime = *found;
    if (configuredJob && (!compressionEnabled ||
        runtime.value("descriptorSha256",std::string()) != configuredJob->at("expectedCompressionRestorationDescriptorSha256").get<std::string>() ||
        runtime.value("strength",-1.0) != configuredJob->at("compressionRestorationStrength").get<double>())) return false;
    if (!runtime.contains("modelId") || !runtime.at("modelId").is_string() || runtime.at("modelId").get<std::string>().empty() ||
        !runtime.contains("adapterId") || !runtime.at("adapterId").is_string() || runtime.at("adapterId").get<std::string>().empty() ||
        !runtime.contains("adapterVersion") || !runtime.at("adapterVersion").is_number_integer() || runtime.at("adapterVersion").get<int>() <= 0 ||
        !runtime.contains("backend") || !runtime.at("backend").is_string() || runtime.at("backend").get<std::string>().empty() ||
        runtime.at("backend").get<std::string>() == "unavailable" ||
        !runtime.contains("signature") || !runtime.at("signature").is_string() || runtime.at("signature").get<std::string>().empty() ||
        !runtime.contains("descriptorSha256") || !runtime.at("descriptorSha256").is_string() || !sha256Text(runtime.at("descriptorSha256").get<std::string>()) ||
        !runtime.contains("strength") || !runtime.at("strength").is_number() || !std::isfinite(runtime.at("strength").get<double>()) ||
        runtime.at("strength").get<double>() <= 0.0 || runtime.at("strength").get<double>() > 1.0 ||
        runtime.value("inputProfile",std::string()) != "bt709-sdr8" || runtime.value("pixelFormat",std::string()) != "yuv420p" ||
        !runtime.contains("maximumWidth") || !runtime.at("maximumWidth").is_number_integer() || runtime.at("maximumWidth").get<int>() != 384 ||
        !runtime.contains("maximumHeight") || !runtime.at("maximumHeight").is_number_integer() || runtime.at("maximumHeight").get<int>() != 256 ||
        runtime.value("tilePolicy",std::string()) != (tiled ? restoration::stdf_tiles::policyId : "full-frame-bounded-384x256-v1") ||
        runtime.value("decoderReplayPolicy",std::string()) != "software-source-pts-from-start-v1" ||
        runtime.value("sceneReplayPolicy",std::string()) != "scene-bounds-serial-v1") return false;
    if (tiled) {
        if (!runtime.contains("graphSha256") || !runtime.at("graphSha256").is_string() ||
            !sha256Text(runtime.at("graphSha256").get<std::string>()) ||
            runtime.at("backend").get<std::string>() != "onnxruntime_cuda" ||
            !compressionRestorationRuntimeMatchesPolicy(true,runtime.at("signature").get<std::string>(),runtime.at("graphSha256").get<std::string>()) ||
            !runtime.contains("tiledPolicy") || runtime.at("tiledPolicy").dump() != serializeCompressionRestorationPolicy(true)) return false;
    } else if (runtime.contains("tiledPolicy") || runtime.contains("graphSha256") ||
        !compressionRestorationRuntimeMatchesPolicy(false,runtime.at("signature").get<std::string>())) {
        return false;
    }
    return true;
}

} // namespace

CheckpointArtifacts::CheckpointArtifacts(TaskStore& store, std::string taskId, std::filesystem::path outputPath)
    : store_(store), taskId_(std::move(taskId)) {
    if (taskId_.empty() || taskId_.size() > 128) throw std::invalid_argument("checkpoint task identifier is invalid");
    for (const auto character : taskId_)
        if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '-' || character == '_'))
            throw std::invalid_argument("checkpoint task identifier contains path characters");
    const auto output = std::filesystem::weakly_canonical(std::filesystem::absolute(outputPath));
    if (output.filename().empty()) throw std::invalid_argument("checkpoint output path must name a file");
    const auto baseName = output.filename().wstring() + L".rifelove-checkpoints";
    directory_ = output.parent_path() / baseName / widenAscii(taskId_);
}

std::filesystem::path CheckpointArtifacts::segmentPath(std::uint64_t index) const {
    ensureDirectory();
    std::random_device random;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto nonce = (static_cast<std::uint64_t>(random()) << 32) | random();
        std::ostringstream name;
        name << segmentPrefix(index) << std::hex << std::setw(16) << std::setfill('0') << nonce << ".mkv";
        const auto path = directory_ / name.str();
        if (!std::filesystem::exists(path)) return path;
    }
    throw std::runtime_error("could not allocate a unique checkpoint segment path");
}

void CheckpointArtifacts::ensureDirectory() const {
    const auto base = directory_.parent_path();
    ensurePlainPath(base);
    ensurePlainPath(directory_);
    std::error_code error;
    std::filesystem::create_directories(directory_,error);
    if (error) throw std::filesystem::filesystem_error("create checkpoint artifact directory",directory_,error);
    ensurePlainPath(base);
    ensurePlainPath(directory_);
    if (!std::filesystem::is_directory(base) || !std::filesystem::is_directory(directory_))
        throw std::runtime_error("checkpoint artifact root is not a local directory");
}

void CheckpointArtifacts::setManifest(const std::string& value) {
    const auto manifest = Json::parse(value);
    if (!manifest.is_object() || !manifest.contains("schemaVersion") || !manifest.at("schemaVersion").is_number_integer())
        throw std::invalid_argument("checkpoint manifest has no supported schema version");
    const auto version = manifest.at("schemaVersion").get<int>();
    if (version != 1 && version != 2) throw std::invalid_argument("unsupported checkpoint manifest schema version");
    store_.setCheckpointManifest(taskId_,value,version);
}
std::string CheckpointArtifacts::manifest() const { return store_.checkpointManifest(taskId_); }

std::filesystem::path CheckpointArtifacts::resolveRecordedPath(const TaskCheckpointSegment& segment) const {
    const auto recorded = pathFromUtf8(segment.artifactPath);
    if (!isSegmentName(recorded,segment.segmentIndex))
        throw std::runtime_error("checkpoint journal contains a path outside its task-owned segment directory");
    const auto resolved = directory_ / recorded;
    ensurePlainPath(resolved);
    if (!std::filesystem::is_regular_file(resolved)) throw std::runtime_error("recorded checkpoint segment is missing or not a regular file");
    if (!sameWindowsPath(resolved.parent_path(),directory_))
        throw std::runtime_error("checkpoint segment resolved outside its task-owned directory");
    return resolved;
}

std::vector<media::VideoSegmentInput> CheckpointArtifacts::verifyCommitted(std::stop_token stop) const {
    const auto recorded = store_.checkpointSegments(taskId_);
    if (recorded.empty()) return {};
    ensureDirectory();
    if (manifest().empty()) throw std::runtime_error("checkpoint segments exist without an immutable task manifest");
    std::vector<media::VideoSegmentInput> result;
    result.reserve(recorded.size());
    std::optional<Rational> previousEnd;
    for (const auto& item : recorded) {
        if (stop.stop_requested()) throw std::runtime_error("Checkpoint verification cancelled");
        const auto path = resolveRecordedPath(item);
        SourceLease lease(path);
        const auto identity = lease.fingerprint(stop);
        if (identity.size != item.byteCount || identity.sha256 != item.sha256)
            throw std::runtime_error("checkpoint segment bytes no longer match the SQLite journal");
        auto contract = parseContract(item.contract);
        if (item.endOutputFrame - item.firstOutputFrame != contract.frameCount)
            throw std::runtime_error("checkpoint segment frame range differs from its media contract");
        const auto verified = media::MediaEngine::verifyVideoSegment(utf8(path),contract,stop);
        // The media verifier already checks the decoded first timestamp
        // against this segment's contract within the container time base.
        // Chain continuity must therefore compare the exact checkpoint
        // contract boundaries, rather than the quantized timestamp read
        // back from the container (for example 33/1000 versus 1/30 in
        // Matroska). Otherwise a valid checkpoint can be rejected after a
        // process interruption solely because of muxer time-base rounding.
        if (previousEnd && contract.firstTimestamp != *previousEnd)
            throw std::runtime_error("checkpoint segment timestamps do not form a contiguous prefix");
        previousEnd = verified.endExclusive;
        result.push_back({utf8(path),std::move(contract)});
    }
    return result;
}

std::size_t CheckpointArtifacts::removeUncommittedSegments(std::stop_token stop) const {
    const auto committed = store_.checkpointSegments(taskId_);
    const auto firstUncommitted = static_cast<std::uint64_t>(committed.size());
    ensureDirectory();
    std::size_t removed = 0;
    std::error_code error;
    std::filesystem::directory_iterator iterator(directory_,error), end;
    if (error) throw std::filesystem::filesystem_error("open checkpoint artifact directory",directory_,error);
    for (; iterator != end; iterator.increment(error)) {
        if (error) throw std::filesystem::filesystem_error("inspect checkpoint artifacts",directory_,error);
        if (stop.stop_requested()) throw std::runtime_error("Checkpoint cleanup cancelled");
        const auto name = iterator->path().filename().wstring();
        const auto index = segmentIndex(name);
        if (!index || *index < firstUncommitted) continue;
        if (!isSegmentTemporary(name,*index) && !isSegmentName(iterator->path().filename(),*index)) continue;
        ensurePlainPath(iterator->path());
        const auto status = iterator->symlink_status(error);
        if (error) throw std::filesystem::filesystem_error("inspect checkpoint artifact",iterator->path(),error);
        if (!std::filesystem::is_regular_file(status)) continue;
        if (std::filesystem::remove(iterator->path(),error)) ++removed;
        else if (error) throw std::filesystem::filesystem_error("remove checkpoint artifact",iterator->path(),error);
    }
    if (error) throw std::filesystem::filesystem_error("finish checkpoint artifact cleanup",directory_,error);
    return removed;
}

TaskCheckpointSegment CheckpointArtifacts::commit(std::uint64_t index, std::uint64_t firstOutputFrame,
    std::uint64_t endOutputFrame, const media::VideoSegmentInput& segment, std::stop_token stop) {
    if (firstOutputFrame >= endOutputFrame || endOutputFrame - firstOutputFrame != segment.contract.frameCount)
        throw std::invalid_argument("checkpoint frame range does not match the segment contract");
    ensureDirectory();
    const auto actualPath = pathFromUtf8(segment.path);
    if (!isSegmentName(actualPath.filename(),index) || !sameWindowsPath(actualPath.parent_path(),directory_))
        throw std::invalid_argument("segment artifact path is not the task-owned path for its sequence index");
    ensurePlainPath(actualPath);
    if (!std::filesystem::is_regular_file(actualPath)) throw std::runtime_error("finalized checkpoint segment is missing");

    // Keep the file read-locked across validation, hashing, and the journal
    // transaction so another writer cannot change bytes between those steps.
    SourceLease lease(actualPath);
    (void)media::MediaEngine::verifyVideoSegment(utf8(actualPath),segment.contract,stop);
    const auto identity = lease.fingerprint(stop);
    if (identity.size == 0) throw std::runtime_error("checkpoint segment file is empty");
    TaskCheckpointSegment record{index,firstOutputFrame,endOutputFrame,utf8(actualPath.filename()),identity.sha256,identity.size,
        serializeContract(segment.contract)};
    store_.recordCheckpointSegment(taskId_,record);
    return record;
}

std::string CheckpointArtifacts::serializeContract(const media::VideoSegmentContract& contract) {
    Json value{{"schemaVersion",2},{"frameCount",contract.frameCount},{"firstTimestamp",contract.firstTimestamp.toString()},
        {"frameDuration",contract.frameDuration.toString()},{"endExclusive",contract.endExclusive ? Json(contract.endExclusive->toString()) : Json(nullptr)},
        {"width",contract.width},{"height",contract.height},{"codec",contract.codec},{"encoder",contract.encoder},{"pixelFormat",contract.pixelFormat},
        {"bitDepth",contract.bitDepth},{"primaries",contract.primaries ? Json(*contract.primaries) : Json(nullptr)},
        {"transfer",contract.transfer ? Json(*contract.transfer) : Json(nullptr)},
        {"matrix",contract.matrix ? Json(*contract.matrix) : Json(nullptr)},
        {"range",contract.range ? Json(*contract.range) : Json(nullptr)}};
    return value.dump();
}

media::VideoSegmentContract CheckpointArtifacts::parseContract(const std::string& text) {
    try {
        const auto value = Json::parse(text);
        if (!value.is_object() || (value.at("schemaVersion").get<int>() != 1 && value.at("schemaVersion").get<int>() != 2))
            throw std::invalid_argument("unsupported checkpoint segment contract version");
        const auto schemaVersion = value.at("schemaVersion").get<int>();
        media::VideoSegmentContract contract;
        const auto count = value.at("frameCount").get<std::uint64_t>();
        if (count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            throw std::invalid_argument("checkpoint contract frame count exceeds the supported range");
        contract.frameCount = static_cast<std::size_t>(count);
        contract.firstTimestamp = Rational::parse(value.at("firstTimestamp").get<std::string>());
        contract.frameDuration = Rational::parse(value.at("frameDuration").get<std::string>());
        if (!value.at("endExclusive").is_null()) contract.endExclusive = Rational::parse(value.at("endExclusive").get<std::string>());
        contract.width = value.at("width").get<int>();
        contract.height = value.at("height").get<int>();
        contract.codec = value.at("codec").get<std::string>();
        if (schemaVersion >= 2) contract.encoder = value.at("encoder").get<std::string>();
        contract.pixelFormat = value.at("pixelFormat").get<std::string>();
        contract.bitDepth = value.at("bitDepth").get<int>();
        contract.primaries = optionalString(value,"primaries");
        contract.transfer = optionalString(value,"transfer");
        contract.matrix = optionalString(value,"matrix");
        contract.range = optionalString(value,"range");
        return contract;
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string("invalid checkpoint segment contract: ") + error.what());
    }
}

bool CheckpointArtifacts::manifestMatches(const std::string& saved, const std::string& expected) {
    try {
        auto actual = Json::parse(saved);
        const auto wanted = Json::parse(expected);
        // All manifests created by the current queue require content-bound
        // media identity. Keep legacy job-schema comparisons available only
        // when neither side purports to carry the new proof.
        if (actual.contains("mediaPipelineIdentity") || wanted.contains("mediaPipelineIdentity"))
            requireMediaPipelineIdentity(saved,expected);
        if (!validCompressionRuntime(actual) || !validCompressionRuntime(wanted)) return false;
        if (actual == wanted) return true;
        // Schema 11 makes the algorithm choice explicit. A historical task can
        // migrate only to the exact legacy full-frame policy, preserving its
        // old runtime signature and all existing model/strength/replay checks.
        // Tiled work always needs a new identity and cannot adopt old segments.
        if (actual.is_object() && wanted.is_object() && actual.contains("job") && wanted.contains("job") &&
            actual.at("job").is_object() && wanted.at("job").is_object() &&
            wanted.at("job").value("schemaVersion",0) == 11 && actual.at("job").value("schemaVersion",0) < 11) {
            if (wanted.at("job").at("compressionRestorationTiled").get<bool>()) return false;
            auto previous = wanted;
            previous["job"].erase("compressionRestorationTiled");
            previous["job"].erase("compressionRestorationPolicy");
            previous["job"]["schemaVersion"] = 10;
            auto priorSaved = actual;
            priorSaved["job"].erase("compressionRestorationTiled");
            priorSaved["job"].erase("compressionRestorationPolicy");
            return manifestMatches(priorSaved.dump(),previous.dump());
        }
        // Schema 10 introduces the independent STDF compression-restoration
        // model, strength and descriptor pin. Historical checkpoints may be
        // upgraded only when the new stage is completely disabled; an old
        // segment must never be reused for a newly enabled or partially
        // configured compression path.
        if (actual.is_object() && wanted.is_object() && actual.contains("job") && wanted.contains("job") &&
            actual.at("job").is_object() && wanted.at("job").is_object() &&
            wanted.at("job").value("schemaVersion",0) == 10 && actual.at("job").value("schemaVersion",0) < 10) {
            const auto& oldJob = actual.at("job");
            const auto& newJob = wanted.at("job");
            const auto oldDirectory = oldJob.value("compressionRestorationModelDirectory",std::string());
            const auto oldHash = oldJob.value("expectedCompressionRestorationDescriptorSha256",std::string());
            const auto oldStrength = oldJob.value("compressionRestorationStrength",0.0F);
            const auto newDirectory = newJob.value("compressionRestorationModelDirectory",std::string());
            const auto newHash = newJob.value("expectedCompressionRestorationDescriptorSha256",std::string());
            const auto newStrength = newJob.value("compressionRestorationStrength",0.0F);
            if (!oldDirectory.empty() || !oldHash.empty() || oldStrength != 0.0F ||
                !newDirectory.empty() || !newHash.empty() || newStrength != 0.0F ||
                wanted.contains("compressionRestorationRuntime")) return false;
            auto previous = wanted;
            previous["job"].erase("compressionRestorationModelDirectory");
            previous["job"].erase("expectedCompressionRestorationDescriptorSha256");
            previous["job"].erase("compressionRestorationStrength");
            previous["job"]["schemaVersion"] = 9;
            auto priorSaved = actual;
            // Some fixtures and early schema-10 writers emitted the new
            // disabled fields before lowering the historical schema number.
            // Treat that exact disabled triple as absent for the migration;
            // any non-empty/positive value was rejected above.
            priorSaved["job"].erase("compressionRestorationModelDirectory");
            priorSaved["job"].erase("expectedCompressionRestorationDescriptorSha256");
            priorSaved["job"].erase("compressionRestorationStrength");
            return manifestMatches(priorSaved.dump(),previous.dump());
        }
        // Schema 9 makes automatic conditioning and its exact replay policy
        // explicit. Old fixed-sigma output is reusable only with auto absent.
        if (actual.is_object() && wanted.is_object() && actual.contains("job") && wanted.contains("job") &&
            actual.at("job").is_object() && wanted.at("job").is_object() &&
            wanted.at("job").value("schemaVersion",0) == 9 && actual.at("job").value("schemaVersion",0) < 9) {
            if (actual.at("job").contains("automaticTemporalDenoise") ||
                !wanted.at("job").contains("automaticTemporalDenoise") ||
                !wanted.at("job").at("automaticTemporalDenoise").is_null()) return false;
            auto previous = wanted;
            previous["job"].erase("automaticTemporalDenoise");
            previous["job"]["schemaVersion"] = 8;
            return manifestMatches(saved,previous.dump());
        }
        // Schema 8 introduces a second, source-rate restoration model. Older
        // checkpoints can migrate only when that additional model is absent;
        // compare the remainder through the existing schema-7 migrations.
        if (actual.is_object() && wanted.is_object() && actual.contains("job") && wanted.contains("job") &&
            actual.at("job").is_object() && wanted.at("job").is_object() &&
            wanted.at("job").value("schemaVersion",0) == 8 && actual.at("job").value("schemaVersion",0) < 8) {
            const auto& oldJob = actual.at("job");
            const auto& newJob = wanted.at("job");
            if (oldJob.contains("temporalRestorationModelDirectory") || oldJob.contains("expectedTemporalRestorationDescriptorSha256") ||
                !newJob.contains("temporalRestorationModelDirectory") || !newJob.contains("expectedTemporalRestorationDescriptorSha256") ||
                newJob.at("temporalRestorationModelDirectory") != "" || newJob.at("expectedTemporalRestorationDescriptorSha256") != "" ||
                wanted.contains("temporalRestorationRuntime")) return false;
            auto previous = wanted;
            previous["job"].erase("temporalRestorationModelDirectory");
            previous["job"].erase("expectedTemporalRestorationDescriptorSha256");
            previous["job"]["schemaVersion"] = 7;
            return manifestMatches(saved,previous.dump());
        }
        if (actual.is_object() && wanted.is_object() && actual.value("schemaVersion",0) == 2 &&
            wanted.value("schemaVersion",0) == 2 && actual.contains("job") && wanted.contains("job") &&
            actual.contains("identity") && wanted.contains("identity") && actual.at("job").is_object() && wanted.at("job").is_object()) {
            auto& oldJob = actual["job"];
            if (actual.at("identity") == wanted.at("identity") && wanted.at("job").value("schemaVersion",0) == 5 &&
                oldJob.value("schemaVersion",0) == 4 && oldJob.contains("restorationModelDirectory") &&
                oldJob.contains("expectedRestorationDescriptorSha256") && !oldJob.contains("motionBlur")) {
                oldJob["motionBlur"] = {{"enabled",false},{"shutterAngleDegrees",180},{"sampleCount",4}};
                oldJob["schemaVersion"] = 5;
                return actual == wanted;
            }
            if (actual.at("identity") == wanted.at("identity") && wanted.at("job").value("schemaVersion",0) == 6 &&
                oldJob.value("schemaVersion",0) == 4 && oldJob.contains("restorationModelDirectory") &&
                oldJob.contains("expectedRestorationDescriptorSha256") && !oldJob.contains("motionBlur") &&
                oldJob.contains("encoding") && oldJob.at("encoding").is_object() &&
                !oldJob.at("encoding").contains("outputWidth") && !oldJob.at("encoding").contains("outputHeight")) {
                oldJob["motionBlur"] = {{"enabled",false},{"shutterAngleDegrees",180},{"sampleCount",4}};
                oldJob["encoding"]["outputWidth"] = 0;
                oldJob["encoding"]["outputHeight"] = 0;
                oldJob["schemaVersion"] = 6;
                return actual == wanted;
            }
            if (actual.at("identity") == wanted.at("identity") && wanted.at("job").value("schemaVersion",0) == 6 &&
                oldJob.value("schemaVersion",0) == 5 && oldJob.contains("restorationModelDirectory") &&
                oldJob.contains("expectedRestorationDescriptorSha256") && oldJob.contains("motionBlur") &&
                oldJob.contains("encoding") && oldJob.at("encoding").is_object() &&
                !oldJob.at("encoding").contains("outputWidth") && !oldJob.at("encoding").contains("outputHeight")) {
                oldJob["encoding"]["outputWidth"] = 0;
                oldJob["encoding"]["outputHeight"] = 0;
                oldJob["schemaVersion"] = 6;
                return actual == wanted;
            }
            if (actual.at("identity") == wanted.at("identity") && wanted.at("job").value("schemaVersion",0) == 7 &&
                oldJob.value("schemaVersion",0) == 6 && oldJob.contains("restorationModelDirectory") &&
                oldJob.contains("expectedRestorationDescriptorSha256") && oldJob.contains("motionBlur") &&
                oldJob.contains("encoding") && oldJob.at("encoding").is_object() &&
                oldJob.at("encoding").contains("outputWidth") && oldJob.at("encoding").contains("outputHeight") &&
                !oldJob.contains("temporalDenoiseSigma255")) {
                // Schema 6 checkpoints used the runtime default. They may
                // resume only as the explicit schema 7 default; a changed
                // sigma must fail the identity comparison below.
                oldJob["temporalDenoiseSigma255"] = 25.0F;
                oldJob["schemaVersion"] = 7;
                return actual == wanted;
            }
            if (actual.at("identity") == wanted.at("identity") && wanted.at("job").value("schemaVersion",0) == 7 &&
                oldJob.value("schemaVersion",0) == 5 && oldJob.contains("restorationModelDirectory") &&
                oldJob.contains("expectedRestorationDescriptorSha256") && oldJob.contains("motionBlur") &&
                oldJob.contains("encoding") && oldJob.at("encoding").is_object() &&
                !oldJob.at("encoding").contains("outputWidth") && !oldJob.at("encoding").contains("outputHeight") &&
                !oldJob.contains("temporalDenoiseSigma255")) {
                oldJob["encoding"]["outputWidth"] = 0;
                oldJob["encoding"]["outputHeight"] = 0;
                oldJob["temporalDenoiseSigma255"] = 25.0F;
                oldJob["schemaVersion"] = 7;
                return actual == wanted;
            }
            if (actual.at("identity") == wanted.at("identity") && wanted.at("job").value("schemaVersion",0) == 7 &&
                oldJob.value("schemaVersion",0) == 4 && oldJob.contains("restorationModelDirectory") &&
                oldJob.contains("expectedRestorationDescriptorSha256") && !oldJob.contains("motionBlur") &&
                oldJob.contains("encoding") && oldJob.at("encoding").is_object() &&
                !oldJob.at("encoding").contains("outputWidth") && !oldJob.at("encoding").contains("outputHeight") &&
                !oldJob.contains("temporalDenoiseSigma255")) {
                oldJob["motionBlur"] = {{"enabled",false},{"shutterAngleDegrees",180},{"sampleCount",4}};
                oldJob["encoding"]["outputWidth"] = 0;
                oldJob["encoding"]["outputHeight"] = 0;
                oldJob["temporalDenoiseSigma255"] = 25.0F;
                oldJob["schemaVersion"] = 7;
                return actual == wanted;
            }
            if (actual.at("identity") == wanted.at("identity") &&
                (wanted.at("job").value("schemaVersion",0) == 4 || wanted.at("job").value("schemaVersion",0) == 5 || wanted.at("job").value("schemaVersion",0) == 6 || wanted.at("job").value("schemaVersion",0) == 7) &&
                !oldJob.contains("restorationModelDirectory") && !oldJob.contains("expectedRestorationDescriptorSha256")) {
                if (oldJob.value("schemaVersion",0) == 2 && !oldJob.contains("inferenceBackend") &&
                    wanted.at("job").value("inferenceBackend",std::string()) == "auto") {
                    oldJob["inferenceBackend"] = "auto";
                    oldJob["schemaVersion"] = 3;
                }
                if (oldJob.value("schemaVersion",0) == 3 &&
                    oldJob.value("inferenceBackend",std::string()) == wanted.at("job").value("inferenceBackend",std::string())) {
                    oldJob["restorationModelDirectory"] = "";
                    oldJob["expectedRestorationDescriptorSha256"] = "";
                    oldJob["schemaVersion"] = 4;
                }
                if (oldJob.value("schemaVersion",0) == 4 && wanted.at("job").value("schemaVersion",0) >= 5 && !oldJob.contains("motionBlur")) {
                    oldJob["motionBlur"] = {{"enabled",false},{"shutterAngleDegrees",180},{"sampleCount",4}};
                    oldJob["schemaVersion"] = 5;
                }
                if (oldJob.value("schemaVersion",0) == 5 && wanted.at("job").value("schemaVersion",0) >= 6 &&
                    oldJob.contains("encoding") && oldJob.at("encoding").is_object() &&
                    !oldJob.at("encoding").contains("outputWidth") && !oldJob.at("encoding").contains("outputHeight")) {
                    oldJob["encoding"]["outputWidth"] = 0;
                    oldJob["encoding"]["outputHeight"] = 0;
                    oldJob["schemaVersion"] = 6;
                }
                if (oldJob.value("schemaVersion",0) == 6 && wanted.at("job").value("schemaVersion",0) >= 7 &&
                    !oldJob.contains("temporalDenoiseSigma255")) {
                    oldJob["temporalDenoiseSigma255"] = 25.0F;
                    oldJob["schemaVersion"] = 7;
                }
                return actual == wanted;
            }
            return false;
        }
        if (!actual.is_object() || !wanted.is_object() || actual.value("schemaVersion",0) != 1 ||
            wanted.value("schemaVersion",0) != 2 || !actual.contains("job") || !actual.contains("identity") ||
            !actual.at("job").is_object() || !actual.at("identity").is_object()) return false;
        auto& oldJob = actual["job"];
        auto& oldIdentity = actual["identity"];
        if (oldJob.value("schemaVersion",0) != 1 || oldIdentity.value("schemaVersion",0) != 1 ||
            !oldIdentity.contains("model") || !oldIdentity.at("model").is_object()) return false;
        if (oldJob.contains("restorationModelDirectory") || oldJob.contains("expectedRestorationDescriptorSha256")) return false;
        // A v1 task could only select the built-in adapter. If it contains
        // adapter-like extension data, do not reinterpret it as trusted.
        if ((oldJob.contains("expectedModelAdapterId") && oldJob.at("expectedModelAdapterId") != "rife-sdr-v1") ||
            (oldJob.contains("expectedModelAdapterVersion") && oldJob.at("expectedModelAdapterVersion") != 1) ||
            oldIdentity.at("model").contains("adapter")) return false;
        oldJob["expectedModelAdapterId"] = "rife-sdr-v1";
        oldJob["expectedModelAdapterVersion"] = 1;
        oldJob["inferenceBackend"] = "auto";
        oldJob["restorationModelDirectory"] = "";
        oldJob["expectedRestorationDescriptorSha256"] = "";
        const auto wantedJobVersion = wanted.at("job").value("schemaVersion",0);
        oldJob["schemaVersion"] = wantedJobVersion >= 7 ? 7 : (wantedJobVersion >= 6 ? 6 : (wantedJobVersion >= 5 ? 5 : 4));
        if (wantedJobVersion >= 5)
            oldJob["motionBlur"] = {{"enabled",false},{"shutterAngleDegrees",180},{"sampleCount",4}};
        if (wantedJobVersion >= 6) {
            oldJob["encoding"]["outputWidth"] = 0;
            oldJob["encoding"]["outputHeight"] = 0;
        }
        if (wantedJobVersion >= 7) oldJob["temporalDenoiseSigma255"] = 25.0F;
        oldIdentity["model"]["adapter"] = {{"id","rife-sdr-v1"},{"version",1}};
        oldIdentity["schemaVersion"] = 2;
        actual["schemaVersion"] = 2;
        return actual == wanted;
    } catch (const std::exception&) {
        return false;
    }
}

std::string CheckpointArtifacts::mediaPipelineIdentity(std::stop_token stop) {
    // Bump this whenever writer conversion/filter/color interpretation changes.
    // v2 binds the corrected YUV layout resampling: bicubic for pixel-layout
    // conversion, Lanczos for size changes, accurate rounding and explicit
    // matrix/range preservation. DLL hashes independently bind libswscale code.
    constexpr auto writerAlgorithm = "swscale-bicubic-layout-lanczos-resize-accurate-rnd-matrix-range-v2";
    Json libraries = Json::object();
    Json dependencies = Json::object();
    std::vector<HMODULE> modules;
    const auto modulePath = [](HMODULE module) {
        std::vector<wchar_t> path(32768);
        const auto count = GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
        if (count == 0 || count >= path.size()) throw std::runtime_error("Could not resolve loaded media DLL file");
        return std::filesystem::path(std::wstring(path.data(),count));
    };
    const auto record = [&](const char* name, unsigned version, const char* configuration) {
        if (stop.stop_requested()) throw std::runtime_error("Media runtime identity verification cancelled");
        if (!configuration || !*configuration) throw std::runtime_error("FFmpeg build configuration is unavailable");
        // configuration() returns the compiled-in static string in that DLL.
        // Use its data address, avoiding MinGW import stubs in the executable
        // that can make &avcodec_version identify the EXE instead of its DLL.
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(configuration),&module))
            throw std::runtime_error(std::string("Could not locate loaded FFmpeg module: ") + name);
        if (module == GetModuleHandleW(nullptr))
            throw std::runtime_error(std::string("FFmpeg checkpoint identity requires an identifiable loaded DLL: ") + name);
        SourceLease lease(modulePath(module));
        const auto binary = lease.fingerprint(stop);
        const std::string config(configuration);
        libraries[name] = {{"version",version},{"configurationSha256",inference::sha256(std::as_bytes(std::span(config)))},
            {"binarySha256",binary.sha256},{"binarySize",binary.size}};
        modules.push_back(module);
    };
    record("avcodec",avcodec_version(),avcodec_configuration());
    record("avformat",avformat_version(),avformat_configuration());
    record("avutil",avutil_version(),avutil_configuration());
    record("swscale",swscale_version(),swscale_configuration());
    std::vector<HMODULE> visited = modules;
    std::array<wchar_t,32768> windowsPath{};
    const auto windowsLength = GetWindowsDirectoryW(windowsPath.data(),static_cast<UINT>(windowsPath.size()));
    if (!windowsLength || windowsLength >= windowsPath.size()) throw std::runtime_error("Could not identify Windows system-library directory");
    const std::wstring windowsPrefix = std::wstring(windowsPath.data(),windowsLength) + L"\\";
    // Follow resolved IAT addresses, not PATH guesses or merely import names.
    // This captures linked external encoders and their non-OS dependencies,
    // including OpenH264/Kvazaar/AOM and swresample when actually imported.
    for (std::size_t index = 0; index < modules.size(); ++index) {
        if (stop.stop_requested()) throw std::runtime_error("Media runtime identity verification cancelled");
        const auto* base = reinterpret_cast<const std::byte*>(modules[index]);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 1024 * 1024)
            throw std::runtime_error("Loaded media module has an unsupported executable header");
        const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (pe->Signature != IMAGE_NT_SIGNATURE) throw std::runtime_error("Loaded media DLL has no valid PE identity");
        const auto size = pe->OptionalHeader.SizeOfImage;
        const auto imports = pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!imports.VirtualAddress) continue;
        if (imports.VirtualAddress >= size || imports.Size > size - imports.VirtualAddress)
            throw std::runtime_error("Loaded media DLL import table exceeds its mapped image");
        const auto* entry = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
        for (std::size_t offset = 0; offset + sizeof(*entry) <= imports.Size; offset += sizeof(*entry),++entry) {
            if (!entry->FirstThunk) break;
            if (entry->FirstThunk >= size) throw std::runtime_error("Loaded media DLL has an invalid import address table");
            const auto* thunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + entry->FirstThunk);
            const auto count = (size - entry->FirstThunk) / sizeof(*thunk);
            for (std::size_t item = 0; item < count && thunk[item].u1.Function; ++item) {
                HMODULE dependency = nullptr;
                if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(thunk[item].u1.Function),&dependency))
                    throw std::runtime_error("Could not resolve actual loaded media dependency");
                if (std::find(visited.begin(),visited.end(),dependency) != visited.end()) continue;
                visited.push_back(dependency);
                const auto file = modulePath(dependency);
                const auto bytes = file.filename().u8string();
                std::string name(bytes.begin(),bytes.end());
                std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const auto& full = file.native();
                const bool system = full.size() > windowsPrefix.size() && CompareStringOrdinal(full.c_str(),static_cast<int>(windowsPrefix.size()),
                    windowsPrefix.c_str(),static_cast<int>(windowsPrefix.size()),TRUE) == CSTR_EQUAL;
                // Do not treat third-party codec DLLs as OS libraries merely
                // because someone installed them below the Windows directory.
                const bool externalCodec = name.starts_with("lib") || name.find("openh264") != std::string::npos ||
                    name.find("kvazaar") != std::string::npos || name.find("aom") != std::string::npos ||
                    name.find("dav1d") != std::string::npos || name.find("x264") != std::string::npos ||
                    name.find("x265") != std::string::npos || name.find("vpx") != std::string::npos;
                if (system && !externalCodec) continue;
                SourceLease lease(file);
                const auto binary = lease.fingerprint(stop);
                if (dependencies.contains(name)) throw std::runtime_error("Ambiguous loaded media dependency basename");
                dependencies[name] = {{"binarySha256",binary.sha256},{"binarySize",binary.size}};
                modules.push_back(dependency);
            }
        }
    }
    return Json{{"schemaVersion",2},{"writerAlgorithm",writerAlgorithm},
        {"writerSourceSha256",RIFELOVE_WRITER_SOURCE_SHA256},{"libraries",libraries},{"dependencies",dependencies},
        {"processingPipeline",Json::parse(detail::processingPipelineIdentityJson)}}.dump();
}

void CheckpointArtifacts::requireMediaPipelineIdentity(const std::string& saved, const std::string& expected) {
    const auto actual = Json::parse(saved), wanted = Json::parse(expected);
    const auto require = [](const Json& manifest, const char* kind) -> const Json& {
        if (!manifest.is_object() || !manifest.contains("mediaPipelineIdentity"))
            throw std::logic_error(std::string(kind) + " checkpoint has no content-verified FFmpeg/writer identity; preserved files cannot be safely resumed. Restart with a new output path.");
        const auto& identity = manifest.at("mediaPipelineIdentity");
        if (!identity.is_object() || identity.size() != 6 || identity.value("schemaVersion",0) != 2 ||
            !identity.contains("writerAlgorithm") || !identity.at("writerAlgorithm").is_string() || identity.at("writerAlgorithm").get<std::string>().empty() ||
            !identity.contains("libraries") || !identity.at("libraries").is_object() || identity.at("libraries").size() != 4)
            throw std::logic_error(std::string(kind) + " checkpoint media/processing identity is incomplete or unsupported; preserved files require a new output job.");
        const auto digest = [](const Json& value) {
            if (!value.is_string()) return false;
            const auto text = value.get<std::string>();
            return text.size() == 64 && std::all_of(text.begin(),text.end(),[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        };
        if (!identity.contains("writerSourceSha256") || !digest(identity.at("writerSourceSha256")))
            throw std::logic_error("Checkpoint writer source identity is missing or malformed; preserved files require a new output job.");
        if (!identity.contains("processingPipeline") || !identity.at("processingPipeline").is_object())
            throw std::logic_error("Checkpoint processing pipeline identity is missing; preserved files require a new output job.");
        const auto& pipeline = identity.at("processingPipeline");
        if (pipeline.size() != 5 || pipeline.value("schemaVersion",0) != 1 ||
            !pipeline.contains("algorithmVersion") || !pipeline.at("algorithmVersion").is_string() || pipeline.at("algorithmVersion").get<std::string>().empty() ||
            !pipeline.contains("sourceSha256") || !digest(pipeline.at("sourceSha256")) ||
            !pipeline.contains("sourceFiles") || !pipeline.at("sourceFiles").is_object() || pipeline.at("sourceFiles").empty() ||
            !pipeline.contains("build") || !pipeline.at("build").is_object())
            throw std::logic_error("Checkpoint processing source/algorithm proof is malformed; preserved files require a new output job.");
        std::string canonical = "algorithm=" + pipeline.at("algorithmVersion").get<std::string>() + "\n";
        for (const auto& [name,hash] : pipeline.at("sourceFiles").items()) {
            if (name.empty() || name.front() == '/' || name.find('\\') != std::string::npos || name.find(':') != std::string::npos ||
                name.find("..") != std::string::npos || name.find('\n') != std::string::npos || name.find('=') != std::string::npos || !digest(hash))
                throw std::logic_error("Checkpoint controlled source entry is malformed; preserved files require a new output job.");
            canonical += name + "=" + hash.get<std::string>() + "\n";
        }
        if (inference::sha256(std::as_bytes(std::span(canonical))) != pipeline.at("sourceSha256").get<std::string>())
            throw std::logic_error("Checkpoint processing source manifest digest is invalid; preserved files require a new output job.");
        const auto& build = pipeline.at("build");
        if (build.size() != 5 || !build.contains("compilerId") || !build.at("compilerId").is_string() || build.at("compilerId").get<std::string>().empty() ||
            !build.contains("compilerVersion") || !build.at("compilerVersion").is_string() || build.at("compilerVersion").get<std::string>().empty() ||
            !build.contains("buildType") || !build.at("buildType").is_string() || build.at("buildType").get<std::string>().empty() ||
            !build.contains("cxxFlagsSha256") || !digest(build.at("cxxFlagsSha256")) || build.value("cxxStandard",0) != 20)
            throw std::logic_error("Checkpoint processing build contract is malformed; preserved files require a new output job.");
        if (!identity.contains("dependencies") || !identity.at("dependencies").is_object())
            throw std::logic_error("Checkpoint linked media dependency identity is missing; preserved files require a new output job.");
        for (const auto& [name,library] : identity.at("dependencies").items()) {
            if (name.empty() || name.find_first_of("/\\:") != std::string::npos || !library.is_object() || library.size() != 2 ||
                !library.contains("binarySha256") || !digest(library.at("binarySha256")) ||
                !library.contains("binarySize") || !library.at("binarySize").is_number_unsigned() || library.at("binarySize").get<std::uint64_t>() == 0)
                throw std::logic_error("Checkpoint linked codec DLL identity is malformed; preserved files require a new output job.");
        }
        for (const auto* name : {"avcodec","avformat","avutil","swscale"}) {
            const auto& libraries = identity.at("libraries");
            if (!libraries.contains(name)) throw std::logic_error("Checkpoint media DLL identity is missing; preserved files require a new output job.");
            const auto& library = libraries.at(name);
            if (!library.is_object() || library.size() != 4 || !library.contains("binarySha256") || !digest(library.at("binarySha256")) ||
                !library.contains("configurationSha256") || !digest(library.at("configurationSha256")) ||
                !library.contains("binarySize") || !library.at("binarySize").is_number_unsigned() || library.at("binarySize").get<std::uint64_t>() == 0 ||
                !library.contains("version") || !library.at("version").is_number_unsigned() || library.at("version").get<std::uint64_t>() == 0)
                throw std::logic_error("Checkpoint media DLL identity is malformed; preserved files require a new output job.");
        }
        return identity;
    };
    const auto& a = require(actual,"Saved");
    const auto& b = require(wanted,"Current");
    if (a.at("processingPipeline") != b.at("processingPipeline"))
        throw std::logic_error("Processing pipeline source/headers, algorithm or build contract changed; checkpoint files were preserved. Restart with a new output path.");
    if (a.at("writerAlgorithm") != b.at("writerAlgorithm") || a.at("writerSourceSha256") != b.at("writerSourceSha256"))
        throw std::logic_error("Writer color/scaling algorithm changed; checkpoint files were preserved. Restart with a new output path.");
    for (const auto* name : {"avcodec","avformat","avutil","swscale"})
        if (a.at("libraries").at(name) != b.at("libraries").at(name))
            throw std::logic_error(std::string("Loaded FFmpeg ") + name + " content/build changed; checkpoint files were preserved. Restart with a new output path.");
    if (a.at("dependencies") != b.at("dependencies"))
        throw std::logic_error("Linked external codec/media DLL content changed; checkpoint files were preserved. Restart with a new output path.");
}

} // namespace rifelove::application
