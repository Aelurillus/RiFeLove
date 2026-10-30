#include "media/media_engine.hpp"
#include "media/nvenc_diagnostic.hpp"
#include "media/video_writer.hpp"
#include "media/ffmpeg_helpers.hpp"
#include "core/pipeline/output_validation.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <cstdlib>

using rifelove::media::DecodedFrame;
using rifelove::media::MediaEngine;
using rifelove::media::MediaError;

namespace {
class DeclaredEndFixture : public ::testing::Test {
protected:
    void SetUp() override {
        input = avformat_alloc_context();
        ASSERT_NE(input,nullptr);
        input->iformat = av_find_input_format("matroska");
        ASSERT_NE(input->iformat,nullptr);
        stream = avformat_new_stream(input,nullptr);
        ASSERT_NE(stream,nullptr);
        stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        stream->time_base = {1,1000};
        stream->start_time = 0;
        // The observed FFmpeg fill_all_stream_timings fallback, not a track
        // declaration: a 750 ms video shares a file with longer audio.
        stream->duration = 921;
        input->duration = 921000;
    }
    void TearDown() override { avformat_free_context(input); }
    AVFormatContext* input = nullptr;
    AVStream* stream = nullptr;
};
}

TEST_F(DeclaredEndFixture, MatroskaTrackEndOverridesContainerDerivedStreamDuration) {
    ASSERT_EQ(av_dict_set(&stream->metadata,"DURATION","00:00:00.750000000",0),0);
    EXPECT_EQ(rifelove::media::detail::declaredEnd(input,stream),rifelove::Rational(3,4));
    stream->start_time = 250;
    // DURATION is an absolute track end, not a length to add to start_time.
    EXPECT_EQ(rifelove::media::detail::declaredEnd(input,stream),rifelove::Rational(3,4));
}

TEST_F(DeclaredEndFixture, MatroskaAudioCodecDelayUsesTheDemuxedPacketClock) {
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->initial_padding = 1024;
    stream->codecpar->sample_rate = 48000;
    ASSERT_EQ(av_dict_set(&stream->metadata,"DURATION","00:00:00.771000000",0),0);
    EXPECT_EQ(rifelove::media::detail::declaredEnd(input,stream),rifelove::Rational(3,4));
}

TEST_F(DeclaredEndFixture, MatroskaMissingOrMalformedTrackEndDoesNotUseContainerDuration) {
    EXPECT_FALSE(rifelove::media::detail::declaredEnd(input,stream));
    for (const char* tag : {"", "bad", "00:61:00.000000000", "00:00:00.1234567890", "9999999999999999999999:00:00"}) {
        ASSERT_EQ(av_dict_set(&stream->metadata,"DURATION",tag,0),0);
        EXPECT_FALSE(rifelove::media::detail::declaredEnd(input,stream)) << tag;
    }
}

TEST_F(DeclaredEndFixture, OtherContainersKeepTheirStreamDurationAndOrigin) {
    input->iformat = av_find_input_format("mov");
    ASSERT_NE(input->iformat,nullptr);
    stream->start_time = 250;
    stream->duration = 750;
    ASSERT_EQ(av_dict_set(&stream->metadata,"DURATION","00:00:09.999000000",0),0);
    EXPECT_EQ(rifelove::media::detail::declaredEnd(input,stream),rifelove::Rational(1));
}

namespace {
class ImageFixture {
public:
    ImageFixture() {
        std::random_device random;
        directory = std::filesystem::temp_directory_path() / ("rifelove-media-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("test directory collision");
        path = directory / "image.ppm";
        std::ofstream file(path, std::ios::binary);
        file << "P6\n2 1\n255\n";
        const std::array<unsigned char, 6> pixels{255, 0, 0, 0, 255, 0};
        file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        if (!file) throw std::runtime_error("could not create image fixture");
    }
    ~ImageFixture() {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) std::filesystem::remove(entry.path(), error);
        std::filesystem::remove(directory, error);
    }
    std::filesystem::path directory;
    std::filesystem::path path;
};
}

TEST(MediaEngine, DecodedPixelsRemainOwnedAfterDecoderCloses) {
    ImageFixture fixture;
    const auto info = MediaEngine::probe(fixture.path.string());
    ASSERT_EQ(info.streams.size(), 1U);
    EXPECT_EQ(info.streams[0].codec, "ppm");
    EXPECT_EQ(info.streams[0].width, 2);
    DecodedFrame retained{};
    const auto summary = MediaEngine::decodeVideo(fixture.path.string(), [&](const DecodedFrame& frame) { retained = frame; });
    EXPECT_EQ(summary.frameCount, 1U);
    ASSERT_TRUE(retained.storage);
    ASSERT_NE(retained.planes[0].data, nullptr);
    EXPECT_EQ(retained.color.pixelFormat, "rgb24");
    EXPECT_EQ(retained.color.bitDepth, 8);
    EXPECT_EQ(retained.planes[0].data[0], 255);
    EXPECT_EQ(retained.planes[0].data[4], 255);
}

TEST(MediaEngine, NvencApiMismatchReportsDriverAndFfmpegRequirements) {
    const auto mismatch = rifelove::media::findNvencApiMismatch(
        rifelove::media::NvencApiVersion{12,2},{13,0},"570.0");
    ASSERT_TRUE(mismatch);
    const auto reason = rifelove::media::nvencApiMismatchReason(*mismatch);
    EXPECT_NE(reason.find("Required: 13.0 Found: 12.2"),std::string::npos);
    EXPECT_NE(reason.find("minimum required Nvidia driver for nvenc is 570.0 or newer"),std::string::npos);
}

TEST(MediaEngine, NvencApiMismatchOmitsUnknownMinimumDriverVersion) {
    const auto mismatch = rifelove::media::findNvencApiMismatch(
        rifelove::media::NvencApiVersion{12,2},{13,0},"");
    ASSERT_TRUE(mismatch);
    const auto reason = rifelove::media::nvencApiMismatchReason(*mismatch);
    EXPECT_NE(reason.find("Required: 13.0 Found: 12.2"),std::string::npos);
    EXPECT_EQ(reason.find("minimum required Nvidia driver"),std::string::npos);
}

TEST(MediaEngine, NvencApiMismatchIsNotReportedWhenTheDriverIsCompatible) {
    EXPECT_FALSE(rifelove::media::findNvencApiMismatch(
        rifelove::media::NvencApiVersion{13,0},{13,0},"570.0"));
    EXPECT_FALSE(rifelove::media::findNvencApiMismatch(
        rifelove::media::NvencApiVersion{13,1},{13,0},"570.0"));
    EXPECT_FALSE(rifelove::media::findNvencApiMismatch(std::nullopt,{13,0},"570.0"));
}

TEST(MediaEngine, PreferCudaFallsBackForSoftwareOnlyDecoders) {
    ImageFixture fixture;
    DecodedFrame retained{};
    const auto summary = MediaEngine::decodeVideo(fixture.path.string(),
        [&](const DecodedFrame& frame) { retained = frame; },-1,{},
        {rifelove::media::DecodeAcceleration::preferCuda,0});
    EXPECT_EQ(summary.frameCount,1U);
    EXPECT_EQ(summary.decoder,"ppm");
    EXPECT_EQ(summary.acceleration,"software");
    EXPECT_FALSE(summary.accelerationFallbackReason.empty());
    ASSERT_TRUE(retained.storage);
    EXPECT_EQ(retained.planes[0].data[0],255);
}

TEST(MediaEngine, CancellationIsExplicitAndRetainedFramesStayValid) {
    ImageFixture fixture;
    std::stop_source source;
    DecodedFrame retained{};
    try {
        (void)MediaEngine::decodeVideo(fixture.path.string(), [&](const DecodedFrame& frame) {
            retained = frame;
            source.request_stop();
        }, -1, source.get_token());
        FAIL() << "expected cancellation";
    } catch (const MediaError& error) { EXPECT_TRUE(error.cancelled()); }
    ASSERT_TRUE(retained.storage);
    EXPECT_EQ(retained.planes[0].data[0], 255);
    EXPECT_THROW((void)MediaEngine::probe(fixture.path.string(), source.get_token()), MediaError);
    const auto output = fixture.directory / "cancelled.mp4";
    EXPECT_THROW((void)MediaEngine::remux(fixture.path.string(), output.string(), source.get_token()), MediaError);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(MediaEngine, RejectsNetworkInputsBeforeOpeningDemuxer) {
    EXPECT_THROW((void)MediaEngine::probe("https://example.invalid/video.mp4"), std::invalid_argument);
}

TEST(MediaEngine, ExactPreviewSeeksToTheNearestTaggedSdrPresentationFrame) {
    ImageFixture fixture;
    const auto source = fixture.directory / "preview-source.mkv";
    const auto command = "ffmpeg.exe -y -v error -f lavfi -i \"testsrc2=size=96x64:rate=4:duration=1\" "
        "-vf \"setparams=range=tv:color_primaries=bt709:color_trc=bt709:colorspace=bt709\" "
        "-c:v ffv1 -pix_fmt yuv420p \"" + source.string() + "\"";
    ASSERT_EQ(std::system(command.c_str()),0) << "Could not create tagged local preview fixture";

    const auto first = MediaEngine::decodeFrameNearTime(source.string(),rifelove::Rational(37,100));
    EXPECT_EQ(first.timestamp,rifelove::Rational(1,4));
    EXPECT_EQ(first.duration,rifelove::Rational(1,4));
    EXPECT_EQ(first.previousTimestamp,rifelove::Rational(0));
    EXPECT_EQ(first.nextTimestamp,rifelove::Rational(1,2));
    EXPECT_EQ(first.width,96);
    EXPECT_EQ(first.height,64);
    ASSERT_EQ(first.rgba.size(),96U * 64U * 4U);
    EXPECT_EQ(first.color.primaries,"bt709");
    EXPECT_EQ(first.color.matrix,"bt709");
    for (std::size_t index = 3; index < first.rgba.size(); index += 4)
        EXPECT_EQ(first.rgba[index],255U);

    const auto second = MediaEngine::decodeFrameNearTime(source.string(),rifelove::Rational(42,100));
    EXPECT_EQ(second.timestamp,rifelove::Rational(1,2));
    EXPECT_EQ(second.previousTimestamp,rifelove::Rational(1,4));
    EXPECT_EQ(second.nextTimestamp,rifelove::Rational(3,4));
    const auto last = MediaEngine::decodeFrameNearTime(source.string(),rifelove::Rational(2));
    EXPECT_EQ(last.timestamp,rifelove::Rational(3,4));
    EXPECT_EQ(last.previousTimestamp,rifelove::Rational(1,2));
    EXPECT_FALSE(last.nextTimestamp.has_value());
}

TEST(VideoMuxWriter, StrongerValidationRunsBeforePublicationAndFailureRemovesOutput) {
    ImageFixture fixture;
    DecodedFrame retained{};
    (void)MediaEngine::decodeVideo(fixture.path.string(),[&](const DecodedFrame& frame) { retained = frame; });
    rifelove::media::VideoEncodingSettings settings; settings.encoder = "ffv1"; settings.pixelFormat = "bgr0";
    settings.timeBase = rifelove::Rational(1,50); settings.frameRate = rifelove::Rational(50);
    const auto path = fixture.directory / "failed-validation.mkv";
    rifelove::media::VideoMuxWriter writer(fixture.path.string(),path.string(),settings);
    writer.writeFrame(retained,{},rifelove::Rational(1,50));
    bool checked = false;
    EXPECT_THROW((void)writer.finish([&](const std::string& temporary) {
        checked = true; EXPECT_FALSE(std::filesystem::exists(path)); EXPECT_EQ(MediaEngine::probe(temporary).streams.size(),1U);
        throw std::runtime_error("injected validation failure");
    }),std::runtime_error);
    EXPECT_TRUE(checked); EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(VideoMuxWriter, WritesGeneratedFramesAtExplicitTimes) {
    ImageFixture fixture;
    DecodedFrame retained{};
    (void)MediaEngine::decodeVideo(fixture.path.string(), [&](const DecodedFrame& frame) { retained = frame; });
    rifelove::media::VideoEncodingSettings settings;
    settings.encoder = "ffv1";
    settings.pixelFormat = "bgr0";
    settings.timeBase = rifelove::Rational(1, 50);
    settings.frameRate = rifelove::Rational(50);
    const auto output = fixture.directory / "generated.mkv";
    rifelove::media::VideoMuxWriter writer(fixture.path.string(), output.string(), settings);
    for (int i = 0; i < 3; ++i) writer.writeFrame(retained, rifelove::Rational(i, 50), rifelove::Rational(1, 50));
    const auto result = writer.finish();
    EXPECT_EQ(result.framesSubmitted, 3U);
    EXPECT_EQ(result.endExclusive, rifelove::Rational(3, 50));
    const auto decoded = MediaEngine::decodeVideo(output.string());
    EXPECT_EQ(decoded.frameCount, 3U);
    EXPECT_EQ(decoded.firstTimestamp, rifelove::Rational(0));
    EXPECT_EQ(decoded.lastTimestamp, rifelove::Rational(1, 25));
    EXPECT_THROW((void)writer.finish(), std::logic_error);
    EXPECT_EQ(retained.planes[0].data[0], 255);
}

TEST(VideoMuxWriter, SupportsExplicitRestoredOutputDimensions) {
    ImageFixture fixture;
    DecodedFrame source{};
    (void)MediaEngine::decodeVideo(fixture.path.string(),[&](const DecodedFrame& frame) { source = frame; });
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(4U * 2U * 3U,127);
    DecodedFrame restored = source;
    restored.width = 4;
    restored.height = 2;
    restored.planes[0] = {pixels->data(),12};
    restored.storage = pixels;

    rifelove::media::VideoEncodingSettings settings;
    settings.encoder = "ffv1";
    settings.pixelFormat = "bgr0";
    settings.timeBase = rifelove::Rational(1,50);
    settings.frameRate = rifelove::Rational(50);
    settings.outputWidth = 4;
    settings.outputHeight = 2;
    const auto output = fixture.directory / "restored.mkv";
    rifelove::media::VideoMuxWriter writer(fixture.path.string(),output.string(),settings);
    writer.writeFrame(restored,{},rifelove::Rational(1,50));
    EXPECT_EQ(writer.finish().framesSubmitted,1U);
    const auto result = MediaEngine::probe(output.string());
    ASSERT_EQ(result.streams.size(),1U);
    EXPECT_EQ(result.streams.front().width,4);
    EXPECT_EQ(result.streams.front().height,2);
}

TEST(VideoMuxWriter, ResamplesRestoredFramesToAnExplicitTargetWithoutChangingTimeline) {
    ImageFixture fixture;
    DecodedFrame source{};
    (void)MediaEngine::decodeVideo(fixture.path.string(),[&](const DecodedFrame& frame) { source = frame; });
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(4U * 2U * 3U,127);
    DecodedFrame restored = source;
    restored.width = 4;
    restored.height = 2;
    restored.planes[0] = {pixels->data(),12};
    restored.storage = pixels;

    rifelove::media::VideoEncodingSettings settings;
    settings.encoder = "ffv1";
    settings.pixelFormat = "bgr0";
    settings.timeBase = rifelove::Rational(1,50);
    settings.frameRate = rifelove::Rational(50);
    settings.outputWidth = 2;
    settings.outputHeight = 1;
    const auto output = fixture.directory / "resampled-target.mkv";
    rifelove::media::VideoMuxWriter writer(fixture.path.string(),output.string(),settings);
    writer.writeFrame(restored,{},rifelove::Rational(1,50));
    EXPECT_EQ(writer.finish().framesSubmitted,1U);
    const auto info = MediaEngine::probe(output.string());
    ASSERT_EQ(info.streams.size(),1U);
    EXPECT_EQ(info.streams.front().width,2);
    EXPECT_EQ(info.streams.front().height,1);
    const auto decoded = MediaEngine::decodeVideo(output.string());
    EXPECT_EQ(decoded.frameCount,1U);
    EXPECT_EQ(decoded.firstTimestamp,rifelove::Rational(0));
}

TEST(VideoMuxWriter, RejectsOnlyOneExplicitOutputDimension) {
    ImageFixture fixture;
    rifelove::media::VideoEncodingSettings settings;
    settings.encoder = "ffv1";
    settings.outputWidth = 4;
    EXPECT_THROW((void)rifelove::media::VideoMuxWriter(fixture.path.string(),(fixture.directory / "invalid-dimensions.mkv").string(),settings),std::invalid_argument);
}

TEST(VideoMuxWriter, RejectsTargetAspectRatioChangesInsteadOfStretching) {
    ImageFixture fixture;
    rifelove::media::VideoEncodingSettings settings;
    settings.encoder = "ffv1";
    settings.outputWidth = 3;
    settings.outputHeight = 1;
    EXPECT_THROW((void)rifelove::media::VideoMuxWriter(fixture.path.string(),(fixture.directory / "aspect-change.mkv").string(),settings),std::invalid_argument);
}

TEST(VideoMuxWriter, RejectsGeneratedFrameAspectRatioChangesInsteadOfStretching) {
    ImageFixture fixture;
    DecodedFrame source{};
    (void)MediaEngine::decodeVideo(fixture.path.string(),[&](const DecodedFrame& frame) { source = frame; });
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(4U * 3U * 3U,127);
    DecodedFrame malformed = source;
    malformed.width = 4;
    malformed.height = 3;
    malformed.planes[0] = {pixels->data(),12};
    malformed.storage = pixels;

    rifelove::media::VideoEncodingSettings settings;
    settings.encoder = "ffv1";
    settings.pixelFormat = "bgr0";
    settings.outputWidth = 2;
    settings.outputHeight = 1;
    const auto output = fixture.directory / "frame-aspect-change.mkv";
    rifelove::media::VideoMuxWriter writer(fixture.path.string(),output.string(),settings);
    EXPECT_THROW(writer.writeFrame(malformed,{},rifelove::Rational(1,50)),std::invalid_argument);
}

TEST(VideoMuxWriter, WritesVideoOnlyKeyframedSegmentsAtTheGlobalTimelineOrigin) {
    ImageFixture fixture;
    const auto source = fixture.directory / "source-with-audio.mkv";
    const auto command = "ffmpeg.exe -y -v error -f lavfi -i \"testsrc2=size=96x64:rate=5:duration=0.6\" "
        "-f lavfi -i \"sine=frequency=440:sample_rate=48000:duration=0.6\" -c:v ffv1 -pix_fmt yuv420p -c:a flac \"" +
        source.string() + "\"";
    ASSERT_EQ(std::system(command.c_str()),0) << "Could not create local segment fixture with FFmpeg";
    const auto sourceInfo = MediaEngine::probe(source.string());
    ASSERT_EQ(sourceInfo.streams.size(),2U);
    const auto audio = std::find_if(sourceInfo.streams.begin(),sourceInfo.streams.end(),[](const auto& stream) { return stream.type == "audio"; });
    ASSERT_NE(audio,sourceInfo.streams.end());
    EXPECT_EQ(audio->sampleRate,48000);
    EXPECT_EQ(audio->channels,1);
    EXPECT_EQ(audio->channelLayout,"mono");
    EXPECT_FALSE(audio->sampleFormat.empty());
    DecodedFrame retained{};
    (void)MediaEngine::decodeVideo(source.string(),[&](const DecodedFrame& frame) { if (!retained.storage) retained = frame; });
    rifelove::media::VideoEncodingSettings settings;
    settings.encoder = "ffv1";
    settings.pixelFormat = "yuv420p";
    settings.timeBase = rifelove::Rational(1,50);
    settings.frameRate = rifelove::Rational(50);
    settings.copyAuxiliaryStreams = false;
    settings.requireFirstPacketKeyframe = true;
    const auto output = fixture.directory / "segment-000000.mkv";
    rifelove::media::VideoMuxWriter writer(source.string(),output.string(),settings);
    const auto start = rifelove::Rational(3,2);
    writer.writeFrame(retained,start,rifelove::Rational(1,50));
    writer.writeFrame(retained,start + rifelove::Rational(1,50),rifelove::Rational(1,50));
    const auto result = writer.finish();
    EXPECT_EQ(result.firstTimestamp,start);
    EXPECT_EQ(result.endExclusive,start + rifelove::Rational(2,50));
    EXPECT_EQ(result.framesSubmitted,2U);
    EXPECT_EQ(result.copiedPackets,0U);
    const auto info = MediaEngine::probe(output.string());
    ASSERT_EQ(info.streams.size(),1U);
    EXPECT_EQ(info.streams.front().type,"video");
    const auto decoded = MediaEngine::decodeVideo(output.string());
    EXPECT_EQ(decoded.frameCount,2U);
    EXPECT_EQ(decoded.firstTimestamp,start);
    EXPECT_EQ(decoded.lastTimestamp,start + rifelove::Rational(1,50));

    rifelove::media::VideoSegmentContract contract;
    contract.frameCount = 2;
    contract.firstTimestamp = start;
    contract.frameDuration = rifelove::Rational(1,50);
    contract.width = 96;
    contract.height = 64;
    contract.codec = "ffv1";
    contract.pixelFormat = "yuv420p";
    contract.bitDepth = 8;
    const auto verified = MediaEngine::verifyVideoSegment(output.string(),contract);
    EXPECT_EQ(verified.frameCount,2U);
    EXPECT_EQ(verified.firstTimestamp,start);
    EXPECT_EQ(verified.endExclusive,start + rifelove::Rational(2,50));
    auto wrongCount = contract;
    wrongCount.frameCount = 1;
    EXPECT_THROW((void)MediaEngine::verifyVideoSegment(output.string(),wrongCount),std::runtime_error);
    auto wrongOrigin = contract;
    wrongOrigin.firstTimestamp = rifelove::Rational(0);
    EXPECT_THROW((void)MediaEngine::verifyVideoSegment(output.string(),wrongOrigin),std::runtime_error);
    auto wrongColor = contract;
    wrongColor.primaries = "bt709";
    EXPECT_THROW((void)MediaEngine::verifyVideoSegment(output.string(),wrongColor),std::runtime_error);
}

TEST(VideoMuxWriter, ResamplingKeepsCopiedAudioAndVideoTimeline) {
    ImageFixture fixture;
    const auto source = fixture.directory / "resample-source-with-audio.mkv";
    const auto command = "ffmpeg.exe -y -v error -f lavfi -i \"testsrc2=size=96x64:rate=5:duration=0.6\" "
        "-f lavfi -i \"sine=frequency=440:sample_rate=48000:duration=0.6\" -c:v ffv1 -pix_fmt yuv420p -c:a flac \"" +
        source.string() + "\"";
    ASSERT_EQ(std::system(command.c_str()),0) << "Could not create local resampling fixture with FFmpeg";
    DecodedFrame retained{};
    (void)MediaEngine::decodeVideo(source.string(),[&](const DecodedFrame& frame) { if (!retained.storage) retained = frame; });

    rifelove::media::VideoEncodingSettings settings;
    settings.encoder = "ffv1";
    settings.pixelFormat = "yuv420p";
    settings.timeBase = rifelove::Rational(1,5);
    settings.frameRate = rifelove::Rational(5);
    settings.outputWidth = 48;
    settings.outputHeight = 32;
    const auto output = fixture.directory / "resampled-with-audio.mkv";
    rifelove::media::VideoMuxWriter writer(source.string(),output.string(),settings);
    for (int index = 0; index < 3; ++index)
        writer.writeFrame(retained,rifelove::Rational(index,5),rifelove::Rational(1,5));
    EXPECT_EQ(writer.finish().framesSubmitted,3U);

    const auto info = MediaEngine::probe(output.string());
    const auto video = std::find_if(info.streams.begin(),info.streams.end(),[](const auto& stream) { return stream.type == "video"; });
    const auto audio = std::find_if(info.streams.begin(),info.streams.end(),[](const auto& stream) { return stream.type == "audio"; });
    ASSERT_NE(video,info.streams.end());
    ASSERT_NE(audio,info.streams.end());
    EXPECT_EQ(video->width,48);
    EXPECT_EQ(video->height,32);
    EXPECT_EQ(audio->codec,"flac");
    EXPECT_EQ(audio->sampleRate,48000);
    EXPECT_EQ(audio->channels,1);
    EXPECT_EQ(MediaEngine::decodeVideo(output.string()).frameCount,3U);
}

TEST(MediaEngine, AssemblesVerifiedVideoSegmentsAndCopiesOriginalAudioOnce) {
    ImageFixture fixture;
    const auto source = fixture.directory / "assembly-source.mkv";
    const auto command = "ffmpeg.exe -y -v error -f lavfi -i \"testsrc2=size=96x64:rate=5:duration=0.6\" "
        "-f lavfi -i \"sine=frequency=440:sample_rate=48000:duration=0.6\" -c:v ffv1 -pix_fmt yuv420p -c:a flac \"" +
        source.string() + "\"";
    ASSERT_EQ(std::system(command.c_str()),0) << "Could not create local segment assembly fixture with FFmpeg";
    DecodedFrame retained{};
    (void)MediaEngine::decodeVideo(source.string(),[&](const DecodedFrame& frame) { if (!retained.storage) retained = frame; });

    rifelove::media::VideoEncodingSettings settings;
    settings.encoder = "ffv1";
    settings.pixelFormat = "yuv420p";
    settings.timeBase = rifelove::Rational(1,100);
    settings.frameRate = rifelove::Rational(50);
    settings.copyAuxiliaryStreams = false;
    settings.requireFirstPacketKeyframe = true;
    const auto makeSegment = [&](const std::string& name, rifelove::Rational first, rifelove::Rational lastDuration) {
        const auto path = fixture.directory / name;
        rifelove::media::VideoMuxWriter writer(source.string(),path.string(),settings);
        for (int i = 0; i < 2; ++i)
            writer.writeFrame(retained,first + rifelove::Rational(i,50),i == 1 ? lastDuration : rifelove::Rational(1,50));
        (void)writer.finish();
        rifelove::media::VideoSegmentContract contract;
        contract.frameCount = 2;
        contract.firstTimestamp = first;
        contract.frameDuration = rifelove::Rational(1,50);
        contract.endExclusive = first + rifelove::Rational(1,50) + lastDuration;
        contract.width = 96;
        contract.height = 64;
        contract.codec = "ffv1";
        contract.pixelFormat = "yuv420p";
        contract.bitDepth = 8;
        return rifelove::media::VideoSegmentInput{path.string(),contract};
    };
    const std::vector<rifelove::media::VideoSegmentInput> segments{
        makeSegment("assembled-part-000000.mkv",rifelove::Rational(0),rifelove::Rational(1,50)),
        makeSegment("assembled-part-000001.mkv",rifelove::Rational(2,50),rifelove::Rational(1,100))};

    const auto output = fixture.directory / "assembled.mkv";
    bool validatedBeforePublish = false;
    const auto result = MediaEngine::assembleVideoSegments(source.string(),output.string(),-1,segments,{},[&](const std::string& temporary) {
        validatedBeforePublish = true;
        EXPECT_TRUE(std::filesystem::exists(temporary));
        EXPECT_FALSE(std::filesystem::exists(output));
        EXPECT_EQ(MediaEngine::probe(temporary).streams.size(),2U);
    });
    EXPECT_TRUE(validatedBeforePublish);
    EXPECT_EQ(result.segments,2U);
    EXPECT_EQ(result.videoPackets,4U);
    EXPECT_GT(result.auxiliaryPackets,0U);
    EXPECT_EQ(result.firstTimestamp,rifelove::Rational(0));
    EXPECT_EQ(result.endExclusive,rifelove::Rational(7,100));
    const auto info = MediaEngine::probe(output.string());
    ASSERT_EQ(info.streams.size(),2U);
    EXPECT_EQ(info.streams[0].type,"video");
    EXPECT_EQ(info.streams[1].type,"audio");
    EXPECT_EQ(info.streams[1].codec,"flac");
    const auto decoded = MediaEngine::decodeVideo(output.string());
    EXPECT_EQ(decoded.frameCount,4U);
    EXPECT_EQ(decoded.firstTimestamp,rifelove::Rational(0));
    EXPECT_EQ(decoded.lastTimestamp,rifelove::Rational(3,50));

    auto gap = segments;
    gap[1].contract.firstTimestamp = rifelove::Rational(3,50);
    const auto rejectedOutput = fixture.directory / "rejected-assembly.mkv";
    EXPECT_THROW((void)MediaEngine::assembleVideoSegments(source.string(),rejectedOutput.string(),-1,gap),std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(rejectedOutput));

    const auto validationFailureOutput = fixture.directory / "rejected-validation.mkv";
    EXPECT_THROW((void)MediaEngine::assembleVideoSegments(source.string(),validationFailureOutput.string(),-1,segments,{},[](const std::string&) {
        throw std::runtime_error("validation rejected the assembled output");
    }),std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(validationFailureOutput));
}

TEST(MediaEngine, VerifiesHighRateSegmentWithQuantizedFinalDuration) {
    ImageFixture fixture;
    DecodedFrame retained{};
    (void)MediaEngine::decodeVideo(fixture.path.string(), [&](const DecodedFrame& frame) { if (!retained.storage) retained = frame; });
    rifelove::media::VideoEncodingSettings settings;
    settings.encoder = "ffv1";
    settings.pixelFormat = "bgr0";
    settings.timeBase = rifelove::Rational(1,360);
    settings.frameRate = rifelove::Rational(360);
    settings.copyAuxiliaryStreams = false;
    settings.requireFirstPacketKeyframe = true;
    const auto path = fixture.directory / "high-rate-segment.mkv";
    rifelove::media::VideoMuxWriter writer(fixture.path.string(),path.string(),settings);
    for (int index = 0; index < 17; ++index)
        writer.writeFrame(retained,rifelove::Rational(index,360),rifelove::Rational(1,360));
    (void)writer.finish();
    rifelove::media::VideoSegmentContract contract;
    contract.frameCount = 17; contract.firstTimestamp = rifelove::Rational(0);
    contract.frameDuration = rifelove::Rational(1,360); contract.endExclusive = rifelove::Rational(17,360);
    contract.width = 2; contract.height = 1; contract.codec = "ffv1"; contract.pixelFormat = "bgr0"; contract.bitDepth = 8;
    EXPECT_NO_THROW((void)MediaEngine::verifyVideoSegment(path.string(),contract));
    const auto assembled = fixture.directory / "high-rate-assembled.mkv";
    ASSERT_NO_THROW((void)MediaEngine::assembleVideoSegments(fixture.path.string(),assembled.string(),-1,{{path.string(),contract}}));
    const auto assembledInfo = MediaEngine::probe(assembled.string());
    const auto validated = rifelove::detail::validatePipelineOutput(assembled.string(),0,2,1,
        rifelove::Rational(0),rifelove::Rational(17,360),rifelove::Rational(360),17,assembledInfo);
    EXPECT_EQ(validated.decodedFrames,17U);
    EXPECT_TRUE(validated.quantized);
    EXPECT_THROW((void)rifelove::detail::validatePipelineOutput(assembled.string(),0,2,1,
        rifelove::Rational(0),rifelove::Rational(16,360) + rifelove::Rational(1,10000),rifelove::Rational(360),17,assembledInfo),std::runtime_error);
    auto invalid = contract;
    invalid.endExclusive = rifelove::Rational(16,360) + rifelove::Rational(1,10000);
    EXPECT_THROW((void)MediaEngine::verifyVideoSegment(path.string(),invalid),std::runtime_error);
}

TEST(MediaEngine, AssemblesHighRateMatroskaAcrossUnevenSegments) {
    ImageFixture fixture;
    DecodedFrame retained{};
    (void)MediaEngine::decodeVideo(fixture.path.string(), [&](const DecodedFrame& frame) { if (!retained.storage) retained = frame; });
    for (const int fps : {360, 480}) {
        SCOPED_TRACE(fps);
        rifelove::media::VideoEncodingSettings settings;
        settings.encoder = "ffv1"; settings.pixelFormat = "bgr0";
        settings.timeBase = rifelove::Rational(1,fps); settings.frameRate = rifelove::Rational(fps);
        settings.copyAuxiliaryStreams = false; settings.requireFirstPacketKeyframe = true;
        std::vector<rifelove::media::VideoSegmentInput> segments;
        for (int segmentIndex = 0; segmentIndex < 2; ++segmentIndex) {
            const int offset = segmentIndex == 0 ? 0 : 17;
            const int count = segmentIndex == 0 ? 17 : 13;
            const auto path = fixture.directory / ("high-rate-" + std::to_string(fps) + "-" + std::to_string(segmentIndex) + ".mkv");
            rifelove::media::VideoMuxWriter writer(fixture.path.string(),path.string(),settings);
            for (int index = 0; index < count; ++index)
                writer.writeFrame(retained,rifelove::Rational(offset + index,fps),rifelove::Rational(1,fps));
            (void)writer.finish();
            rifelove::media::VideoSegmentContract contract;
            contract.frameCount = static_cast<std::size_t>(count);
            contract.firstTimestamp = rifelove::Rational(offset,fps);
            contract.frameDuration = rifelove::Rational(1,fps);
            contract.endExclusive = rifelove::Rational(offset + count,fps);
            contract.width = 2; contract.height = 1; contract.codec = "ffv1"; contract.pixelFormat = "bgr0"; contract.bitDepth = 8;
            segments.push_back({path.string(),contract});
        }
        const auto output = fixture.directory / ("high-rate-assembled-" + std::to_string(fps) + ".mkv");
        const auto result = MediaEngine::assembleVideoSegments(fixture.path.string(),output.string(),-1,segments);
        EXPECT_EQ(result.videoPackets,30U);
        const auto info = MediaEngine::probe(output.string());
        const auto verified = rifelove::detail::validatePipelineOutput(output.string(),0,2,1,
            rifelove::Rational(0),rifelove::Rational(30,fps),rifelove::Rational(fps),30,info);
        EXPECT_EQ(verified.decodedFrames,30U);
        auto broken = segments;
        broken[1].contract.firstTimestamp = rifelove::Rational(18,fps);
        const auto rejected = fixture.directory / ("high-rate-rejected-" + std::to_string(fps) + ".mkv");
        EXPECT_THROW((void)MediaEngine::assembleVideoSegments(fixture.path.string(),rejected.string(),-1,broken),std::invalid_argument);
        EXPECT_FALSE(std::filesystem::exists(rejected));
    }
}

TEST(MediaEngine, RestoresExactMp4AndMovTimingFromMillisecondCheckpoints) {
    ImageFixture fixture;
    const auto source = fixture.directory / "exact-assembly-source.mkv";
    const auto command = "ffmpeg.exe -y -v error -f lavfi -i \"testsrc2=size=96x64:rate=12:duration=0.5\" "
        "-c:v ffv1 -pix_fmt yuv420p \"" + source.string() + "\"";
    ASSERT_EQ(std::system(command.c_str()),0);
    DecodedFrame retained{};
    (void)MediaEngine::decodeVideo(source.string(),[&](const DecodedFrame& frame) { if (!retained.storage) retained = frame; });
    struct Scenario { rifelove::Rational rate; const char* extension; bool shortenedLastFrame; int maxBFrames = 0; };
    const std::vector<Scenario> scenarios{{rifelove::Rational(60),".mp4",false},
        {rifelove::Rational(60000,1001),".mov",false},
        {rifelove::Rational(360),".mp4",true},
        {rifelove::Rational(60),".mov",false,2}};
    for (std::size_t scenarioIndex = 0; scenarioIndex < scenarios.size(); ++scenarioIndex) {
        const auto& scenario = scenarios[scenarioIndex];
        SCOPED_TRACE(scenario.rate.toString() + scenario.extension);
        const auto period = rifelove::Rational(1) / scenario.rate;
        const auto expectedEnd = period * rifelove::Rational(30) - (scenario.shortenedLastFrame ? period / rifelove::Rational(2) : rifelove::Rational(0));
        rifelove::media::VideoEncodingSettings settings;
        settings.encoder = "mpeg4";
        settings.pixelFormat = "yuv420p";
        settings.timeBase = rifelove::Rational(1,scenario.rate.numerator() * (scenario.shortenedLastFrame ? 2 : 1));
        settings.frameRate = scenario.rate;
        settings.copyAuxiliaryStreams = false;
        settings.requireFirstPacketKeyframe = true;
        settings.maxBFrames = scenario.maxBFrames;
        std::vector<rifelove::media::VideoSegmentInput> segments;
        for (int segmentIndex = 0; segmentIndex < 2; ++segmentIndex) {
            const auto first = period * rifelove::Rational(segmentIndex == 0 ? 0 : 18);
            const std::size_t count = segmentIndex == 0 ? 18 : 12;
            const auto end = segmentIndex == 0 ? period * rifelove::Rational(18) : expectedEnd;
            const auto path = fixture.directory / ("exact-part-" + std::to_string(scenarioIndex) + "-" + std::to_string(segmentIndex) + ".mkv");
            rifelove::media::VideoMuxWriter writer(source.string(),path.string(),settings);
            for (std::size_t index = 0; index < count; ++index) {
                const auto timestamp = first + period * rifelove::Rational(static_cast<std::int64_t>(index));
                writer.writeFrame(retained,timestamp,index + 1 == count ? end - timestamp : period);
            }
            (void)writer.finish();
            rifelove::media::VideoSegmentContract contract;
            contract.frameCount = count;
            contract.firstTimestamp = first;
            contract.frameDuration = period;
            contract.endExclusive = end;
            contract.width = 96;
            contract.height = 64;
            contract.codec = "mpeg4";
            contract.pixelFormat = "yuv420p";
            contract.bitDepth = 8;
            segments.push_back({path.string(),contract});
        }
        const auto output = fixture.directory / ("exact-assembled-" + std::to_string(scenarioIndex) + scenario.extension);
        const auto result = MediaEngine::assembleVideoSegments(source.string(),output.string(),-1,segments);
        EXPECT_EQ(result.videoPackets,30U);
        EXPECT_EQ(result.endExclusive,expectedEnd);
        const auto info = MediaEngine::probe(output.string());
        ASSERT_EQ(info.streams.size(),1U);
        EXPECT_EQ(info.streams[0].declaredEnd,expectedEnd);
        std::size_t count = 0;
        std::optional<rifelove::Rational> decodedEnd;
        const auto decoded = MediaEngine::decodeVideo(output.string(),[&](const DecodedFrame& frame) {
            ASSERT_TRUE(frame.timestamp);
            EXPECT_EQ(*frame.timestamp,period * rifelove::Rational(static_cast<std::int64_t>(count++)));
            ASSERT_TRUE(frame.duration);
            decodedEnd = *frame.timestamp + *frame.duration;
        });
        EXPECT_EQ(decoded.frameCount,30U);
        EXPECT_EQ(decodedEnd,expectedEnd);
    }
}

TEST(VideoMuxWriter, InvalidTimingAndCancellationRemovePartialFiles) {
    ImageFixture fixture;
    DecodedFrame retained{};
    (void)MediaEngine::decodeVideo(fixture.path.string(), [&](const DecodedFrame& frame) { retained = frame; });
    rifelove::media::VideoEncodingSettings settings;
    settings.encoder = "ffv1";
    settings.pixelFormat = "bgr0";
    settings.timeBase = rifelove::Rational(1, 50);
    settings.frameRate = rifelove::Rational(50);
    const auto output = fixture.directory / "invalid.mkv";
    rifelove::media::VideoMuxWriter invalid(fixture.path.string(), output.string(), settings);
    EXPECT_THROW(invalid.writeFrame(retained, rifelove::Rational(1, 3), rifelove::Rational(1, 50)), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(output));
    std::stop_source stop;
    const auto cancelled = fixture.directory / "cancelled.mkv";
    rifelove::media::VideoMuxWriter writer(fixture.path.string(), cancelled.string(), settings, stop.get_token());
    writer.writeFrame(retained, rifelove::Rational(0), rifelove::Rational(1, 50));
    stop.request_stop();
    EXPECT_THROW(writer.writeFrame(retained, rifelove::Rational(1, 50), rifelove::Rational(1, 50)), MediaError);
    EXPECT_FALSE(std::filesystem::exists(cancelled));
    EXPECT_EQ(static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(fixture.directory), std::filesystem::directory_iterator())), 1U);
}

TEST(VideoMuxWriter, FractionalRateAndLastDurationRemainExactAcrossAThousandFrames) {
    ImageFixture fixture;
    const auto input = fixture.directory / "source.y4m";
    {
        std::ofstream file(input, std::ios::binary);
        file << "YUV4MPEG2 W16 H16 F24000:1001 Ip A1:1 C420jpeg\nFRAME\n";
        std::vector<unsigned char> pixels(384, 128);
        std::fill(pixels.begin(), pixels.begin() + 256, static_cast<unsigned char>(90));
        file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    }
    DecodedFrame retained{};
    (void)MediaEngine::decodeVideo(input.string(), [&](const DecodedFrame& frame) { retained = frame; });
    rifelove::media::VideoEncodingSettings settings;
    settings.encoder = "libopenh264";
    settings.timeBase = rifelove::Rational(1, 60000);
    settings.frameRate = rifelove::Rational(60000, 1001);
    settings.bitRate = 300000;
    const auto output = fixture.directory / "fractional.mp4";
    rifelove::media::VideoMuxWriter writer(input.string(), output.string(), settings);
    for (int i = 0; i < 1000; ++i) writer.writeFrame(retained, rifelove::Rational(i * 1001, 60000), rifelove::Rational(1001, 60000));
    const auto result = writer.finish();
    EXPECT_EQ(result.endExclusive, rifelove::Rational(1001, 60));
    std::size_t frames = 0;
    const auto summary = MediaEngine::decodeVideo(output.string(), [&](const DecodedFrame& frame) {
        EXPECT_EQ(frame.timestamp, rifelove::Rational(static_cast<std::int64_t>(frames) * 1001, 60000));
        ++frames;
    });
    EXPECT_EQ(summary.frameCount, 1000U);
    const auto info = MediaEngine::probe(output.string());
    ASSERT_FALSE(info.streams.empty());
    EXPECT_EQ(info.streams[0].duration, result.endExclusive);
}
