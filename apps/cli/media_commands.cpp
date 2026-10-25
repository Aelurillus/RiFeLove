#include "media_commands.hpp"
#include "common/json.hpp"
#include "media/media_engine.hpp"
#include "media/video_writer.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <iomanip>
#include <iostream>
#include <sstream>

using rifelove::jsonString;
using namespace rifelove::media;

namespace {
std::string number(const std::optional<rifelove::Rational>& value) { return value ? jsonString(value->toString()) : "null"; }
void writeMetadata(std::ostream& out, const Metadata& metadata) {
    out << '{';
    bool first = true;
    for (const auto& [key, value] : metadata) {
        if (!first) out << ',';
        first = false;
        out << jsonString(key) << ':' << jsonString(value);
    }
    out << '}';
}
void writeColor(std::ostream& out, const ColorInfo& color) {
    out << "{\"pixel_format\":" << jsonString(color.pixelFormat) << ",\"bit_depth\":" << color.bitDepth
        << ",\"primaries\":" << jsonString(color.primaries) << ",\"transfer\":" << jsonString(color.transfer)
        << ",\"matrix\":" << jsonString(color.matrix) << ",\"range\":" << jsonString(color.range)
        << ",\"hdr_transfer\":" << (color.hdrTransfer ? "true" : "false") << ",\"mastering_display\":";
    if (color.masteringDisplay) {
        const auto& mastering = *color.masteringDisplay;
        out << "{\"min_luminance\":" << number(mastering.minLuminance) << ",\"max_luminance\":" << number(mastering.maxLuminance) << ",\"primaries\":";
        if (mastering.primaries) {
            out << '[';
            for (std::size_t i = 0; i < 3; ++i) {
                if (i) out << ',';
                out << '[' << jsonString((*mastering.primaries)[i][0].toString()) << ',' << jsonString((*mastering.primaries)[i][1].toString()) << ']';
            }
            out << ']';
        } else out << "null";
        out << ",\"white_point\":";
        if (mastering.whitePoint) out << '[' << jsonString((*mastering.whitePoint)[0].toString()) << ',' << jsonString((*mastering.whitePoint)[1].toString()) << ']';
        else out << "null";
        out << '}';
    } else out << "null";
    out << ",\"content_light\":";
    if (color.contentLight) out << "{\"max_cll\":" << color.contentLight->maxContentLight << ",\"max_fall\":" << color.contentLight->maxFrameAverageLight << '}';
    else out << "null";
    out << '}';
}

void writeProbe(std::ostream& out, const MediaInfo& info) {
    out << "{\"path\":" << jsonString(info.path) << ",\"format\":" << jsonString(info.format)
        << ",\"duration\":" << number(info.duration) << ",\"start\":" << number(info.start) << ",\"bit_rate\":" << info.bitRate << ",\"metadata\":";
    writeMetadata(out, info.metadata);
    out << ",\"streams\":[";
    bool first = true;
    for (const auto& stream : info.streams) {
        if (!first) out << ',';
        first = false;
        out << "{\"index\":" << stream.index << ",\"type\":" << jsonString(stream.type) << ",\"codec\":" << jsonString(stream.codec)
            << ",\"time_base\":" << jsonString(stream.timeBase.toString()) << ",\"start\":" << number(stream.start)
            << ",\"duration\":" << number(stream.duration) << ",\"average_rate\":" << number(stream.averageRate)
            << ",\"nominal_rate\":" << number(stream.nominalRate) << ",\"declared_frames\":" << stream.declaredFrameCount
            << ",\"bit_rate\":" << stream.bitRate << ",\"width\":" << stream.width << ",\"height\":" << stream.height
            << ",\"sample_rate\":" << stream.sampleRate << ",\"channels\":" << stream.channels
            << ",\"sample_format\":" << jsonString(stream.sampleFormat) << ",\"channel_layout\":" << jsonString(stream.channelLayout)
            << ",\"disposition\":" << stream.disposition << ",\"default_track\":" << (stream.defaultTrack ? "true" : "false")
            << ",\"forced_track\":" << (stream.forcedTrack ? "true" : "false")
            << ",\"declared_end\":" << number(stream.declaredEnd) << ",\"initial_padding\":" << stream.initialPadding
            << ",\"rotation_degrees\":";
        if (stream.rotationDegrees) out << std::setprecision(16) << *stream.rotationDegrees;
        else out << "null";
        out << ",\"display_matrix\":";
        if (stream.displayMatrix) {
            out << '[';
            for (std::size_t i = 0; i < stream.displayMatrix->size(); ++i) {
                if (i) out << ',';
                out << (*stream.displayMatrix)[i];
            }
            out << ']';
        } else out << "null";
        out << ",\"color\":"; writeColor(out, stream.color);
        out << ",\"metadata\":"; writeMetadata(out, stream.metadata);
        out << ",\"side_data\":[";
        for (std::size_t i = 0; i < stream.sideData.size(); ++i) {
            if (i) out << ',';
            out << "{\"name\":" << jsonString(stream.sideData[i].name) << ",\"bytes\":" << stream.sideData[i].bytes << '}';
        }
        out << "]}";
    }
    out << "],\"chapters\":[";
    for (std::size_t i = 0; i < info.chapters.size(); ++i) {
        if (i) out << ',';
        const auto& chapter = info.chapters[i];
        out << "{\"id\":" << chapter.id << ",\"start\":" << jsonString(chapter.start.toString()) << ",\"end\":" << jsonString(chapter.end.toString()) << ",\"metadata\":";
        writeMetadata(out, chapter.metadata);
        out << '}';
    }
    out << "]}\n";
}
}

int runMediaCommand(const std::vector<std::string>& args) {
    if (args[1] == "transcode") {
        if (args.size() < 4) throw std::invalid_argument("transcode requires INPUT OUTPUT and optional encoding settings");
        VideoEncodingSettings settings;
        bool progress = false;
        auto parseInteger = [](const std::string& value) {
            std::int64_t number = 0;
            const auto [end, status] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (status != std::errc{} || end != value.data() + value.size()) throw std::invalid_argument("invalid integer encoding setting");
            return number;
        };
        for (std::size_t i = 4; i < args.size(); ++i) {
            const auto& option = args[i];
            if (option == "--progress") { progress = true; continue; }
            if (option == "--allow-precision-loss") { settings.allowPrecisionLoss = true; continue; }
            if (i + 1 >= args.size()) throw std::invalid_argument("missing value for " + option);
            const auto& value = args[++i];
            if (option == "--encoder") settings.encoder = value;
            else if (option == "--pixel-format") settings.pixelFormat = value;
            else if (option == "--bitrate") settings.bitRate = parseInteger(value);
            else if (option == "--bframes") {
                const auto number = parseInteger(value);
                if (number < 0 || number > 16) throw std::invalid_argument("B-frame count must be between 0 and 16");
                settings.maxBFrames = static_cast<int>(number);
            } else if (option == "--option") {
                const auto equal = value.find('=');
                if (equal == std::string::npos || equal == 0) throw std::invalid_argument("encoder options must be KEY=VALUE");
                settings.encoderOptions[value.substr(0, equal)] = value.substr(equal + 1);
            } else throw std::invalid_argument("unknown transcode option: " + option);
        }
        const auto result = transcodeVideo(args[2], args[3], settings, progress ? std::function<void(const EncodeProgress&)>([](const EncodeProgress& current) {
            std::cout << "{\"event\":\"encode_progress\",\"frames_submitted\":" << current.framesSubmitted << ",\"pts\":" << jsonString(current.timestamp.toString())
                << ",\"processing_fps\":" << current.processingFps << "}\n";
        }) : std::function<void(const EncodeProgress&)>());
        std::cout << "{\"event\":\"transcode_completed\",\"encoder\":" << jsonString(result.encoder)
            << ",\"frames_submitted\":" << result.framesSubmitted << ",\"video_packets\":" << result.videoPackets
            << ",\"copied_packets\":" << result.copiedPackets << ",\"first_pts\":" << jsonString(result.firstTimestamp.toString())
            << ",\"end_exclusive\":" << jsonString(result.endExclusive.toString())
            << ",\"elapsed_seconds\":" << result.elapsedSeconds << ",\"processing_fps\":" << result.processingFps << "}\n";
        return 0;
    }
    if (args[1] == "media-probe") {
        if (args.size() != 3) throw std::invalid_argument("media-probe requires INPUT");
        std::ostringstream json;
        writeProbe(json, MediaEngine::probe(args[2]));
        std::cout << json.str();
        return 0;
    }
    if (args[1] == "media-capabilities") {
        if (args.size() < 3) throw std::invalid_argument("media-capabilities requires INPUT and optional --fps RATE");
        std::optional<rifelove::Rational> frameRate;
        for (std::size_t i = 3; i < args.size(); ++i) {
            if (args[i] != "--fps" || i + 1 >= args.size()) throw std::invalid_argument("media-capabilities accepts only --fps RATE");
            frameRate = rifelove::Rational::parse(args[++i]);
            if (*frameRate <= rifelove::Rational(0)) throw std::invalid_argument("probe FPS must be positive");
        }
        const auto info = MediaEngine::probe(args[2]);
        const auto video = std::find_if(info.streams.begin(),info.streams.end(),[&](const StreamInfo& stream) {
            return stream.index == info.preferredVideoStream && stream.type == "video";
        });
        if (video == info.streams.end()) throw std::invalid_argument("input has no video stream to probe");
        constexpr std::array<const char*,4> names{"h264_nvenc","libopenh264","libkvazaar","libaom-av1"};
        std::cout << "{\"event\":\"media_capabilities\",\"ffmpeg_version\":" << jsonString(MediaEngine::runtimeVersion())
            << ",\"width\":" << video->width << ",\"height\":" << video->height << ",\"pixel_format\":" << jsonString(video->color.pixelFormat)
            << ",\"probe_fps\":" << jsonString((frameRate.value_or(video->averageRate.value_or(video->nominalRate.value_or(rifelove::Rational(30))))).toString())
            << ",\"probe_scope\":\"encoder_context_open_only\",\"frames_encoded\":false,\"encoders\":[";
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i) std::cout << ',';
            const auto capability = MediaEngine::probeEncoder(names[i],*video,frameRate);
            std::cout << "{\"name\":" << jsonString(capability.encoder) << ",\"registered\":" << (capability.registered ? "true" : "false")
                << ",\"context_opened\":" << (capability.contextOpened ? "true" : "false")
                << ",\"reason\":" << jsonString(capability.reason) << '}';
        }
        std::cout << "]}\n";
        return 0;
    }
    if (args[1] == "media-scan") {
        if (args.size() < 3) throw std::invalid_argument("media-scan requires INPUT [--frames] [--stream INDEX]");
        bool frames = false;
        int stream = -1;
        DecodeOptions decodeOptions;
        for (std::size_t i = 3; i < args.size(); ++i) {
            if (args[i] == "--frames") frames = true;
            else if (args[i] == "--stream" && i + 1 < args.size()) {
                const auto& value = args[++i];
                const auto [end, status] = std::from_chars(value.data(), value.data() + value.size(), stream);
                if (status != std::errc{} || end != value.data() + value.size() || stream < 0) throw std::invalid_argument("invalid video stream index");
            } else if (args[i] == "--decode" && i + 1 < args.size()) {
                const auto& value = args[++i];
                if (value == "software") decodeOptions.acceleration = DecodeAcceleration::software;
                else if (value == "cuda") decodeOptions.acceleration = DecodeAcceleration::preferCuda;
                else throw std::invalid_argument("--decode must be software or cuda");
            } else throw std::invalid_argument("unknown media-scan option: " + args[i]);
        }
        const auto summary = MediaEngine::decodeVideo(args[2], frames ? std::function<void(const DecodedFrame&)>([](const DecodedFrame& frame) {
            std::cout << "{\"event\":\"decoded_frame\",\"index\":" << frame.index << ",\"pts\":" << number(frame.timestamp)
                << ",\"duration\":" << number(frame.duration) << ",\"pts_recovered\":" << (frame.timestampRecovered ? "true" : "false")
                << ",\"width\":" << frame.width << ",\"height\":" << frame.height << ",\"color\":";
            writeColor(std::cout, frame.color);
            std::cout << "}\n";
        }) : std::function<void(const DecodedFrame&)>(), stream,{},decodeOptions);
        std::cout << "{\"event\":\"decode_summary\",\"frames\":" << summary.frameCount << ",\"cadence\":" << jsonString(summary.cadence)
            << ",\"missing_timestamps\":" << summary.missingTimestamps << ",\"recovered_timestamps\":" << summary.recoveredTimestamps
            << ",\"non_increasing_timestamps\":" << summary.nonIncreasingTimestamps
            << ",\"decoder\":" << jsonString(summary.decoder) << ",\"acceleration\":" << jsonString(summary.acceleration)
            << ",\"acceleration_fallback_reason\":" << jsonString(summary.accelerationFallbackReason)
            << ",\"first_pts\":" << number(summary.firstTimestamp) << ",\"last_pts\":" << number(summary.lastTimestamp) << "}\n";
        return 0;
    }
    if (args[1] == "remux") {
        if (args.size() != 4) throw std::invalid_argument("remux requires INPUT OUTPUT");
        const auto result = MediaEngine::remux(args[2], args[3]);
        std::cout << "{\"event\":\"remux_completed\",\"output\":" << jsonString(args[3]) << ",\"copied_streams\":" << result.copiedStreams
            << ",\"packets_written\":" << result.packetsWritten << "}\n";
        return 0;
    }
    throw std::invalid_argument("unknown media command");
}
