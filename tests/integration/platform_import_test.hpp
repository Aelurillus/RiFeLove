#pragma once

#include "application/import_service.hpp"
#include "application/job_serialization.hpp"
#include "inference/model_pack.hpp"
#include "media/ffmpeg_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <span>

namespace rifelove::tests::platform_import {
using Json = nlohmann::json;
inline std::string digest(const std::filesystem::path& path) { return inference::sha256File(path,1024 * 1024); }

// Metadata/graph fixtures are read and hashed by import; no fake graph is
// executed and no CUDA device or inference provider is initialized.
struct Fixture {
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("rifelove-platform-import-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Platform import fixture collision");
        std::filesystem::create_directory(directory / "rife");
        std::filesystem::create_directory(directory / "sr");
        std::filesystem::create_directory(directory / "runtime");
        write(directory / "rife/model.onnx","fixture graph, never executed");
        write(directory / "rife/LICENSE.txt","test fixture");
        write(directory / "rife/model.json",Json{
            {"schemaVersion",1},{"id","rife-fixture"},{"family","rife"},{"version","1"},
            {"modelFile","model.onnx"},{"sha256",digest(directory / "rife/model.onnx")},
            {"license","MIT"},{"licenseFile","LICENSE.txt"},{"inputLayout","NCHW"},{"dtype","float32"},
            {"batch",1},{"channels",3},{"inputRange",Json::array({0.0,1.0})},{"spatialMultiple",64},
            {"timestepShape",Json::array({1,1,1,1})},{"dynamicTimestep",true},{"dynamicSpatial",true}}.dump());
        write(directory / "sr/model.onnx","fixture spatial graph, never executed");
        write(directory / "sr/LICENSE.txt","test fixture");
        write(directory / "sr/source.json",Json{{"license","MIT"},{"upstreamRepository","https://example.invalid/fixture"},
            {"upstreamCommit","fixture-only"}}.dump());
        const Json caps{{"tasks",Json::array({"super-resolution"})},{"placements",Json::array({"output-rate-after-interpolation"})},
            {"supportedInputProfiles",Json::array({"bt709-sdr8"})},{"temporalFramesBefore",0},{"temporalFramesAfter",0},
            {"scaleNumerator",2},{"scaleDenominator",1},{"supportsBoundaryPadding",false}};
        const Json adapter{{"id","image-sr-rgb-f32-nchw-v1"},{"version",1}};
        const auto graphHash = digest(directory / "sr/model.onnx");
        write(directory / "sr/validation.json",Json{{"status","passed_cpu_reference"},{"model_sha256",graphHash},
            {"model_adapter",adapter},{"numerical_profile","fixture-only"},{"validated_backends",Json::array({"onnxruntime_cpu"})},
            {"capabilities",caps},{"cases",Json::array({Json{{"passed",true}}})}}.dump());
        write(directory / "sr/model.json",Json{{"schemaVersion",1},{"id","sr-fixture"},{"family","video-restoration"},
            {"version","1"},{"adapter",adapter},{"modelFile","model.onnx"},{"sha256",graphHash},
            {"license",{{"id","MIT"},{"file","LICENSE.txt"},{"sha256",digest(directory / "sr/LICENSE.txt")}}},
            {"provenance",{{"manifestFile","source.json"},{"manifestSha256",digest(directory / "sr/source.json")}}},
            {"capabilities",caps},{"validation",{{"reportFile","validation.json"},
                {"reportSha256",digest(directory / "sr/validation.json")},
                {"numericalProfile","fixture-only"},{"validatedBackends",Json::array({"onnxruntime_cpu"})}}}}.dump());
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(directory,error); }
    static void write(const std::filesystem::path& path, const std::string& value) {
        std::ofstream stream(path,std::ios::binary); stream << value;
        if (!stream) throw std::runtime_error("Could not write platform import fixture");
    }
};

inline void writeSource(const std::filesystem::path& path, int width, int height, bool yuv444 = false, bool tenBit = false) {
    using media::detail::check;
    struct Resources {
        AVFormatContext* output = nullptr;
        AVCodecContext* codec = nullptr;
        AVFrame* frame = av_frame_alloc();
        AVPacket* packet = av_packet_alloc();
        ~Resources() {
            av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&codec);
            if (output) { if (output->pb) avio_closep(&output->pb); avformat_free_context(output); }
        }
    } resources;
    const auto* encoder = avcodec_find_encoder_by_name("ffv1");
    if (!encoder) throw std::runtime_error("FFV1 is required for CPU import fixtures");
    check(avformat_alloc_output_context2(&resources.output,nullptr,"matroska",path.string().c_str()),"platform fixture output");
    resources.codec = avcodec_alloc_context3(encoder);
    if (!resources.output || !resources.codec || !resources.frame || !resources.packet) throw std::bad_alloc();
    auto* codec = resources.codec;
    if (tenBit && !yuv444) throw std::invalid_argument("10-bit fixture requires 4:4:4");
    codec->width = width; codec->height = height;
    codec->pix_fmt = tenBit ? AV_PIX_FMT_YUV444P10LE : yuv444 ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_YUV420P;
    codec->time_base = {1,30}; codec->framerate = {30,1}; codec->thread_count = 1;
    codec->color_primaries = AVCOL_PRI_BT709; codec->color_trc = AVCOL_TRC_BT709;
    codec->colorspace = AVCOL_SPC_BT709; codec->color_range = AVCOL_RANGE_MPEG;
    if (resources.output->oformat->flags & AVFMT_GLOBALHEADER) codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    check(avcodec_open2(codec,encoder,nullptr),"platform fixture encoder");
    auto* stream = avformat_new_stream(resources.output,nullptr);
    if (!stream) throw std::bad_alloc();
    stream->time_base = codec->time_base; stream->avg_frame_rate = codec->framerate;
    check(avcodec_parameters_from_context(stream->codecpar,codec),"platform fixture stream");
    check(avio_open(&resources.output->pb,path.string().c_str(),AVIO_FLAG_WRITE),"platform fixture file");
    check(avformat_write_header(resources.output,nullptr),"platform fixture header");
    auto* frame = resources.frame;
    frame->format = codec->pix_fmt; frame->width = width; frame->height = height;
    frame->pts = 0; frame->duration = 1;
    check(av_frame_get_buffer(frame,32),"platform fixture pixels");
    for (int p = 0; p < 3; ++p) for (int y = 0; y < (p && !yuv444 ? height / 2 : height); ++y) {
        auto* row = frame->data[p] + y * frame->linesize[p];
        const int samples = p && !yuv444 ? width / 2 : width;
        if (tenBit) std::fill_n(reinterpret_cast<std::uint16_t*>(row),samples,static_cast<std::uint16_t>(p ? 512 : 256));
        else std::memset(row,p ? 128 : 64,static_cast<std::size_t>(samples));
    }
    const auto drain = [&] {
        for (;;) {
            const int status = avcodec_receive_packet(codec,resources.packet);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return;
            check(status,"platform fixture packet");
            resources.packet->duration = 1; resources.packet->stream_index = stream->index;
            av_packet_rescale_ts(resources.packet,codec->time_base,stream->time_base);
            check(av_interleaved_write_frame(resources.output,resources.packet),"platform fixture mux");
            av_packet_unref(resources.packet);
        }
    };
    check(avcodec_send_frame(codec,frame),"platform fixture submit"); drain();
    check(avcodec_send_frame(codec,nullptr),"platform fixture flush"); drain();
    check(av_write_trailer(resources.output),"platform fixture trailer");
    check(avio_closep(&resources.output->pb),"platform fixture close");
}
} // namespace rifelove::tests::platform_import

namespace rifelove::tests::platform_import {
inline void verifyPersistedPolicy() {
    using namespace rifelove;
    using namespace rifelove::application;
    using namespace rifelove::tests::platform_import;
    Fixture fixture;
    const auto landscape = fixture.directory / "landscape.mkv", portrait = fixture.directory / "portrait.mkv";
    writeSource(landscape,1280,720); writeSource(portrait,1080,1920);
    int batch = 0;
    for (const auto profile : {PlatformBitrate::youtube,PlatformBitrate::localQuality})
        for (const auto rate : {Rational(30000,1001),Rational(60000,1001)}) for (const int scale : {1,2}) {
            SCOPED_TRACE("profile=" + std::to_string(static_cast<int>(profile)) + " fps=" + rate.toString() + " scale=" + std::to_string(scale));
            TaskQueue queue(fixture.directory / ("queue-" + std::to_string(batch++) + ".sqlite"));
            ImportRequest request;
            request.inputs = {landscape,portrait}; request.outputDirectory = fixture.directory;
            request.settings.modelDirectory = fixture.directory / "rife";
            request.settings.runtimeDirectory = fixture.directory / "runtime";
            request.settings.settings.targetRate = rate; request.settings.settings.encoding.encoder = "auto";
            // Deliberately incorrect copied settings must be recalculated per input.
            request.settings.settings.encoding.bitRate = 123; request.settings.settings.encoding.gopSize = 987;
            if (scale == 2) request.settings.restorationModelDirectory = fixture.directory / "sr";
            request.platformBitrate = profile; request.container = "mp4";
            const auto imported = importVideos(queue,request);
            for (const auto& failure : imported.rejected) ADD_FAILURE() << failure.path << ": " << failure.detail;
            ASSERT_EQ(imported.taskIds.size(),2U);
            const auto rows = queue.list(); ASSERT_EQ(rows.size(),2U);
            for (const auto& row : rows) {
                const auto job = parseJob(row.task.sourcePath,row.task.outputPath,row.configuration);
                const bool wide = std::filesystem::path(job.source).filename() == landscape.filename();
                const int shortEdge = (wide ? 720 : 1080) * scale;
                const int fps = rate == Rational(30000,1001) ? 30 : 60;
                const auto expectedBitRate = platformBitrateKbps(profile,shortEdge,fps) * std::int64_t{1000};
                EXPECT_EQ(job.settings.targetRate,rate);
                EXPECT_EQ(job.settings.encoding.bitRate,expectedBitRate);
                EXPECT_EQ(job.settings.encoding.gopSize,profile == PlatformBitrate::youtube ? fps / 2 : fps * 2);
                EXPECT_EQ(job.settings.encoding.outputWidth,0); EXPECT_EQ(job.settings.encoding.outputHeight,0);
                EXPECT_EQ(row.task.state,TaskState::queued);
                EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(job.output)));
            }
        }
}
} // namespace rifelove::tests::platform_import
