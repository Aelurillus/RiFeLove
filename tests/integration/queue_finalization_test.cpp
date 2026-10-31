#include "application/job_serialization.hpp"
#include "application/task_queue.hpp"
#include "core/pipeline/output_validation.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <system_error>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
std::atomic<unsigned> engineInvocations{0};
std::atomic<bool> emitFinalProgress{false};
std::atomic<bool> failAfterPublication{false};
std::atomic<bool> createPublicationConflict{false};
std::filesystem::path localPath(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(),value.end()));
}
struct DirectoryIdentity {
    DWORD volume = 0;
    ULONGLONG index = 0;
};
bool sameWindowsPath(const std::filesystem::path& left, const std::filesystem::path& right) {
    const auto leftText = left.wstring();
    const auto rightText = right.wstring();
    return CompareStringOrdinal(leftText.c_str(),-1,rightText.c_str(),-1,TRUE) == CSTR_EQUAL;
}
std::optional<DirectoryIdentity> readDirectoryIdentity(const std::filesystem::path& path, std::string& reason) {
    const auto handle = CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        reason = "could not open fixture directory (Windows error " + std::to_string(GetLastError()) + ")";
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool read = GetFileInformationByHandle(handle,&info) != FALSE;
    const auto error = read ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!read) {
        reason = "could not read fixture directory identity (Windows error " + std::to_string(error) + ")";
        return std::nullopt;
    }
    return DirectoryIdentity{info.dwVolumeSerialNumber,
        (static_cast<ULONGLONG>(info.nFileIndexHigh) << 32U) | info.nFileIndexLow};
}
bool hasReparsePoint(const std::filesystem::path& path, std::string& reason) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        reason = "could not inspect fixture path (Windows error " + std::to_string(GetLastError()) + ")";
        return true;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        reason = "fixture path contains a reparse point: " + path.string();
        return true;
    }
    return false;
}
}

namespace rifelove::application {
// Test-only engine substitution in this separate executable. The model/GPU
// computation is replaced, but checkpoint commits, media decode validation,
// pre-publication hashing, SQLite proof storage and no-replace publication all
// use the real application services. Production executables link the ordinary
// processing_service.cpp implementation instead.
PipelineResult executeProcessingJob(const ProcessingJob& job, const std::function<void(ProcessingEvent)>& progress,
    std::stop_token stop, PauseGate&,
    const std::function<void(const ProcessingIdentity&,PipelineSettings&)>& configurePipeline) {
    ++engineInvocations;
    SourceLease source(localPath(job.source));
    ProcessingIdentity identity{};
    identity.source = source.fingerprint(stop);
    identity.modelId = "cpu-publication-fixture";
    identity.modelVersion = "1";
    identity.modelSha256 = std::string(64,'a');
    identity.device.name = "CPU publication fixture (no CUDA context)";
    identity.backend = "test-fixture";
    auto settings = job.settings;
    configurePipeline(identity,settings);
    if (!settings.checkpoint) throw std::logic_error("Publication fixture needs the real queue checkpoint callbacks");

    const auto sourceInfo = media::MediaEngine::probe(job.source,stop);
    const auto& video = sourceInfo.streams.at(0);
    media::VideoSegmentContract contract;
    contract.frameCount = 2;
    contract.firstTimestamp = Rational(0);
    contract.frameDuration = Rational(1,25);
    contract.endExclusive = Rational(2,25);
    contract.width = video.width; contract.height = video.height;
    contract.codec = video.codec; contract.encoder = "ffv1";
    contract.pixelFormat = video.color.pixelFormat; contract.bitDepth = video.color.bitDepth;
    contract.primaries = video.color.primaries; contract.transfer = video.color.transfer;
    contract.matrix = video.color.matrix; contract.range = video.color.range;
    const auto segmentPath = settings.checkpoint->segmentPath(0);
    if (!std::filesystem::copy_file(localPath(job.source),localPath(segmentPath)))
        throw std::runtime_error("Could not create publication fixture checkpoint");
    settings.checkpoint->commitSegment(0,0,2,{segmentPath,contract});

    auto temporary = localPath(job.output);
    temporary += ".publication-fixture.partial.mkv";
    if (!std::filesystem::copy_file(localPath(segmentPath),temporary))
        throw std::runtime_error("Could not create publication fixture temporary output");
    PipelineResult result{};
    result.video.encoder = "ffv1"; result.video.framesSubmitted = 2;
    result.video.videoPackets = 2; result.video.firstTimestamp = Rational(0);
    result.video.endExclusive = Rational(2,25);
    result.sourceFrames = 2; result.originalFrames = 2; result.processedFrames = 2;
    result.modelId = identity.modelId; result.backend = identity.backend;
    result.validation = detail::validatePipelineOutput(temporary.string(),0,video.width,video.height,
        Rational(0),Rational(2,25),Rational(25),2,sourceInfo,{},stop);
    settings.checkpoint->prepareOutputPublication(temporary.string(),result);
    if (createPublicationConflict.load()) {
        const auto conflicting = CreateFileW(localPath(job.output).c_str(),GENERIC_WRITE,0,nullptr,
            CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (conflicting == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not create fixture publication conflict");
        constexpr char content[] = "Another publisher owns this file";
        DWORD written = 0;
        const auto success = WriteFile(conflicting,content,sizeof(content) - 1,&written,nullptr);
        CloseHandle(conflicting);
        if (!success || written != sizeof(content) - 1) throw std::runtime_error("Could not write fixture publication conflict");
    }
    if (!MoveFileExW(temporary.c_str(),localPath(job.output).c_str(),MOVEFILE_WRITE_THROUGH))
        throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"publish fixture without replacement");
    if (failAfterPublication.load()) throw std::runtime_error("injected failure after publication before completed event");
    if (emitFinalProgress.load()) {
        // Cross the queue's actual one-second progress persistence boundary,
        // which was just reset by pre-publication fingerprinting above.
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        ProcessingEvent event; event.stage = "completed";
        event.outputFrames = 2; event.processedFrames = 2;
        progress(std::move(event));
    }
    return result;
}
} // namespace rifelove::application

namespace {
using namespace rifelove;
using namespace rifelove::application;

class QueueFinalization : public ::testing::Test {
protected:
    struct Events {
        bool drained = false;
        unsigned completed = 0, failed = 0;
        std::string error;
        void observe(QueueEvent event) {
            if (event.drained) { drained = true; error = std::move(event.queueError); }
            else if (event.processing.kind == ProcessingEvent::Kind::completed) ++completed;
            else if (event.processing.kind == ProcessingEvent::Kind::failed) ++failed;
        }
    };
    void SetUp() override {
        cleanupParent = std::filesystem::canonical(std::filesystem::temp_directory_path());
        directory = cleanupParent / ("rifelove-finalization-" +
            std::to_string(std::random_device{}()) + "-" + std::to_string(std::random_device{}()));
        expectedDirectory = std::filesystem::absolute(directory).lexically_normal();
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        expectedCanonicalDirectory = std::filesystem::canonical(directory);
        std::string identityError;
        const auto identity = readDirectoryIdentity(directory,identityError);
        ASSERT_TRUE(identity.has_value()) << identityError;
        cleanupIdentity = *identity;
        database = directory / "queue.sqlite";
        source = directory / "source.mkv";
        output = directory / "published.mkv";
        const auto command = "ffmpeg.exe -v error -nostdin -f lavfi -i \"color=black:size=64x64:rate=25:duration=0.08\" "
            "-vf \"setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=limited\" "
            "-c:v ffv1 -pix_fmt yuv420p \"" + source.string() + "\"";
        ASSERT_EQ(std::system(command.c_str()),0);
        ProcessingJob job;
        job.source = source.string(); job.output = output.string();
        job.modelDirectory = directory / "unused-model";
        job.runtimeDirectory = directory / "unused-gpu-runtime";
        job.settings.targetRate = Rational(25); job.settings.encoding.encoder = "ffv1";
        TaskStore seed(database.string());
        id = seed.createConfigured({{job.source,job.output,serializeJob(job)}}).front();
        initialInvocations = engineInvocations.load();
        emitFinalProgress.store(false);
        failAfterPublication.store(false);
        createPublicationConflict.store(false);
    }
    void TearDown() override {
        emitFinalProgress.store(false);
        failAfterPublication.store(false);
        createPublicationConflict.store(false);
        std::string reason;
        if (!removeFixtureSafely(reason)) {
            // The fixture is retained whenever any identity, containment or
            // reparse-point check is inconclusive. Never broaden cleanup to
            // recover from a failed safety check.
            RecordProperty("cleanup_status",("preserved: " + reason).c_str());
            std::cerr << "queue finalization fixture retained at " << directory.string()
                      << ": " << reason << '\n';
            return;
        }
        RecordProperty("cleanup_status","removed");
    }
    void sql(const char* statement) {
        sqlite3* connection = nullptr;
        if (sqlite3_open(database.string().c_str(),&connection) != SQLITE_OK) {
            if (connection) sqlite3_close(connection);
            throw std::runtime_error("Could not open failure-injection connection");
        }
        char* message = nullptr;
        const auto status = sqlite3_exec(connection,statement,nullptr,nullptr,&message);
        const std::string detail = message ? message : "";
        sqlite3_free(message); sqlite3_close(connection);
        if (status != SQLITE_OK) throw std::runtime_error(detail);
    }
    void rejectCompletion() {
        sql("CREATE TRIGGER reject_completion BEFORE UPDATE ON tasks WHEN NEW.state='completed' "
            "BEGIN SELECT RAISE(ABORT,'injected completed state write failure'); END");
    }
    FileIdentity fingerprint() { SourceLease lease(output); return lease.fingerprint({}); }
    void expectUnchanged(const FileIdentity& before) {
        const auto after = fingerprint();
        EXPECT_EQ(after.sha256,before.sha256); EXPECT_EQ(after.size,before.size);
        EXPECT_EQ(after.fileId,before.fileId); EXPECT_EQ(after.modified,before.modified);
    }
    TaskOutputPublication proof() {
        const auto saved = TaskStore(database.string()).outputPublicationIntent(id);
        if (!saved) throw std::runtime_error("Validated output lost its saved publication proof");
        return *saved;
    }
    bool removeFixtureSafely(std::string& reason) const {
        std::error_code error;
        if (!std::filesystem::exists(directory,error)) {
            if (error) { reason = "could not check fixture existence: " + error.message(); return false; }
            return true;
        }
        if (error || !std::filesystem::is_directory(directory,error) || error) {
            reason = error ? "could not inspect fixture directory: " + error.message() : "fixture path is not a directory";
            return false;
        }
        if (std::filesystem::absolute(directory).lexically_normal() != expectedDirectory) {
            reason = "fixture absolute path changed"; return false;
        }
        if (directory.filename() != expectedDirectory.filename() || directory.parent_path() != cleanupParent) {
            reason = "fixture name or parent directory changed"; return false;
        }
        const auto canonicalParent = std::filesystem::canonical(directory.parent_path(),error);
        if (error || !sameWindowsPath(canonicalParent,cleanupParent)) {
            reason = error ? "could not verify fixture parent: " + error.message() : "fixture escaped its temporary root";
            return false;
        }
        const auto canonicalDirectory = std::filesystem::canonical(directory,error);
        if (error || !sameWindowsPath(canonicalDirectory,expectedCanonicalDirectory)) {
            reason = error ? "could not verify fixture canonical path: " + error.message() : "fixture canonical path changed";
            return false;
        }
        if (hasReparsePoint(directory,reason)) return false;
        const auto currentIdentity = readDirectoryIdentity(directory,reason);
        if (!currentIdentity || currentIdentity->volume != cleanupIdentity.volume || currentIdentity->index != cleanupIdentity.index) {
            if (reason.empty()) reason = "fixture directory identity changed";
            return false;
        }
        std::filesystem::recursive_directory_iterator iterator(directory,std::filesystem::directory_options::none,error);
        std::filesystem::recursive_directory_iterator end;
        while (!error && iterator != end) {
            if (hasReparsePoint(iterator->path(),reason)) return false;
            iterator.increment(error);
        }
        if (error) { reason = "could not inspect fixture descendants: " + error.message(); return false; }
        std::filesystem::remove_all(directory,error);
        if (error) { reason = "could not remove validated fixture: " + error.message(); return false; }
        return true;
    }
    std::filesystem::path cleanupParent, directory, expectedDirectory, expectedCanonicalDirectory, database, source, output;
    DirectoryIdentity cleanupIdentity{};
    std::string id;
    unsigned initialInvocations = 0;
};
}

TEST_F(QueueFinalization, CompletedWriteFailurePreservesPublishedOutputAndAdoptsItOnReopen) {
    FileIdentity published;
    std::string savedResult;
    {
        TaskQueue queue(database);
        rejectCompletion();
        Events events;
        ASSERT_TRUE(queue.start([&](QueueEvent event) { events.observe(std::move(event)); }));
        queue.wait();
        EXPECT_FALSE(queue.busy()); EXPECT_TRUE(events.drained);
        EXPECT_EQ(events.failed,0U); EXPECT_EQ(events.completed,0U);
        EXPECT_NE(events.error.find("validated and published"),std::string::npos);
        EXPECT_NE(events.error.find("injected completed state write failure"),std::string::npos);
        ASSERT_EQ(queue.list().front().task.state,TaskState::interrupted);
        EXPECT_TRUE(queue.list().front().result.empty());
        published = fingerprint();
        const auto saved = proof(); savedResult = saved.result;
        EXPECT_EQ(saved.sha256,published.sha256); EXPECT_EQ(saved.byteCount,published.size);
        EXPECT_THROW(queue.retry(id),std::logic_error);
        Events next;
        ASSERT_TRUE(queue.start([&](QueueEvent event) { next.observe(std::move(event)); }));
        queue.wait();
        EXPECT_TRUE(next.drained); EXPECT_TRUE(next.error.empty());
        EXPECT_EQ(engineInvocations.load(),initialInvocations + 1);
        expectUnchanged(published);
    }
    sql("DROP TRIGGER reject_completion");
    {
        TaskQueue recovered(database);
        EXPECT_EQ(recovered.recoveredPublishedOutputCount(),1U);
        ASSERT_EQ(recovered.list().front().task.state,TaskState::completed);
        EXPECT_EQ(recovered.list().front().result,savedResult);
        EXPECT_TRUE(recovered.list().front().task.error.empty());
        expectUnchanged(published);
    }
    TaskQueue reopened(database);
    EXPECT_EQ(reopened.recoveredPublishedOutputCount(),0U);
    EXPECT_EQ(engineInvocations.load(),initialInvocations + 1);
    EXPECT_EQ(reopened.list().front().task.state,TaskState::completed);
    expectUnchanged(published);
}

TEST_F(QueueFinalization, ResultWriteRollbackRemainsInterruptedAndExplicitlyRecoversWithoutReencoding) {
    TaskQueue queue(database);
    sql("CREATE TRIGGER reject_result BEFORE UPDATE OF result ON task_details WHEN NEW.result<>'' "
        "BEGIN SELECT RAISE(ABORT,'injected completed result write failure'); END");
    Events events;
    ASSERT_TRUE(queue.start([&](QueueEvent event) { events.observe(std::move(event)); }));
    queue.wait();
    EXPECT_TRUE(events.drained); EXPECT_EQ(events.failed,0U);
    EXPECT_NE(events.error.find("injected completed result write failure"),std::string::npos);
    ASSERT_EQ(queue.list().front().task.state,TaskState::interrupted);
    EXPECT_TRUE(queue.list().front().result.empty());
    const auto published = fingerprint(); const auto saved = proof();
    sql("DROP TRIGGER reject_result");
    EXPECT_NO_THROW(queue.resumeCheckpoint(id));
    ASSERT_EQ(queue.list().front().task.state,TaskState::completed);
    EXPECT_EQ(queue.list().front().result,saved.result);
    EXPECT_EQ(engineInvocations.load(),initialInvocations + 1);
    expectUnchanged(published);
}

TEST_F(QueueFinalization, CompletionAndRecoveryWriteFailuresReportBothCausesAndPreserveProof) {
    FileIdentity published;
    {
        TaskQueue queue(database);
        rejectCompletion();
        sql("CREATE TRIGGER reject_interruption BEFORE UPDATE ON tasks "
            "WHEN NEW.state='interrupted' AND OLD.state IN ('running','paused') "
            "BEGIN SELECT RAISE(ABORT,'injected interruption write failure'); END");
        Events events;
        ASSERT_TRUE(queue.start([&](QueueEvent event) { events.observe(std::move(event)); }));
        queue.wait();
        EXPECT_FALSE(queue.busy()); EXPECT_TRUE(events.drained); EXPECT_EQ(events.failed,0U);
        EXPECT_NE(events.error.find("injected completed state write failure"),std::string::npos);
        EXPECT_NE(events.error.find("could not persist interrupted task state"),std::string::npos);
        EXPECT_NE(events.error.find("injected interruption write failure"),std::string::npos);
        EXPECT_EQ(queue.list().front().task.state,TaskState::running);
        published = fingerprint();
        EXPECT_EQ(proof().sha256,published.sha256);
    }
    sql("DROP TRIGGER reject_interruption");
    TaskQueue recovered(database);
    ASSERT_EQ(recovered.list().front().task.state,TaskState::interrupted);
    EXPECT_NE(recovered.list().front().task.error.find("injected completed state write failure"),std::string::npos);
    EXPECT_NE(recovered.list().front().task.error.find("left untouched"),std::string::npos);
    EXPECT_EQ(recovered.recoveredPublishedOutputCount(),0U);
    expectUnchanged(published);
    sql("DROP TRIGGER reject_completion");
    EXPECT_NO_THROW(recovered.resumeCheckpoint(id));
    EXPECT_EQ(recovered.list().front().task.state,TaskState::completed);
    EXPECT_EQ(engineInvocations.load(),initialInvocations + 1);
    expectUnchanged(published);
}

TEST_F(QueueFinalization, ModifiedPublishedOutputIsNeverReplacedOrAdoptedAfterCompletionWriteFailure) {
    TaskQueue queue(database);
    rejectCompletion();
    Events events;
    ASSERT_TRUE(queue.start([&](QueueEvent event) { events.observe(std::move(event)); }));
    queue.wait();
    ASSERT_EQ(queue.list().front().task.state,TaskState::interrupted);
    const auto saved = proof();
    { std::ofstream changed(output,std::ios::binary | std::ios::app); changed << "external edit after publication"; }
    const auto edited = fingerprint();
    ASSERT_NE(edited.sha256,saved.sha256);
    sql("DROP TRIGGER reject_completion");
    EXPECT_THROW(queue.resumeCheckpoint(id),std::invalid_argument);
    EXPECT_EQ(queue.list().front().task.state,TaskState::interrupted);
    EXPECT_EQ(engineInvocations.load(),initialInvocations + 1);
    expectUnchanged(edited);
}

TEST_F(QueueFinalization, FinalProgressWriteFailureAfterPublicationIsRecoverableAndNotAMediaFailure) {
    TaskQueue queue(database);
    sql("CREATE TRIGGER reject_final_progress BEFORE UPDATE OF progress ON task_details "
        "WHEN NEW.progress LIKE '%\"stage\":\"completed\"%' "
        "BEGIN SELECT RAISE(ABORT,'injected published progress write failure'); END");
    emitFinalProgress.store(true);
    Events events;
    ASSERT_TRUE(queue.start([&](QueueEvent event) { events.observe(std::move(event)); }));
    queue.wait();
    EXPECT_TRUE(events.drained); EXPECT_FALSE(queue.busy()); EXPECT_EQ(events.failed,0U);
    EXPECT_NE(events.error.find("final progress could not be saved or delivered"),std::string::npos);
    EXPECT_NE(events.error.find("injected published progress write failure"),std::string::npos);
    ASSERT_EQ(queue.list().front().task.state,TaskState::interrupted);
    const auto published = fingerprint(); const auto saved = proof();
    EXPECT_NO_THROW(queue.resumeCheckpoint(id));
    EXPECT_EQ(queue.list().front().task.state,TaskState::completed);
    EXPECT_EQ(queue.list().front().result,saved.result);
    EXPECT_EQ(engineInvocations.load(),initialInvocations + 1);
    expectUnchanged(published);
}

TEST_F(QueueFinalization, ExceptionAfterPublicationBeforeCompletedEventRetainsProofAndRecovers) {
    TaskQueue queue(database);
    failAfterPublication.store(true);
    Events events;
    ASSERT_TRUE(queue.start([&](QueueEvent event) { events.observe(std::move(event)); }));
    queue.wait();
    EXPECT_TRUE(events.drained); EXPECT_FALSE(queue.busy()); EXPECT_EQ(events.failed,0U);
    EXPECT_NE(events.error.find("publication proof"),std::string::npos);
    EXPECT_NE(events.error.find("injected failure after publication before completed event"),std::string::npos);
    ASSERT_EQ(queue.list().front().task.state,TaskState::interrupted);
    const auto published = fingerprint(); const auto saved = proof();
    EXPECT_NO_THROW(queue.resumeCheckpoint(id));
    EXPECT_EQ(queue.list().front().task.state,TaskState::completed);
    EXPECT_EQ(queue.list().front().result,saved.result);
    EXPECT_EQ(engineInvocations.load(),initialInvocations + 1);
    expectUnchanged(published);
}

TEST_F(QueueFinalization, ConflictingPublicationFileIsPreservedButNeverAdoptedAsValidatedOutput) {
    FileIdentity conflicting;
    {
        TaskQueue queue(database);
        createPublicationConflict.store(true);
        Events events;
        ASSERT_TRUE(queue.start([&](QueueEvent event) { events.observe(std::move(event)); }));
        queue.wait();
        EXPECT_TRUE(events.drained); EXPECT_EQ(events.failed,0U);
        EXPECT_NE(events.error.find("publication proof"),std::string::npos);
        ASSERT_EQ(queue.list().front().task.state,TaskState::interrupted);
        conflicting = fingerprint();
        EXPECT_NE(conflicting.sha256,proof().sha256);
        EXPECT_THROW(queue.resumeCheckpoint(id),std::invalid_argument);
        expectUnchanged(conflicting);
    }
    TaskQueue recovered(database);
    EXPECT_EQ(recovered.recoveredPublishedOutputCount(),0U);
    EXPECT_EQ(recovered.list().front().task.state,TaskState::interrupted);
    EXPECT_NE(recovered.list().front().task.error.find("does not match the saved validation proof"),std::string::npos);
    EXPECT_EQ(engineInvocations.load(),initialInvocations + 1);
    expectUnchanged(conflicting);
}
