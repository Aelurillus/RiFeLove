#include "application/checkpoint_artifacts.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
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
    return name.size() > prefix.size() + 4 &&
        CompareStringOrdinal(name.data(),static_cast<int>(prefix.size()),prefix.data(),static_cast<int>(prefix.size()),TRUE) == CSTR_EQUAL;
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

void CheckpointArtifacts::setManifest(const std::string& value) { store_.setCheckpointManifest(taskId_,value); }
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
        if (previousEnd && verified.firstTimestamp != *previousEnd)
            throw std::runtime_error("checkpoint segment timestamps do not form a contiguous prefix");
        previousEnd = verified.endExclusive;
        result.push_back({utf8(path),std::move(contract)});
    }
    return result;
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
    Json value{{"schemaVersion",1},{"frameCount",contract.frameCount},{"firstTimestamp",contract.firstTimestamp.toString()},
        {"frameDuration",contract.frameDuration.toString()},{"endExclusive",contract.endExclusive ? Json(contract.endExclusive->toString()) : Json(nullptr)},
        {"width",contract.width},{"height",contract.height},{"codec",contract.codec},{"pixelFormat",contract.pixelFormat},
        {"bitDepth",contract.bitDepth},{"primaries",contract.primaries ? Json(*contract.primaries) : Json(nullptr)},
        {"transfer",contract.transfer ? Json(*contract.transfer) : Json(nullptr)},
        {"matrix",contract.matrix ? Json(*contract.matrix) : Json(nullptr)},
        {"range",contract.range ? Json(*contract.range) : Json(nullptr)}};
    return value.dump();
}

media::VideoSegmentContract CheckpointArtifacts::parseContract(const std::string& text) {
    try {
        const auto value = Json::parse(text);
        if (!value.is_object() || value.at("schemaVersion").get<int>() != 1)
            throw std::invalid_argument("unsupported checkpoint segment contract version");
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

} // namespace rifelove::application
