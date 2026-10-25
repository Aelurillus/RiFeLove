#include "media/video_writer.hpp"
#include "media/output_helpers.hpp"

extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace rifelove::media {
namespace {
struct CodecDeleter { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct PacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct FrameDeleter { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct ScaleDeleter { void operator()(SwsContext* p) const { sws_freeContext(p); } };
struct Dictionary {
    AVDictionary* value{};
    ~Dictionary() { av_dict_free(&value); }
};
AVRational avRational(Rational value) {
    if (value.numerator() <= 0 || value.numerator() > std::numeric_limits<int>::max() || value.denominator() > std::numeric_limits<int>::max())
        throw std::invalid_argument("encoding time base and frame rate must be positive 32-bit rationals");
    return {static_cast<int>(value.numerator()), static_cast<int>(value.denominator())};
}
AVRational avMetadataRational(Rational value) {
    if (value.numerator() < std::numeric_limits<int>::min() || value.numerator() > std::numeric_limits<int>::max() || value.denominator() > std::numeric_limits<int>::max())
        throw std::invalid_argument("color metadata rational is out of range");
    return {static_cast<int>(value.numerator()), static_cast<int>(value.denominator())};
}
std::int64_t ticks(Rational timestamp, AVRational timeBase) {
    const auto scaled = timestamp / detail::rational(timeBase);
    if (scaled.denominator() != 1) throw std::invalid_argument("frame timestamp/duration is not exactly representable in the encoder time base");
    return scaled.numerator();
}
bool sameColor(const ColorInfo& a, const ColorInfo& b) {
    return a.primaries == b.primaries && a.transfer == b.transfer && a.matrix == b.matrix && a.range == b.range;
}
void addFrameHdr(AVFrame* frame, const ColorInfo& color) {
    if (color.masteringDisplay) {
        auto* data = av_mastering_display_metadata_create_side_data(frame);
        if (!data) throw std::bad_alloc();
        const auto& source = *color.masteringDisplay;
        if (source.primaries && source.whitePoint) {
            for (std::size_t i = 0; i < 3; ++i)
                for (std::size_t j = 0; j < 2; ++j) data->display_primaries[i][j] = avMetadataRational((*source.primaries)[i][j]);
            for (std::size_t i = 0; i < 2; ++i) data->white_point[i] = avMetadataRational((*source.whitePoint)[i]);
            data->has_primaries = 1;
        }
        if (source.minLuminance && source.maxLuminance) {
            data->min_luminance = avMetadataRational(*source.minLuminance);
            data->max_luminance = avMetadataRational(*source.maxLuminance);
            data->has_luminance = 1;
        }
    }
    if (color.contentLight) {
        auto* data = av_content_light_metadata_create_side_data(frame);
        if (!data) throw std::bad_alloc();
        data->MaxCLL = color.contentLight->maxContentLight;
        data->MaxFALL = color.contentLight->maxFrameAverageLight;
    }
}
}

class VideoMuxWriter::Impl final {
public:
    Impl(const std::string& source, const std::string& destination, const VideoEncodingSettings& settings, std::stop_token stop)
        : stop_(stop), input_(source, stop), temporary_(detail::pathFromUtf8(destination)), settings_(settings),
          packet_(av_packet_alloc()), auxiliary_(av_packet_alloc()) {
        const auto extension = detail::outputExtension(detail::pathFromUtf8(destination));
        if (!packet_ || !auxiliary_) throw std::bad_alloc();
        selected_ = settings.videoStreamIndex;
        if (selected_ < 0) selected_ = av_find_best_stream(input_.context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        detail::check(selected_, "select video stream to encode");
        if (static_cast<unsigned>(selected_) >= input_.context->nb_streams || input_.context->streams[selected_]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            throw std::invalid_argument("selected encoding stream is not video");
        auto* sourceStream = input_.context->streams[selected_];
        auto* parameters = sourceStream->codecpar;
        referenceColor_ = detail::color(parameters);
        const auto* encoder = avcodec_find_encoder_by_name(settings.encoder.c_str());
        if (!encoder || encoder->type != AVMEDIA_TYPE_VIDEO) throw std::invalid_argument("video encoder is not registered: " + settings.encoder);
        codec_.reset(avcodec_alloc_context3(encoder));
        if (!codec_) throw std::bad_alloc();
        codec_->width = parameters->width;
        codec_->height = parameters->height;
        if (codec_->width <= 0 || codec_->height <= 0) throw std::invalid_argument("input video dimensions are unknown");
        codec_->pix_fmt = settings.pixelFormat.empty() ? static_cast<AVPixelFormat>(parameters->format) : av_get_pix_fmt(settings.pixelFormat.c_str());
        if (!av_pix_fmt_desc_get(codec_->pix_fmt)) throw std::invalid_argument("output pixel format is unknown");
        const void* supported = nullptr;
        int supportedCount = 0;
        detail::check(avcodec_get_supported_config(codec_.get(), encoder, AV_CODEC_CONFIG_PIX_FORMAT, 0, &supported, &supportedCount), "query encoder pixel formats");
        if (supported && supportedCount > 0) {
            const auto* formats = static_cast<const AVPixelFormat*>(supported);
            if (std::find(formats, formats + supportedCount, codec_->pix_fmt) == formats + supportedCount)
                throw std::invalid_argument("encoder does not support the requested pixel format; choose an explicit compatible format");
        }
        const int outputDepth = av_pix_fmt_desc_get(codec_->pix_fmt)->comp[0].depth;
        if (outputDepth < referenceColor_.bitDepth && (!settings.allowPrecisionLoss || referenceColor_.hdrTransfer))
            throw std::invalid_argument("pixel bit-depth reduction is disabled; HDR reduction requires a validated color pipeline");
        codec_->time_base = settings.timeBase ? avRational(*settings.timeBase) : sourceStream->time_base;
        if (codec_->time_base.num <= 0 || codec_->time_base.den <= 0) throw std::invalid_argument("encoder time base is unknown");
        const auto sourceRate = sourceStream->avg_frame_rate.num > 0 ? sourceStream->avg_frame_rate : sourceStream->r_frame_rate;
        codec_->framerate = settings.frameRate ? avRational(*settings.frameRate) : sourceRate;
        if (codec_->framerate.num <= 0 || codec_->framerate.den <= 0) throw std::invalid_argument("encoder needs a nominal frame rate");
        if (settings.bitRate < 0 || settings.maxBFrames < 0 || settings.maxBFrames > 16 || settings.gopSize < 0)
            throw std::invalid_argument("invalid encoding bitrate, GOP or B-frame setting");
        codec_->bit_rate = settings.bitRate;
        codec_->gop_size = settings.gopSize;
        codec_->max_b_frames = settings.maxBFrames;
        codec_->sample_aspect_ratio = sourceStream->sample_aspect_ratio;
        codec_->color_primaries = parameters->color_primaries;
        codec_->color_trc = parameters->color_trc;
        codec_->colorspace = parameters->color_space;
        codec_->color_range = parameters->color_range;
        codec_->chroma_sample_location = parameters->chroma_location;
        codec_->flags |= AV_CODEC_FLAG_FRAME_DURATION;
        detail::check(avformat_alloc_output_context2(&output_.value, nullptr, nullptr, destination.c_str()), "select encoding container");
        if (!output_.value) throw std::bad_alloc();
        output_.value->interrupt_callback = input_.context->interrupt_callback;
        output_.value->max_interleave_delta = AV_TIME_BASE;
        if (output_.value->oformat->flags & AVFMT_GLOBALHEADER) codec_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        Dictionary options;
        for (const auto& [key, value] : settings.encoderOptions) detail::check(av_dict_set(&options.value, key.c_str(), value.c_str(), 0), "configure encoder option");
        detail::checkStopped(stop_);
        detail::check(avcodec_open2(codec_.get(), encoder, &options.value), "open video encoder " + settings.encoder);
        if (const auto* ignored = av_dict_get(options.value, "", nullptr, AV_DICT_IGNORE_SUFFIX))
            throw std::invalid_argument("unsupported encoder option: " + std::string(ignored->key));
        if (settings_.copyAuxiliaryStreams) {
            detail::check(av_dict_copy(&output_.value->metadata, input_.context->metadata, 0), "copy encoding container metadata");
            detail::copyChapters(input_.context, output_.value);
        }
        for (unsigned i = 0; i < input_.context->nb_streams; ++i) {
            if (!settings_.copyAuxiliaryStreams && static_cast<int>(i) != selected_) continue;
            const auto* incoming = input_.context->streams[i];
            auto* outgoing = avformat_new_stream(output_.value, nullptr);
            if (!outgoing) throw std::bad_alloc();
            const bool replaced = static_cast<int>(i) == selected_;
            const auto codecId = replaced ? encoder->id : incoming->codecpar->codec_id;
            if (avformat_query_codec(output_.value->oformat, codecId, FF_COMPLIANCE_NORMAL) == 0)
                throw std::runtime_error("output container does not support stream " + std::to_string(i) + " (" + avcodec_get_name(codecId) + ")");
            if (replaced) {
                outputVideoIndex_ = outgoing->index;
                detail::check(avcodec_parameters_from_context(outgoing->codecpar, codec_.get()), "copy encoded video parameters");
                outgoing->time_base = codec_->time_base;
                outgoing->avg_frame_rate = codec_->framerate;
                for (int j = 0; j < incoming->codecpar->nb_coded_side_data; ++j) {
                    const auto& side = incoming->codecpar->coded_side_data[j];
                    if (side.type == AV_PKT_DATA_DISPLAYMATRIX || side.type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA || side.type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL) {
                        auto* copied = av_packet_side_data_new(&outgoing->codecpar->coded_side_data, &outgoing->codecpar->nb_coded_side_data, side.type, side.size, 0);
                        if (!copied) throw std::bad_alloc();
                        std::memcpy(copied->data, side.data, side.size);
                    }
                }
            } else {
                detail::check(avcodec_parameters_copy(outgoing->codecpar, incoming->codecpar), "copy auxiliary stream parameters");
                outgoing->time_base = incoming->time_base;
                outgoing->avg_frame_rate = incoming->avg_frame_rate;
            }
            outgoing->codecpar->codec_tag = 0;
            outgoing->sample_aspect_ratio = incoming->sample_aspect_ratio;
            outgoing->disposition = incoming->disposition;
            detail::copyStreamMetadata(&outgoing->metadata, incoming->metadata, extension);
            if (replaced) detail::check(av_dict_set(&outgoing->metadata,"encoder",("RifeLove / " + settings_.encoder).c_str(),0),"record selected video encoder");
        }
        if (settings_.copyAuxiliaryStreams && extension == ".mkv") {
            const auto padding = detail::initialAudioPadding(source,stop_);
            if (padding.size() != output_.value->nb_streams) throw std::runtime_error("Input stream layout changed while inspecting audio priming");
            for (unsigned i = 0; i < output_.value->nb_streams; ++i)
                if (output_.value->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                    output_.value->streams[i]->codecpar->initial_padding = padding[i];
        }
        detail::writeOutputHeader(output_.value, detail::pathToUtf8(temporary_.path), extension);
    }

    Rational outputTimeBase() const {
        if (!output_.value || outputVideoIndex_ < 0 || static_cast<unsigned>(outputVideoIndex_) >= output_.value->nb_streams)
            throw std::logic_error("Video muxer has no initialized output stream");
        return detail::rational(output_.value->streams[outputVideoIndex_]->time_base);
    }

    void write(const DecodedFrame& input, Rational timestamp, Rational duration) {
        detail::checkStopped(stop_);
        if (!input.storage || input.width != codec_->width || input.height != codec_->height) throw std::invalid_argument("generated frame storage or dimensions are invalid");
        if (!sameColor(input.color, referenceColor_)) {
            const auto describe = [](const ColorInfo& color) {
                return color.primaries + "/" + color.transfer + "/" + color.matrix + "/" + color.range;
            };
            throw std::invalid_argument("frame color properties differ from the output color contract (frame=" +
                describe(input.color) + ", output=" + describe(referenceColor_) + ")");
        }
        const auto pts = ticks(timestamp, codec_->time_base);
        const auto length = ticks(duration, codec_->time_base);
        if (pts == AV_NOPTS_VALUE) throw std::invalid_argument("frame timestamp uses the reserved unknown-timestamp value");
        if (length <= 0 || (end_ && timestamp < *end_)) throw std::invalid_argument("frame times overlap or have non-positive duration");
        if (durationByPts_.size() >= 1024) throw std::runtime_error("encoder lookahead exceeded the bounded frame queue");
        const auto sourceFormat = av_get_pix_fmt(input.color.pixelFormat.c_str());
        const auto* sourceDescription = av_pix_fmt_desc_get(sourceFormat);
        const auto* outputDescription = av_pix_fmt_desc_get(codec_->pix_fmt);
        if (!sourceDescription) throw std::invalid_argument("generated frame pixel format is unknown");
        if ((sourceDescription->flags & AV_PIX_FMT_FLAG_RGB) != (outputDescription->flags & AV_PIX_FMT_FLAG_RGB))
            throw std::invalid_argument("RGB/YUV conversion needs an explicit color-managed preprocessing stage");
        if (outputDescription->comp[0].depth < sourceDescription->comp[0].depth && (!settings_.allowPrecisionLoss || input.color.hdrTransfer))
            throw std::invalid_argument("generated frame bit-depth reduction is disabled");
        const int planes = av_pix_fmt_count_planes(sourceFormat);
        if (planes < 1 || planes > 4) throw std::invalid_argument("unsupported generated-frame plane layout");
        std::array<const std::uint8_t*, 4> sourceData{};
        std::array<int, 4> sourceStrides{};
        for (int i = 0; i < planes; ++i) {
            const auto& plane = input.planes[static_cast<std::size_t>(i)];
            const auto stride = static_cast<std::int64_t>(plane.stride);
            const auto magnitude = stride < 0 ? -stride : stride;
            const int minimum = av_image_get_linesize(sourceFormat, input.width, i);
            if (!plane.data || minimum < 0 || magnitude < minimum) throw std::invalid_argument("generated frame has an invalid plane or stride");
            sourceData[static_cast<std::size_t>(i)] = plane.data;
            sourceStrides[static_cast<std::size_t>(i)] = plane.stride;
        }
        std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
        if (!frame) throw std::bad_alloc();
        frame->format = codec_->pix_fmt;
        frame->width = codec_->width;
        frame->height = codec_->height;
        frame->pts = pts;
        frame->duration = length;
        if (settings_.requireFirstPacketKeyframe && frames_ == 0) frame->pict_type = AV_PICTURE_TYPE_I;
        frame->time_base = codec_->time_base;
        frame->color_primaries = codec_->color_primaries;
        frame->color_trc = codec_->color_trc;
        frame->colorspace = codec_->colorspace;
        frame->color_range = codec_->color_range;
        frame->chroma_location = codec_->chroma_sample_location;
        frame->sample_aspect_ratio = codec_->sample_aspect_ratio;
        detail::check(av_frame_get_buffer(frame.get(), 32), "allocate encoding frame");
        if (sourceFormat == codec_->pix_fmt) av_image_copy(frame->data, frame->linesize, sourceData.data(), sourceStrides.data(), sourceFormat, input.width, input.height);
        else {
            scale_.reset(sws_getCachedContext(scale_.release(), input.width, input.height, sourceFormat,
                input.width, input.height, codec_->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr));
            if (!scale_) throw std::runtime_error("pixel layout conversion is unsupported");
            if (!(sourceDescription->flags & AV_PIX_FMT_FLAG_RGB)) {
                int matrix = static_cast<int>(codec_->colorspace);
                if (matrix == AVCOL_SPC_UNSPECIFIED) {
                    if (sourceDescription->log2_chroma_w != outputDescription->log2_chroma_w || sourceDescription->log2_chroma_h != outputDescription->log2_chroma_h)
                        throw std::invalid_argument("chroma resampling requires known color matrix metadata");
                    matrix = SWS_CS_DEFAULT;
                }
                if (matrix == AVCOL_SPC_BT2020_CL) throw std::invalid_argument("constant-luminance BT.2020 conversion is not implemented");
                const int fullRange = codec_->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
                const auto* coefficients = sws_getCoefficients(matrix);
                detail::check(sws_setColorspaceDetails(scale_.get(), coefficients, fullRange, coefficients, fullRange, 0, 1 << 16, 1 << 16), "configure pixel layout color preservation");
            }
            const int rows = sws_scale(scale_.get(), sourceData.data(), sourceStrides.data(), 0, input.height, frame->data, frame->linesize);
            if (rows != input.height) throw std::runtime_error("pixel layout conversion returned an incomplete frame");
        }
        addFrameHdr(frame.get(), input.color);
        durationByPts_.emplace(pts, length);
        int sent = avcodec_send_frame(codec_.get(), frame.get());
        if (sent == AVERROR(EAGAIN)) { receive(); sent = avcodec_send_frame(codec_.get(), frame.get()); }
        detail::check(sent, "submit generated frame to encoder");
        if (!first_) first_ = timestamp;
        end_ = timestamp + duration;
        ++frames_;
        receive();
    }

    VideoWriteResult finish(const std::function<void(const std::string&)>& validate) {
        detail::checkStopped(stop_);
        if (frames_ == 0) throw std::logic_error("cannot finalize a video with no frames");
        detail::check(avcodec_send_frame(codec_.get(), nullptr), "flush video encoder");
        if (receive() != AVERROR_EOF) throw std::runtime_error("video encoder did not finish draining");
        if (!durationByPts_.empty()) throw std::runtime_error("video encoder did not return all submitted frame timestamps");
        copyAuxiliary(std::nullopt);
        detail::check(av_write_trailer(output_.value), "finalize encoded output");
        detail::check(avio_closep(&output_.value->pb), "close encoded output");
        detail::checkStopped(stop_);
        const auto verified = MediaEngine::probe(detail::pathToUtf8(temporary_.path), stop_);
        const auto video = std::find_if(verified.streams.begin(), verified.streams.end(), [&](const StreamInfo& stream) { return stream.index == outputVideoIndex_ && stream.type == "video"; });
        if (video == verified.streams.end() || video->color.bitDepth != av_pix_fmt_desc_get(codec_->pix_fmt)->comp[0].depth)
            throw std::runtime_error("encoded output validation found changed video bit depth");
        const auto matchesKnown = [](const std::string& wanted, const std::string& actual) { return wanted.empty() || wanted == "unknown" || wanted == "unspecified" || wanted == actual; };
        if (!matchesKnown(referenceColor_.primaries, video->color.primaries) || !matchesKnown(referenceColor_.transfer, video->color.transfer) ||
            !matchesKnown(referenceColor_.matrix, video->color.matrix) || !matchesKnown(referenceColor_.range, video->color.range))
            throw std::runtime_error("encoded output validation found changed color metadata");
        if (settings_.copyAuxiliaryStreams) {
            std::map<std::pair<std::string, std::string>, unsigned> counts;
            for (const auto& stream : verified.streams) ++counts[{stream.type, stream.codec}];
            for (unsigned i = 0; i < input_.context->nb_streams; ++i) {
                const auto* parameters = input_.context->streams[i]->codecpar;
                const auto codec = static_cast<int>(i) == selected_ ? codec_->codec_id : parameters->codec_id;
                const auto* kind = av_get_media_type_string(parameters->codec_type);
                auto& count = counts[{kind ? kind : "unknown", avcodec_get_name(codec)}];
                if (count == 0) throw std::runtime_error("encoded output validation found a missing stream");
                --count;
            }
            if (verified.chapters.size() < input_.context->nb_chapters) throw std::runtime_error("encoded output lost chapter metadata");
        } else if (verified.streams.size() != 1 || verified.streams.front().type != "video") {
            throw std::runtime_error("video-only segment unexpectedly contains auxiliary streams");
        }
        if (validate) validate(detail::pathToUtf8(temporary_.path));
        detail::checkStopped(stop_);
        temporary_.publish();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        return {settings_.encoder, frames_, videoPackets_, copiedPackets_, *first_, *end_, seconds,
            seconds > 0 ? static_cast<double>(frames_) / seconds : 0.0};
    }

private:
    void copyAuxiliary(std::optional<Rational> boundary) {
        if (!settings_.copyAuxiliaryStreams) return;
        while (!auxiliaryEof_) {
            detail::checkStopped(stop_);
            if (!haveAuxiliary_) {
                const int read = av_read_frame(input_.context, auxiliary_.get());
                if (read == AVERROR_EOF) { auxiliaryEof_ = true; break; }
                detail::check(read, "read auxiliary input packet");
                if (auxiliary_->stream_index < 0 || static_cast<unsigned>(auxiliary_->stream_index) >= output_.value->nb_streams)
                    throw std::runtime_error("Input stream layout changed during processing");
                haveAuxiliary_ = true;
            }
            const auto* incoming = input_.context->streams[auxiliary_->stream_index];
            if (auxiliary_->stream_index == selected_) {
                const auto time = detail::timestamp(auxiliary_->dts != AV_NOPTS_VALUE ? auxiliary_->dts : auxiliary_->pts, incoming->time_base);
                if (boundary && time && *time >= *boundary) return;
            } else {
                const auto* outgoing = output_.value->streams[auxiliary_->stream_index];
                detail::normalizeCopiedAudioDuration(auxiliary_.get(),incoming,detail::declaredEnd(input_.context,incoming));
                av_packet_rescale_ts(auxiliary_.get(), incoming->time_base, outgoing->time_base);
                auxiliary_->pos = -1;
                detail::check(av_interleaved_write_frame(output_.value, auxiliary_.get()), "copy auxiliary compressed packet");
                ++copiedPackets_;
            }
            av_packet_unref(auxiliary_.get());
            haveAuxiliary_ = false;
        }
    }
    int receive() {
        for (;;) {
            detail::checkStopped(stop_);
            const int status = avcodec_receive_packet(codec_.get(), packet_.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return status;
            detail::check(status, "receive encoded video packet");
            if (settings_.requireFirstPacketKeyframe && videoPackets_ == 0 && !(packet_->flags & AV_PKT_FLAG_KEY))
                throw std::runtime_error("segment encoder did not produce an independently decodable first packet");
            const auto duration = durationByPts_.find(packet_->pts);
            if (duration == durationByPts_.end()) throw std::runtime_error("encoder returned an unrecognized presentation timestamp");
            packet_->duration = duration->second;
            durationByPts_.erase(duration);
            copyAuxiliary(detail::timestamp(packet_->dts != AV_NOPTS_VALUE ? packet_->dts : packet_->pts, codec_->time_base));
            av_packet_rescale_ts(packet_.get(), codec_->time_base, output_.value->streams[outputVideoIndex_]->time_base);
            packet_->stream_index = outputVideoIndex_;
            packet_->pos = -1;
            detail::check(av_interleaved_write_frame(output_.value, packet_.get()), "mux encoded video packet");
            ++videoPackets_;
            av_packet_unref(packet_.get());
        }
    }
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::stop_token stop_;
    detail::Input input_;
    detail::TemporaryOutput temporary_;
    detail::OutputContext output_;
    VideoEncodingSettings settings_;
    ColorInfo referenceColor_;
    std::unique_ptr<AVCodecContext, CodecDeleter> codec_;
    std::unique_ptr<AVPacket, PacketDeleter> packet_;
    std::unique_ptr<AVPacket, PacketDeleter> auxiliary_;
    std::unique_ptr<SwsContext, ScaleDeleter> scale_;
    std::map<std::int64_t, std::int64_t> durationByPts_;
    int selected_ = -1;
    int outputVideoIndex_ = 0;
    bool haveAuxiliary_ = false;
    bool auxiliaryEof_ = false;
    std::size_t frames_ = 0;
    std::size_t videoPackets_ = 0;
    std::size_t copiedPackets_ = 0;
    std::optional<Rational> first_;
    std::optional<Rational> end_;
};

VideoMuxWriter::VideoMuxWriter(const std::string& source, const std::string& output, const VideoEncodingSettings& settings, std::stop_token stop)
    : impl_(std::make_unique<Impl>(source, output, settings, stop)) {}
VideoMuxWriter::~VideoMuxWriter() = default;
Rational VideoMuxWriter::outputTimeBase() const {
    if (!impl_) throw std::logic_error("video writer is closed");
    return impl_->outputTimeBase();
}
void VideoMuxWriter::writeFrame(const DecodedFrame& frame, Rational timestamp, Rational duration) {
    if (!impl_) throw std::logic_error("video writer is closed");
    try { impl_->write(frame, timestamp, duration); }
    catch (...) { impl_.reset(); throw; }
}
VideoWriteResult VideoMuxWriter::finish(const std::function<void(const std::string&)>& validate) {
    if (!impl_) throw std::logic_error("video writer is closed");
    try { auto result = impl_->finish(validate); impl_.reset(); return result; }
    catch (...) { impl_.reset(); throw; }
}

VideoWriteResult transcodeVideo(const std::string& source, const std::string& output, const VideoEncodingSettings& settings,
    const std::function<void(const EncodeProgress&)>& progress, std::stop_token stop) {
    const auto started = std::chrono::steady_clock::now();
    VideoMuxWriter writer(source, output, settings, stop);
    std::optional<DecodedFrame> pending;
    std::size_t count = 0;
    auto submit = [&](const DecodedFrame& frame, Rational duration) {
        if (!frame.timestamp) throw std::runtime_error("transcoding requires a known presentation timestamp for every frame");
        writer.writeFrame(frame, *frame.timestamp, duration);
        ++count;
        if (progress) {
            const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            progress({count, *frame.timestamp, seconds > 0 ? static_cast<double>(count) / seconds : 0.0});
        }
    };
    (void)MediaEngine::decodeVideo(source, [&](const DecodedFrame& frame) {
        if (!frame.timestamp) throw std::runtime_error("transcoding requires a known presentation timestamp for every frame");
        if (pending) submit(*pending, *frame.timestamp - *pending->timestamp);
        pending = frame;
    }, settings.videoStreamIndex, stop);
    if (!pending || !pending->duration || *pending->duration <= Rational(0))
        throw std::runtime_error("the final frame has no reliable duration; an explicit timeline boundary is required");
    submit(*pending, *pending->duration);
    return writer.finish();
}
} // namespace rifelove::media
