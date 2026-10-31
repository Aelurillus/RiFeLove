#pragma once

#include "core/pipeline/video_pipeline.hpp"
#include "media/ffmpeg_helpers.hpp"
#include "media/rife_color.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <future>
#include <numeric>
#include <random>
#include <span>

namespace {
using rifelove::Rational;

// This backend deliberately has no model dependency: it exercises the real
// production CUDA upload/reuse/download contract with deterministic samples.
// It is a numerical timeline fixture, not a substitute RIFE quality test.
class ExposureLinearBackend final : public rifelove::inference::IInterpolationBackend {
public:
    explicit ExposureLinearBackend(rifelove::gpu::CudaDevice& device) : device_(device) {}
    rifelove::inference::ModelCapabilities capabilities() const override {
        return {"test-cuda-linear","test-exposure",64,true,false,{}, {"bt709-sdr8"}};
    }
    void loadModel(const rifelove::inference::ModelPack&) override {}
    std::future<rifelove::inference::InterpolationResult> interpolate(
        rifelove::inference::InterpolationRequest request) override {
        if (request.stop.stop_requested()) throw rifelove::inference::InferenceError(
            rifelove::inference::InferenceErrorKind::cancelled,"Exposure fixture cancelled");
        const auto bytes = request.frame0.pixels->size();
        if (bytes != request.frame1.pixels->size() || bytes % sizeof(float) != 0)
            throw std::invalid_argument("Exposure fixture received incompatible tensors");
        std::vector<float> a(bytes / sizeof(float)), b(a.size());
        request.frame0.pixels->download(std::as_writable_bytes(std::span(a)),0,request.stop);
        request.frame1.pixels->download(std::as_writable_bytes(std::span(b)),0,request.stop);
        for (std::size_t index = 0; index < a.size(); ++index)
            a[index] += (b[index] - a[index]) * request.timestep;
        const bool reused = static_cast<bool>(request.output);
        auto output = request.output ? std::move(request.output) : device_.allocate(bytes,request.stop);
        output->upload(std::as_bytes(std::span(a)),0,request.stop);
        ++calls;
        std::promise<rifelove::inference::InterpolationResult> promise;
        promise.set_value({{std::move(output),request.frame0.width,request.frame0.height},0,reused});
        return promise.get_future();
    }
    std::size_t calls = 0;
private:
    rifelove::gpu::CudaDevice& device_;
};

// Small, lossless, explicitly tagged SDR inputs. Matroska stores milliseconds;
// its actual rational decoded PTS (including quantized high-FPS cadence), rather
// than the nominal rate below, drive the production exposure window.
void writeExposureSource(const std::filesystem::path& path, int rate, int count, int cutAt = -1,
    const std::function<int(int,int,int,int)>& lumaTransform = {},
    AVPixelFormat pixelFormat = AV_PIX_FMT_YUV444P) {
    using rifelove::media::detail::check;
    struct Output {
        AVFormatContext* value = nullptr;
        ~Output() { if (value) { if (value->pb) avio_closep(&value->pb); avformat_free_context(value); } }
    } output;
    struct Codec {
        AVCodecContext* value = nullptr;
        ~Codec() { avcodec_free_context(&value); }
    } codec;
    struct Frame {
        AVFrame* value = av_frame_alloc();
        ~Frame() { av_frame_free(&value); }
    } frame;
    struct Packet {
        AVPacket* value = av_packet_alloc();
        ~Packet() { av_packet_free(&value); }
    } packet;
    const auto* encoder = avcodec_find_encoder_by_name("ffv1");
    if (!encoder) throw std::runtime_error("FFV1 is required for exposure integration fixtures");
    check(avformat_alloc_output_context2(&output.value,nullptr,"matroska",path.string().c_str()),"fixture mux context");
    if (!output.value || !frame.value || !packet.value) throw std::bad_alloc();
    codec.value = avcodec_alloc_context3(encoder);
    if (!codec.value) throw std::bad_alloc();
    codec.value->width = 64; codec.value->height = 64;
    codec.value->pix_fmt = pixelFormat;
    codec.value->time_base = {1,rate}; codec.value->framerate = {rate,1};
    codec.value->color_primaries = AVCOL_PRI_BT709;
    codec.value->color_trc = AVCOL_TRC_BT709;
    codec.value->colorspace = AVCOL_SPC_BT709;
    codec.value->color_range = AVCOL_RANGE_MPEG;
    codec.value->thread_count = 1;
    if (output.value->oformat->flags & AVFMT_GLOBALHEADER) codec.value->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    check(avcodec_open2(codec.value,encoder,nullptr),"fixture FFV1 encoder");
    auto* stream = avformat_new_stream(output.value,nullptr);
    if (!stream) throw std::bad_alloc();
    stream->time_base = codec.value->time_base;
    stream->avg_frame_rate = codec.value->framerate;
    check(avcodec_parameters_from_context(stream->codecpar,codec.value),"fixture codec parameters");
    check(avio_open(&output.value->pb,path.string().c_str(),AVIO_FLAG_WRITE),"fixture file");
    check(avformat_write_header(output.value,nullptr),"fixture header");
    frame.value->format = codec.value->pix_fmt;
    frame.value->width = 64; frame.value->height = 64;
    check(av_frame_get_buffer(frame.value,32),"fixture frame allocation");
    const auto drain = [&] {
        for (;;) {
            const int status = avcodec_receive_packet(codec.value,packet.value);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return;
            check(status,"fixture encoded packet");
            packet.value->duration = 1;
            packet.value->stream_index = stream->index;
            av_packet_rescale_ts(packet.value,codec.value->time_base,stream->time_base);
            check(av_interleaved_write_frame(output.value,packet.value),"fixture mux packet");
            av_packet_unref(packet.value);
        }
    };
    for (int index = 0; index < count; ++index) {
        check(av_frame_make_writable(frame.value),"fixture writable frame");
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
            const bool bar = (x - index % 64 + 64) % 64 < 4;
            const bool after = cutAt >= 0 && index >= cutAt;
            const int luma = cutAt < 0 ? (bar ? 224 : 32) : after ? (bar ? 224 : 192) : (bar ? 64 : 32);
            frame.value->data[0][y * frame.value->linesize[0] + x] = static_cast<std::uint8_t>(lumaTransform ? lumaTransform(index,x,y,luma) : luma);
        }
        const int chromaWidth = pixelFormat == AV_PIX_FMT_YUV420P ? 32 : 64;
        const int chromaHeight = pixelFormat == AV_PIX_FMT_YUV420P ? 32 : 64;
        for (int y = 0; y < chromaHeight; ++y) for (int x = 0; x < chromaWidth; ++x) {
            frame.value->data[1][y * frame.value->linesize[1] + x] = 128;
            frame.value->data[2][y * frame.value->linesize[2] + x] = 128;
        }
        frame.value->pts = index; frame.value->duration = 1;
        check(avcodec_send_frame(codec.value,frame.value),"fixture submit frame");
        drain();
    }
    check(avcodec_send_frame(codec.value,nullptr),"fixture flush encoder");
    drain();
    check(av_write_trailer(output.value),"fixture trailer");
    check(avio_closep(&output.value->pb),"fixture close");
}

struct ExposurePixels {
    std::vector<Rational> timestamps;
    std::vector<std::vector<float>> frames;
};
ExposurePixels readExposurePixels(const std::filesystem::path& path) {
    ExposurePixels result;
    (void)rifelove::media::MediaEngine::decodeVideo(path.string(),[&](const rifelove::media::DecodedFrame& frame) {
        if (!frame.timestamp) throw std::runtime_error("Exposure output lacks PTS");
        result.timestamps.push_back(*frame.timestamp);
        result.frames.push_back(rifelove::media::prepareRifeSdr(frame).pixels);
    });
    return result;
}

class MotionBlurPipeline : public ::testing::Test {
protected:
    void verifyDenseSources();
    void verifyBothSourceEdges();
    void verifySceneCut();
    void verifyBudgetFailure();
    void verifyCancellation();
    void SetUp() override {
        try { device = std::make_unique<rifelove::gpu::CudaDevice>(0,8 * 1024 * 1024); }
        catch (const rifelove::gpu::GpuError& error) {
            if (error.kind() == rifelove::gpu::GpuErrorKind::unavailable) GTEST_SKIP() << error.what();
            throw;
        }
        std::random_device random;
        directory = std::filesystem::temp_directory_path() / ("rifelove-exposure-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Exposure fixture directory collision");
    }
    void TearDown() override {
        if (!directory.empty()) { std::error_code error; std::filesystem::remove_all(directory,error); }
    }
    static rifelove::PipelineSettings settings(Rational target = Rational(60)) {
        rifelove::PipelineSettings result;
        result.targetRate = target; result.encoding.encoder = "ffv1";
        result.motionBlur = {true,360,16};
        return result;
    }
    void expectNoPublication(const std::filesystem::path& destination) {
        EXPECT_FALSE(std::filesystem::exists(destination));
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            EXPECT_EQ(entry.path().filename(),"source.mkv") << "Unpublished artifact leaked: " << entry.path();
    }
    std::filesystem::path directory;
    std::unique_ptr<rifelove::gpu::CudaDevice> device;
};

void MotionBlurPipeline::verifyDenseSources() {
    for (const int sourceRate : {240,300,360,480,600}) for (const auto target : {Rational(60),Rational(60000,1001)}) {
        SCOPED_TRACE("source=" + std::to_string(sourceRate) + " target=" + target.toString());
        const auto stem = std::to_string(sourceRate) + "-" + std::to_string(target.numerator());
        const auto source = directory / (stem + "-source.mkv");
        const auto blurred = directory / (stem + "-blur.mkv");
        const auto sharp = directory / (stem + "-sharp.mkv");
        writeExposureSource(source,sourceRate,sourceRate / 5);
        ExposureLinearBackend backend(*device);
        auto config = settings(target);
        const auto result = rifelove::interpolateVideo(source.string(),blurred.string(),config,*device,backend);
        ASSERT_TRUE(result.motionBlurActive);
        EXPECT_EQ(result.sourceFrames,static_cast<std::size_t>(sourceRate / 5));
        EXPECT_EQ(result.processedFrames,12U);
        EXPECT_EQ(result.motionBlurredFrames + result.motionBlurFallbackFrames,result.processedFrames);
        EXPECT_EQ(result.motionBlurredFrames,result.processedFrames);
        EXPECT_EQ(result.motionBlurFallbackFrames,0U);
        EXPECT_TRUE(result.motionBlurFallbackReasons.empty());
        EXPECT_EQ(result.sceneCuts,0U); EXPECT_EQ(result.abnormalPairs,0U); EXPECT_EQ(result.duplicatePairs,0U);
        EXPECT_GT(result.motionBlurEdgeHeldFrames,0U);
        EXPECT_GT(result.motionBlurPeakWindowFrames,4U);
        EXPECT_LE(result.motionBlurPeakWindowFrames,static_cast<std::size_t>(sourceRate / 59 + 4));
        EXPECT_LE(result.motionBlurPeakPreparedBytes,config.hostFloatBufferLimit);
        EXPECT_EQ(result.inferenceCalls,backend.calls); EXPECT_GT(backend.calls,0U);
        const auto integrated = readExposurePixels(blurred);
        ASSERT_EQ(integrated.frames.size(),result.processedFrames);
        EXPECT_EQ(result.validation.decodedFrames,integrated.frames.size());
        config.motionBlur.enabled = false;
        const auto sharpResult = rifelove::interpolateVideo(source.string(),sharp.string(),config,*device,backend);
        const auto centered = readExposurePixels(sharp);
        ASSERT_EQ(integrated.frames.size(),centered.frames.size());
        EXPECT_EQ(result.video.endExclusive,sharpResult.video.endExclusive);
        EXPECT_EQ(integrated.timestamps,centered.timestamps);
        std::size_t differentInteriorFrames = 0;
        for (std::size_t frame = 1; frame + 1 < integrated.frames.size(); ++frame) {
            double difference = 0;
            for (std::size_t pixel = 0; pixel < integrated.frames[frame].size(); ++pixel)
                difference += std::abs(integrated.frames[frame][pixel] - centered.frames[frame][pixel]);
            if (difference / integrated.frames[frame].size() > 0.005) ++differentInteriorFrames;
        }
        EXPECT_GT(differentInteriorFrames,integrated.frames.size() / 2) << "Exposure must change real pixels, not just counters";
    }
}

void MotionBlurPipeline::verifyBothSourceEdges() {
    const auto source = directory / "source.mkv";
    const auto output = directory / "edges.mkv";
    // The whole clip is shorter than one 360-degree exposure at 60 FPS:
    // the sole output requires both real source-edge holds and EOF draining.
    writeExposureSource(source,600,5);
    ExposureLinearBackend backend(*device);
    const auto result = rifelove::interpolateVideo(source.string(),output.string(),settings(),*device,backend);
    EXPECT_EQ(result.sourceFrames,5U); EXPECT_EQ(result.processedFrames,1U);
    EXPECT_EQ(result.motionBlurredFrames,1U); EXPECT_EQ(result.motionBlurFallbackFrames,0U);
    EXPECT_TRUE(result.motionBlurFallbackReasons.empty());
    EXPECT_EQ(result.motionBlurEdgeHeldFrames,1U);
    EXPECT_EQ(result.validation.decodedFrames,1U);
    EXPECT_GT(backend.calls,0U);
    const auto decoded = readExposurePixels(output);
    const auto original = readExposurePixels(source);
    ASSERT_EQ(decoded.frames.size(),1U); ASSERT_EQ(original.frames.size(),5U);
    EXPECT_EQ(decoded.timestamps.front(),Rational(0));
    double difference = 0;
    for (std::size_t index = 0; index < decoded.frames.front().size(); ++index)
        difference += std::abs(decoded.frames.front()[index] - original.frames.front()[index]);
    EXPECT_GT(difference / decoded.frames.front().size(),0.005);
}

void MotionBlurPipeline::verifySceneCut() {
    const auto source = directory / "source.mkv";
    const auto output = directory / "cut.mkv";
    writeExposureSource(source,600,120,60);
    ExposureLinearBackend backend(*device);
    const auto result = rifelove::interpolateVideo(source.string(),output.string(),settings(),*device,backend);
    EXPECT_EQ(result.sceneCuts,1U);
    EXPECT_EQ(result.motionBlurredFrames + result.motionBlurFallbackFrames,result.processedFrames);
    EXPECT_GT(result.motionBlurredFrames,0U); EXPECT_GT(result.motionBlurFallbackFrames,0U);
    ASSERT_EQ(result.motionBlurFallbackReasons.size(),1U);
    EXPECT_EQ(result.motionBlurFallbackReasons.at("scene_cut_hold"),result.motionBlurFallbackFrames);
    const auto decoded = readExposurePixels(output);
    EXPECT_EQ(decoded.frames.size(),result.processedFrames);
    for (const auto& frame : decoded.frames) {
        const double mean = std::accumulate(frame.begin(),frame.end(),0.0) / frame.size();
        EXPECT_TRUE(mean < 0.30 || mean > 0.60) << "Cross-cut exposure created an intermediate scene: " << mean;
    }
}

void MotionBlurPipeline::verifyBudgetFailure() {
    const auto source = directory / "source.mkv";
    const auto output = directory / "budget.mkv";
    writeExposureSource(source,600,120);
    ExposureLinearBackend backend(*device);
    auto config = settings(); config.hostFloatBufferLimit = 2U * 64 * 64 * 3 * sizeof(float);
    try {
        (void)rifelove::interpolateVideo(source.string(),output.string(),config,*device,backend);
        FAIL() << "Incomplete exposure was silently accepted";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("full shutter"),std::string::npos) << error.what();
    }
    expectNoPublication(output);
}

void MotionBlurPipeline::verifyCancellation() {
    const auto source = directory / "source.mkv";
    const auto output = directory / "cancelled.mkv";
    writeExposureSource(source,600,120);
    ExposureLinearBackend backend(*device);
    std::stop_source stop;
    std::size_t processed = 0;
    try {
        (void)rifelove::interpolateVideo(source.string(),output.string(),settings(),*device,backend,
            [&](const rifelove::PipelineProgress& progress) {
                processed = std::max(processed,progress.processedFrames);
                if (processed >= 2) stop.request_stop();
            },stop.get_token());
        FAIL() << "Cancelled exposure was published";
    } catch (const rifelove::inference::InferenceError& error) {
        EXPECT_EQ(error.kind(),rifelove::inference::InferenceErrorKind::cancelled);
    } catch (const rifelove::media::MediaError& error) {
        EXPECT_TRUE(error.cancelled()) << error.what();
    }
    EXPECT_TRUE(stop.stop_requested()); EXPECT_GE(processed,2U);
    expectNoPublication(output);
}
} // namespace
