#include "core/pipeline/output_validation.hpp"
#include "media/video_writer.hpp"
#include "application/job_serialization.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

using namespace rifelove;

namespace {
class OutputValidationFixture : public ::testing::Test {
protected:
    void SetUp() override {
        std::random_device random;
        directory = std::filesystem::temp_directory_path() / ("rifelove-output-time-" + std::to_string(random()));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        source = directory / "source.mkv";
        const auto command = "ffmpeg.exe -v error -nostdin -f lavfi -i \"color=black:size=192x128:rate=4:duration=0.75\" "
            "-f lavfi -i \"sine=frequency=440:sample_rate=48000:duration=0.9\" "
            "-vf \"setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=limited\" "
            "-c:v libopenh264 -pix_fmt yuv420p -c:a aac -avoid_negative_ts disabled \"" + source.string() + "\"";
        ASSERT_EQ(std::system(command.c_str()),0);
        input = media::MediaEngine::probe(source.string());
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory,error);
    }
    std::filesystem::path transcode(const std::string& name, bool copyAudio = true) {
        const auto output = directory / name;
        media::VideoEncodingSettings settings;
        settings.encoder = "libopenh264";
        settings.copyAuxiliaryStreams = copyAudio;
        (void)media::transcodeVideo(source.string(),output.string(),settings);
        return output;
    }
    OutputTimingValidation validate(const std::filesystem::path& path, const media::MediaInfo& reference,
        Rational origin = {}, Rational end = Rational(3,4), std::size_t count = 3) {
        return detail::validatePipelineOutput(path.string(),0,192,128,origin,end,Rational(4),count,reference);
    }
    void replaceBytes(const std::filesystem::path& path, const std::string& before, const std::string& after) {
        ASSERT_EQ(before.size(),after.size());
        std::ifstream file(path,std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
        file.close();
        const auto index = bytes.find(before);
        ASSERT_NE(index,std::string::npos) << before;
        bytes.replace(index,before.size(),after);
        std::ofstream output(path,std::ios::binary | std::ios::trunc);
        output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output.good());
    }
    std::filesystem::path directory;
    std::filesystem::path source;
    media::MediaInfo input;
};
}

TEST_F(OutputValidationFixture, H264AndAacTailPreserveIndependentTrackEndsAcrossContainers) {
    ASSERT_EQ(input.streams.size(),2U);
    EXPECT_EQ(input.streams[0].declaredEnd,Rational(3,4));
    ASSERT_TRUE(input.streams[1].declaredEnd);
    EXPECT_GT(*input.streams[1].declaredEnd,Rational(3,4));
    for (const auto* extension : {"mkv","mp4","mov"}) {
        SCOPED_TRACE(extension);
        const auto path = transcode(std::string("result.") + extension);
        const auto result = validate(path,input);
        EXPECT_EQ(result.decodedFrames,3U);
        ASSERT_TRUE(result.declaredEndError);
        EXPECT_EQ(*result.declaredEndError,Rational(0));
        EXPECT_LE(result.auxiliaryMaximumEndError,result.timeBase);
    }
}

TEST_F(OutputValidationFixture, VfrSourceLongOutputKeepsValidationTimelineForFreshAndCheckpointedRuns) {
    // Earlier fixtures contained only three frames, so none reached the
    // validator's 120-frame progress boundary. A real desktop/queue observer
    // serializes these callbacks after generation has already finished.
    const auto vfrSource = directory / "vfr-source.mkv";
    const auto output = directory / "long-output.mkv";
    const auto sourceCommand = "ffmpeg.exe -v error -nostdin -f lavfi -i \"color=black:size=192x128:rate=60:duration=4\" "
        "-vf \"select='not(eq(mod(n,5),1))',setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=limited\" "
        "-fps_mode vfr -c:v libopenh264 -pix_fmt yuv420p \"" + vfrSource.string() + "\"";
    ASSERT_EQ(std::system(sourceCommand.c_str()),0);
    const auto outputCommand = "ffmpeg.exe -v error -nostdin -i \"" + vfrSource.string() + "\" "
        "-vf fps=60 -c:v libopenh264 -pix_fmt yuv420p \"" + output.string() + "\"";
    ASSERT_EQ(std::system(outputCommand.c_str()),0);
    const auto reference = media::MediaEngine::probe(vfrSource.string());
    std::optional<Rational> previous, previousInterval;
    bool varyingIntervals = false;
    const auto sourceScan = media::MediaEngine::decodeVideo(vfrSource.string(),[&](const media::DecodedFrame& frame) {
        ASSERT_TRUE(frame.timestamp);
        if (previous) {
            const auto interval = *frame.timestamp - *previous;
            if (previousInterval && interval != *previousInterval) varyingIntervals = true;
            previousInterval = interval;
        }
        previous = frame.timestamp;
    });
    ASSERT_TRUE(varyingIntervals);
    ASSERT_LT(sourceScan.frameCount,240U);
    for (const auto reused : {std::size_t{0},std::size_t{180},std::size_t{240}}) {
        SCOPED_TRACE("reused=" + std::to_string(reused));
        std::size_t events = 0;
        const auto validation = detail::validatePipelineOutput(output.string(),0,192,128,Rational(0),Rational(4),
            Rational(60),240,reference,[&](const PipelineProgress& value) {
                ++events;
                EXPECT_EQ(value.stage,"validating");
                EXPECT_EQ(value.outputFrames,240U);
                EXPECT_EQ(value.detail,"Verified " + std::to_string(events * 120) + " / 240 output frames");
                // Mirror the production pipeline/service handoff: processing
                // counters cover the complete output, not the partial scan.
                application::ProcessingEvent event;
                event.stage = value.stage;
                event.outputFrames = value.outputFrames;
                event.processedFrames = 240 - reused;
                event.reusedFrames = reused;
                event.timestamp = value.timestamp;
                event.detail = value.detail;
                EXPECT_NO_THROW((void)application::serializeProgress(event));
            });
        EXPECT_EQ(events,2U);
        EXPECT_EQ(validation.decodedFrames,240U);
        EXPECT_GE(validation.durationError,Rational(0) - validation.timeBase);
        EXPECT_LE(validation.durationError,validation.timeBase);
        EXPECT_LE(validation.maximumTimestampError,validation.timeBase / Rational(2));
    }
}

TEST_F(OutputValidationFixture, IncorrectVideoTrackEndRemainsAValidationFailure) {
    const auto path = transcode("wrong-video-end.mkv");
    replaceBytes(path,"00:00:00.750000000","00:00:00.650000000");
    EXPECT_THROW((void)validate(path,input),std::runtime_error);
}

TEST_F(OutputValidationFixture, IncorrectCopiedAudioEndRemainsAValidationFailure) {
    const auto path = transcode("wrong-audio-end.mkv");
    const auto info = media::MediaEngine::probe(path.string());
    const auto tag = info.streams[1].metadata.at("DURATION");
    ASSERT_NE(tag,"00:00:00.500000000");
    replaceBytes(path,tag,"00:00:00.500000000");
    EXPECT_THROW((void)validate(path,input),std::runtime_error);
}

TEST_F(OutputValidationFixture, DecodedCountAndTimestampChecksRemainMandatory) {
    const auto path = transcode("wrong-count-or-origin.mkv");
    EXPECT_THROW((void)validate(path,input,{},Rational(3,4),4),std::runtime_error);
    EXPECT_THROW((void)validate(path,input,Rational(1,10)),std::runtime_error);
}

TEST_F(OutputValidationFixture, MissingTrackTagDoesNotBypassTheDecodedFinalBoundaryCheck) {
    const auto path = transcode("no-track-tag.mkv",false);
    const auto reference = media::MediaEngine::probe(path.string());
    replaceBytes(path,"DURATION","XURATION");
    const auto missingTag = media::MediaEngine::probe(path.string());
    ASSERT_FALSE(missingTag.streams[0].declaredEnd);
    EXPECT_EQ(validate(path,reference).decodedFrames,3U);
    EXPECT_THROW((void)validate(path,reference,{},Rational(7,10)),std::runtime_error);
}
