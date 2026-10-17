#include "media_commands.hpp"
#include "common/json.hpp"
#include "media/media_engine.hpp"

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
            << ",\"sample_rate\":" << stream.sampleRate << ",\"channels\":" << stream.channels << ",\"disposition\":" << stream.disposition
            << ",\"rotation_degrees\":";
        if (stream.rotationDegrees) out << std::setprecision(16) << *stream.rotationDegrees;
        else out << "null";
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
    if (args[1] == "media-probe") {
        if (args.size() != 3) throw std::invalid_argument("media-probe requires INPUT");
        std::ostringstream json;
        writeProbe(json, MediaEngine::probe(args[2]));
        std::cout << json.str();
        return 0;
    }
    if (args[1] == "media-scan") {
        if (args.size() < 3) throw std::invalid_argument("media-scan requires INPUT [--frames] [--stream INDEX]");
        bool frames = false;
        int stream = -1;
        for (std::size_t i = 3; i < args.size(); ++i) {
            if (args[i] == "--frames") frames = true;
            else if (args[i] == "--stream" && i + 1 < args.size()) {
                const auto& value = args[++i];
                const auto [end, status] = std::from_chars(value.data(), value.data() + value.size(), stream);
                if (status != std::errc{} || end != value.data() + value.size() || stream < 0) throw std::invalid_argument("invalid video stream index");
            } else throw std::invalid_argument("unknown media-scan option: " + args[i]);
        }
        const auto summary = MediaEngine::decodeVideo(args[2], frames ? std::function<void(const DecodedFrame&)>([](const DecodedFrame& frame) {
            std::cout << "{\"event\":\"decoded_frame\",\"index\":" << frame.index << ",\"pts\":" << number(frame.timestamp)
                << ",\"duration\":" << number(frame.duration) << ",\"width\":" << frame.width << ",\"height\":" << frame.height << ",\"color\":";
            writeColor(std::cout, frame.color);
            std::cout << "}\n";
        }) : std::function<void(const DecodedFrame&)>(), stream);
        std::cout << "{\"event\":\"decode_summary\",\"frames\":" << summary.frameCount << ",\"cadence\":" << jsonString(summary.cadence)
            << ",\"missing_timestamps\":" << summary.missingTimestamps << ",\"non_increasing_timestamps\":" << summary.nonIncreasingTimestamps
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
