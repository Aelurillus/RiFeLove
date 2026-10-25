#include "media/ffmpeg_helpers.hpp"
#include "media/rife_color.hpp"
#include "media/timestamp_recovery.hpp"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <cmath>
#include <limits>
#include <string>

namespace rifelove::media {
namespace {
void freeCodecContext(AVCodecContext* value) { avcodec_free_context(&value); }
void freeBufferReference(AVBufferRef* value) { if (value) av_buffer_unref(&value); }
void freeDecodedFrame(AVFrame* value) { av_frame_free(&value); }

std::string ffmpegError(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
    if (av_strerror(code,message.data(),message.size()) < 0) return "FFmpeg error " + std::to_string(code);
    return message.data();
}

struct HardwareFormatSelection {
    bool requested = false;
    AVPixelFormat format = AV_PIX_FMT_NONE;
    bool selectedCuda = false;
    bool selectedSoftware = false;
};

AVPixelFormat selectDecodeFormat(AVCodecContext* context, const AVPixelFormat* formats) {
    auto* selection = static_cast<HardwareFormatSelection*>(context->opaque);
    if (selection && selection->requested) {
        for (auto current = formats; *current != AV_PIX_FMT_NONE; ++current) {
            if (*current == selection->format) {
                selection->selectedCuda = true;
                return *current;
            }
        }
        selection->selectedSoftware = true;
        for (auto current = formats; *current != AV_PIX_FMT_NONE; ++current) {
            const auto* description = av_pix_fmt_desc_get(*current);
            if (description && !(description->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *current;
        }
    }
    return formats[0];
}

std::unique_ptr<AVCodecContext, decltype(&freeCodecContext)> makeDecoderContext(
    const AVCodec* decoder, const AVStream* stream) {
    std::unique_ptr<AVCodecContext, decltype(&freeCodecContext)> context(avcodec_alloc_context3(decoder),freeCodecContext);
    if (!context) throw std::bad_alloc();
    detail::check(avcodec_parameters_to_context(context.get(),stream->codecpar),"configure video decoder");
    context->pkt_timebase = stream->time_base;
    return context;
}
}

DecodeSummary MediaEngine::decodeVideo(const std::string& path,
    const std::function<void(const DecodedFrame&)>& onFrame, int streamIndex, std::stop_token stop, DecodeOptions options) {
    detail::Input input(path, stop);
    if (streamIndex < 0) streamIndex = av_find_best_stream(input.context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    detail::check(streamIndex, "find video stream");
    if (static_cast<unsigned>(streamIndex) >= input.context->nb_streams ||
        input.context->streams[streamIndex]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
        throw std::invalid_argument("selected stream is not a video stream");
    auto* stream = input.context->streams[streamIndex];
    const auto streamColor = detail::color(stream->codecpar);
    const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) throw std::runtime_error("no decoder is available for this video codec");
    auto freePacket = [](AVPacket* p) { av_packet_free(&p); };
    auto freeFrame = &freeDecodedFrame;
    std::unique_ptr<AVCodecContext, decltype(&freeCodecContext)> context(nullptr,freeCodecContext);
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
    std::unique_ptr<AVFrame, decltype(freeFrame)> frame(av_frame_alloc(), freeFrame);
    std::unique_ptr<AVFrame, decltype(freeFrame)> hostFrame(av_frame_alloc(),freeFrame);
    if (!packet || !frame || !hostFrame) throw std::bad_alloc();

    HardwareFormatSelection formatSelection;
    std::unique_ptr<AVBufferRef,decltype(&freeBufferReference)> hardwareDevice(nullptr,freeBufferReference);
    std::string accelerationFallbackReason;
    if (options.acceleration == DecodeAcceleration::preferCuda) {
        if (options.deviceOrdinal < 0) throw std::invalid_argument("CUDA decode device index must be non-negative");
        const AVCodecHWConfig* configuration = nullptr;
        for (int index = 0;; ++index) {
            const auto* candidate = avcodec_get_hw_config(decoder,index);
            if (!candidate) break;
            if (candidate->device_type == AV_HWDEVICE_TYPE_CUDA &&
                (candidate->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
                configuration = candidate;
                break;
            }
        }
        if (!configuration) {
            accelerationFallbackReason = "selected video decoder has no CUDA hardware configuration";
        } else {
            AVBufferRef* createdDevice = nullptr;
            const auto deviceName = std::to_string(options.deviceOrdinal);
            const int deviceStatus = av_hwdevice_ctx_create(&createdDevice,configuration->device_type,deviceName.c_str(),nullptr,0);
            if (deviceStatus < 0) {
                accelerationFallbackReason = "CUDA video decode device is unavailable: " + ffmpegError(deviceStatus);
                if (createdDevice) av_buffer_unref(&createdDevice);
            } else {
                hardwareDevice.reset(createdDevice);
                formatSelection = {true,configuration->pix_fmt,false,false};
                context = makeDecoderContext(decoder,stream);
                context->hw_device_ctx = av_buffer_ref(hardwareDevice.get());
                if (!context->hw_device_ctx) {
                    accelerationFallbackReason = "could not retain the CUDA video decode device";
                    context.reset();
                } else {
                    context->opaque = &formatSelection;
                    context->get_format = selectDecodeFormat;
                    const int openStatus = avcodec_open2(context.get(),decoder,nullptr);
                    if (openStatus < 0) {
                        accelerationFallbackReason = "CUDA video decoder could not open: " + ffmpegError(openStatus);
                        context.reset();
                        formatSelection = {};
                    }
                }
            }
        }
    }
    if (!context) {
        context = makeDecoderContext(decoder,stream);
        detail::check(avcodec_open2(context.get(),decoder,nullptr),"open video decoder");
    }

    DecodeSummary summary;
    summary.decoder = decoder->name ? decoder->name : "unknown";
    std::optional<Rational> previous;
    std::optional<Rational> delta;
    bool variable = false;
    const auto streamStart = detail::timestamp(stream->start_time,stream->time_base);
    TimestampRecovery timestampRecovery(streamStart);
    const auto rate = stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0 ? stream->avg_frame_rate : stream->r_frame_rate;
    const bool hasRate = rate.num > 0 && rate.den > 0;
    bool matchesQuantizedConstantRate = hasRate;
    bool sawCudaFrame = false;
    bool sawSoftwareFrame = false;
    auto receive = [&]() {
        for (;;) {
            detail::checkStopped(stop);
            const int status = avcodec_receive_frame(context.get(), frame.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return status;
            detail::check(status, "receive decoded frame");
            AVFrame* decodedFrame = frame.get();
            if (frame->format == formatSelection.format && formatSelection.selectedCuda) {
                av_frame_unref(hostFrame.get());
                detail::check(av_hwframe_transfer_data(hostFrame.get(),frame.get(),0),"transfer NVDEC frame to host memory");
                detail::check(av_frame_copy_props(hostFrame.get(),frame.get()),"copy NVDEC frame properties");
                decodedFrame = hostFrame.get();
                sawCudaFrame = true;
            } else {
                sawSoftwareFrame = true;
                if (options.acceleration == DecodeAcceleration::preferCuda &&
                    accelerationFallbackReason.empty() && !formatSelection.selectedCuda)
                    accelerationFallbackReason = "decoder selected a software pixel format instead of CUDA";
            }
            const auto pts = decodedFrame->best_effort_timestamp != AV_NOPTS_VALUE ? decodedFrame->best_effort_timestamp : decodedFrame->pts;
            const auto observedTimestamp = detail::timestamp(pts, stream->time_base);
            const auto duration = decodedFrame->duration > 0 ? detail::timestamp(decodedFrame->duration,stream->time_base) : std::optional<Rational>{};
            const auto resolution = timestampRecovery.resolve(observedTimestamp,duration);
            const auto timestamp = resolution.timestamp;
            if (!timestamp) ++summary.missingTimestamps;
            else {
                if (resolution.recovered) ++summary.recoveredTimestamps;
                if (!summary.firstTimestamp) summary.firstTimestamp = timestamp;
                summary.lastTimestamp = timestamp;
                if (hasRate) {
                    const auto expected = *summary.firstTimestamp + Rational(static_cast<std::int64_t>(summary.frameCount)) / detail::rational(rate);
                    auto error = *timestamp - expected;
                    if (error < Rational(0)) error = Rational(0) - error;
                    if (error > detail::rational(stream->time_base)) matchesQuantizedConstantRate = false;
                }
                if (previous) {
                    const auto step = *timestamp - *previous;
                    if (step <= Rational(0)) ++summary.nonIncreasingTimestamps;
                    else if (!delta) delta = step;
                    else if (step != *delta) variable = true;
                }
                previous = timestamp;
            }
            if (onFrame) {
                std::shared_ptr<AVFrame> retained(av_frame_clone(decodedFrame), freeFrame);
                if (!retained) throw std::bad_alloc();
                DecodedFrame output{};
                output.index = summary.frameCount;
                output.width = retained->width;
                output.height = retained->height;
                output.timestamp = timestamp;
                output.duration = duration;
                output.timestampRecovered = resolution.recovered;
                output.color = normalizeDecodedSdrColor(detail::color(retained.get()), streamColor);
                for (std::size_t i = 0; i < output.planes.size(); ++i) output.planes[i] = {retained->data[i], retained->linesize[i]};
                output.storage = std::move(retained);
                onFrame(output);
            }
            ++summary.frameCount;
            av_frame_unref(frame.get());
            av_frame_unref(hostFrame.get());
        }
    };

    for (;;) {
        detail::checkStopped(stop);
        const int read = av_read_frame(input.context, packet.get());
        if (read == AVERROR_EOF) break;
        detail::check(read, "read media packet");
        if (packet->stream_index == streamIndex) {
            int sent = avcodec_send_packet(context.get(), packet.get());
            if (sent == AVERROR(EAGAIN)) { receive(); sent = avcodec_send_packet(context.get(), packet.get()); }
            detail::check(sent, "send video packet to decoder");
            receive();
        }
        av_packet_unref(packet.get());
    }
    detail::check(avcodec_send_packet(context.get(), nullptr), "flush video decoder");
    if (receive() != AVERROR_EOF) throw std::runtime_error("decoder did not reach EOF after flushing");
    if (summary.frameCount == 0) throw std::runtime_error("video decoder produced no frames");
    if (sawCudaFrame && sawSoftwareFrame) {
        summary.acceleration = "mixed";
        if (accelerationFallbackReason.empty())
            accelerationFallbackReason = "decoder returned both CUDA and software frames";
    }
    else if (sawCudaFrame) summary.acceleration = "cuda";
    if (options.acceleration == DecodeAcceleration::preferCuda && formatSelection.selectedSoftware && accelerationFallbackReason.empty())
        accelerationFallbackReason = "decoder did not negotiate CUDA frames";
    summary.accelerationFallbackReason = accelerationFallbackReason;
    if (summary.missingTimestamps || summary.nonIncreasingTimestamps) summary.cadence = "invalid_timestamps";
    else if (delta) summary.cadence = variable && !matchesQuantizedConstantRate ? "variable" : "constant";
    return summary;
}

DecodedPreviewFrame MediaEngine::decodeFrameNearTime(const std::string& path, Rational targetTime,
    int streamIndex, std::stop_token stop) {
    detail::Input input(path,stop);
    if (streamIndex < 0) streamIndex = av_find_best_stream(input.context,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
    detail::check(streamIndex,"find preview video stream");
    if (static_cast<unsigned>(streamIndex) >= input.context->nb_streams ||
        input.context->streams[streamIndex]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
        throw std::invalid_argument("selected preview stream is not a video stream");
    auto* stream = input.context->streams[streamIndex];
    if (targetTime.denominator() > std::numeric_limits<int>::max())
        throw std::invalid_argument("preview timestamp denominator exceeds FFmpeg's supported range");
    const auto targetPts = av_rescale_q_rnd(targetTime.numerator(),AVRational{1,static_cast<int>(targetTime.denominator())},
        stream->time_base,static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    detail::checkStopped(stop);
    int seek = av_seek_frame(input.context,streamIndex,targetPts,AVSEEK_FLAG_BACKWARD);
    if (seek < 0) seek = avformat_seek_file(input.context,streamIndex,INT64_MIN,targetPts,INT64_MAX,AVSEEK_FLAG_BACKWARD);
    detail::check(seek,"seek preview video frame");

    const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) throw std::runtime_error("no decoder is available for this preview video codec");
    const auto freeCodec = [](AVCodecContext* value) { avcodec_free_context(&value); };
    const auto freePacket = [](AVPacket* value) { av_packet_free(&value); };
    auto freeFrame = +[](AVFrame* value) { av_frame_free(&value); };
    std::unique_ptr<AVCodecContext,decltype(freeCodec)> context(avcodec_alloc_context3(decoder),freeCodec);
    std::unique_ptr<AVPacket,decltype(freePacket)> packet(av_packet_alloc(),freePacket);
    std::unique_ptr<AVFrame,decltype(freeFrame)> frame(av_frame_alloc(),freeFrame);
    std::unique_ptr<AVFrame,decltype(freeFrame)> before(nullptr,freeFrame);
    std::unique_ptr<AVFrame,decltype(freeFrame)> selected(nullptr,freeFrame);
    if (!context || !packet || !frame) throw std::bad_alloc();
    detail::check(avcodec_parameters_to_context(context.get(),stream->codecpar),"configure preview video decoder");
    context->pkt_timebase = stream->time_base;
    detail::check(avcodec_open2(context.get(),decoder,nullptr),"open preview video decoder");

    const auto streamColor = detail::color(stream->codecpar);
    std::int64_t beforePts = AV_NOPTS_VALUE;
    std::int64_t beforePreviousPts = AV_NOPTS_VALUE;
    std::int64_t selectedPts = AV_NOPTS_VALUE;
    std::int64_t selectedPreviousPts = AV_NOPTS_VALUE;
    std::int64_t selectedNextPts = AV_NOPTS_VALUE;
    bool awaitingSelectedNext = false;
    bool found = false;
    const auto receive = [&]() {
        for (;;) {
            detail::checkStopped(stop);
            const int status = avcodec_receive_frame(context.get(),frame.get());
            if (status == AVERROR(EAGAIN)) return 0;
            if (status == AVERROR_EOF) return 1;
            detail::check(status,"receive preview video frame");
            const auto pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
            if (pts != AV_NOPTS_VALUE) {
                if (selected && awaitingSelectedNext && pts > selectedPts) {
                    selectedNextPts = pts;
                    awaitingSelectedNext = false;
                    found = true;
                } else if (!selected && pts == targetPts) {
                    selected.reset(av_frame_clone(frame.get()));
                    if (!selected) throw std::bad_alloc();
                    selectedPts = pts;
                    selectedPreviousPts = beforePts;
                    awaitingSelectedNext = true;
                } else if (!selected && pts < targetPts) {
                    beforePreviousPts = beforePts;
                    before.reset(av_frame_clone(frame.get()));
                    if (!before) throw std::bad_alloc();
                    beforePts = pts;
                } else if (!selected && pts > targetPts) {
                    const long double afterDistance = std::fabs(static_cast<long double>(pts) - static_cast<long double>(targetPts));
                    const long double beforeDistance = before ? std::fabs(static_cast<long double>(beforePts) - static_cast<long double>(targetPts)) :
                        std::numeric_limits<long double>::infinity();
                    if (before && beforeDistance <= afterDistance) {
                        selected = std::move(before);
                        selectedPts = beforePts;
                        selectedPreviousPts = beforePreviousPts;
                        selectedNextPts = pts;
                        found = true;
                    } else {
                        selected.reset(av_frame_clone(frame.get()));
                        if (!selected) throw std::bad_alloc();
                        selectedPts = pts;
                        selectedPreviousPts = beforePts;
                        awaitingSelectedNext = true;
                    }
                }
            }
            av_frame_unref(frame.get());
            if (found) return 2;
        }
    };

    bool decoderEof = false;
    for (;;) {
        detail::checkStopped(stop);
        const int read = av_read_frame(input.context,packet.get());
        if (read == AVERROR_EOF) break;
        detail::check(read,"read preview video packet");
        if (packet->stream_index == streamIndex) {
            int status = avcodec_send_packet(context.get(),packet.get());
            if (status == AVERROR(EAGAIN)) {
                const int received = receive();
                if (received == 2) { av_packet_unref(packet.get()); break; }
                status = avcodec_send_packet(context.get(),packet.get());
            }
            detail::check(status,"send preview packet to decoder");
            const int received = receive();
            if (received == 2) { av_packet_unref(packet.get()); break; }
            decoderEof = received == 1;
        }
        av_packet_unref(packet.get());
        if (decoderEof) break;
    }
    if (!found && !decoderEof) {
        detail::check(avcodec_send_packet(context.get(),nullptr),"flush preview video decoder");
        for (;;) {
            const int received = receive();
            if (received == 2 || received == 1) break;
        }
    }
    if (!selected && before) {
        selected = std::move(before);
        selectedPts = beforePts;
        selectedPreviousPts = beforePreviousPts;
    }
    if (!selected) throw std::runtime_error("no timestamped decoded frame was found near the requested preview time");

    const int width = selected->width, height = selected->height;
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
        throw std::invalid_argument("preview frame dimensions are outside the supported range");
    const auto pixels = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (pixels > (128ULL * 1024ULL * 1024ULL) / 4ULL)
        throw std::invalid_argument("preview frame exceeds the 128 MiB decoded-image limit");
    auto color = normalizeDecodedSdrColor(detail::color(selected.get()),streamColor);
    try { validateRifeSdrColor(color); }
    catch (const std::invalid_argument&) {
        throw std::invalid_argument("exact frame preview requires explicitly tagged supported SDR color; HDR or unknown color is not yet supported");
    }
    const auto sourceFormat = av_get_pix_fmt(color.pixelFormat.c_str());
    const auto* description = av_pix_fmt_desc_get(sourceFormat);
    if (!description) throw std::invalid_argument("preview pixel format is unavailable");
    const int matrix = color.matrix == "bt709" ? SWS_CS_ITU709 : SWS_CS_ITU601;
    const int* coefficients = sws_getCoefficients(matrix);
    auto freeScaler = +[](SwsContext* value) { sws_freeContext(value); };
    std::unique_ptr<SwsContext,decltype(freeScaler)> scaler(
        sws_getContext(width,height,sourceFormat,width,height,AV_PIX_FMT_RGBA,SWS_BILINEAR | SWS_ACCURATE_RND,nullptr,nullptr,nullptr),freeScaler);
    if (!scaler) throw std::runtime_error("could not create exact SDR preview converter");
    detail::check(sws_setColorspaceDetails(scaler.get(),coefficients,color.range == "pc",coefficients,1,0,1 << 16,1 << 16),
        "configure exact preview color conversion");
    DecodedPreviewFrame output;
    output.width = width;
    output.height = height;
    output.timestamp = detail::rational(stream->time_base) * Rational(selectedPts);
    output.duration = selected->duration > 0 ? detail::timestamp(selected->duration,stream->time_base) : std::optional<Rational>{};
    if (selectedPreviousPts != AV_NOPTS_VALUE)
        output.previousTimestamp = detail::rational(stream->time_base) * Rational(selectedPreviousPts);
    if (selectedNextPts != AV_NOPTS_VALUE)
        output.nextTimestamp = detail::rational(stream->time_base) * Rational(selectedNextPts);
    output.color = std::move(color);
    output.rgba.resize(static_cast<std::size_t>(pixels) * 4U);
    const std::uint8_t* source[4]{};
    int sourceStride[4]{};
    for (std::size_t plane = 0; plane < 4; ++plane) {
        source[plane] = selected->data[plane];
        sourceStride[plane] = selected->linesize[plane];
    }
    std::uint8_t* destination[4] = {output.rgba.data()};
    const int destinationStride[4] = {width * 4};
    if (sws_scale(scaler.get(),source,sourceStride,0,height,destination,destinationStride) != height)
        throw std::runtime_error("exact preview color conversion returned incomplete rows");
    detail::checkStopped(stop);
    return output;
}

} // namespace rifelove::media
