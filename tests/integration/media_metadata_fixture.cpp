extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mem.h>
#include <libavutil/rational.h>
}

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
void check(int status, const char* operation) {
    if (status < 0) {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
        av_strerror(status, text.data(), text.size());
        throw std::runtime_error(std::string(operation) + ": " + text.data());
    }
}

AVPacketSideData* addSideData(AVCodecParameters* parameters, AVPacketSideDataType type, std::size_t size) {
    auto* data = av_packet_side_data_new(&parameters->coded_side_data, &parameters->nb_coded_side_data, type, size, 0);
    if (!data) throw std::bad_alloc();
    return data;
}

void addHdrMetadata(AVCodecParameters* parameters) {
    std::size_t masteringSize = 0;
    std::unique_ptr<AVMasteringDisplayMetadata, decltype(&av_free)> mastering(
        av_mastering_display_metadata_alloc_size(&masteringSize), av_free);
    if (!mastering) throw std::bad_alloc();
    mastering->display_primaries[0][0] = {34000, 50000};
    mastering->display_primaries[0][1] = {16000, 50000};
    mastering->display_primaries[1][0] = {13250, 50000};
    mastering->display_primaries[1][1] = {34500, 50000};
    mastering->display_primaries[2][0] = {7500, 50000};
    mastering->display_primaries[2][1] = {3000, 50000};
    mastering->white_point[0] = {15635, 50000};
    mastering->white_point[1] = {16450, 50000};
    mastering->min_luminance = {1, 200};
    mastering->max_luminance = {1000, 1};
    mastering->has_primaries = 1;
    mastering->has_luminance = 1;
    auto* masteringSideData = addSideData(parameters, AV_PKT_DATA_MASTERING_DISPLAY_METADATA, masteringSize);
    std::memcpy(masteringSideData->data, mastering.get(), masteringSize);

    std::size_t lightSize = 0;
    std::unique_ptr<AVContentLightMetadata, decltype(&av_free)> light(av_content_light_metadata_alloc(&lightSize), av_free);
    if (!light) throw std::bad_alloc();
    light->MaxCLL = 1000;
    light->MaxFALL = 400;
    auto* lightSideData = addSideData(parameters, AV_PKT_DATA_CONTENT_LIGHT_LEVEL, lightSize);
    std::memcpy(lightSideData->data, light.get(), lightSize);

    std::array<std::int32_t, 9> matrix{};
    av_display_rotation_set(matrix.data(), 90.0);
    av_display_matrix_flip(matrix.data(), 1, 0);
    auto* matrixSideData = addSideData(parameters, AV_PKT_DATA_DISPLAYMATRIX, sizeof(matrix));
    std::memcpy(matrixSideData->data, matrix.data(), sizeof(matrix));
}

struct InputContext {
    AVFormatContext* value{};
    ~InputContext() { avformat_close_input(&value); }
};
struct OutputContext {
    AVFormatContext* value{};
    ~OutputContext() {
        if (value) {
            if (value->pb) avio_closep(&value->pb);
            avformat_free_context(value);
        }
    }
};
struct Packet {
    AVPacket* value{av_packet_alloc()};
    ~Packet() { av_packet_free(&value); }
};

void createFixture(const std::string& inputPath, const std::string& outputPath) {
    InputContext input;
    check(avformat_open_input(&input.value, inputPath.c_str(), nullptr, nullptr), "open source fixture");
    check(avformat_find_stream_info(input.value, nullptr), "read source fixture streams");
    const int videoIndex = av_find_best_stream(input.value, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    check(videoIndex, "select source video stream");

    OutputContext output;
    check(avformat_alloc_output_context2(&output.value, nullptr, "matroska", outputPath.c_str()), "create Matroska fixture");
    if (!output.value) throw std::bad_alloc();
    auto* incoming = input.value->streams[videoIndex];
    auto* outgoing = avformat_new_stream(output.value, nullptr);
    if (!outgoing) throw std::bad_alloc();
    check(avcodec_parameters_copy(outgoing->codecpar, incoming->codecpar), "copy source codec parameters");
    outgoing->codecpar->codec_tag = 0;
    outgoing->time_base = incoming->time_base;
    outgoing->avg_frame_rate = incoming->avg_frame_rate;
    outgoing->sample_aspect_ratio = incoming->sample_aspect_ratio;
    addHdrMetadata(outgoing->codecpar);

    if (!(output.value->oformat->flags & AVFMT_NOFILE))
        check(avio_open(&output.value->pb, outputPath.c_str(), AVIO_FLAG_WRITE), "open fixture output");
    check(avformat_write_header(output.value, nullptr), "write fixture header");

    Packet packet;
    if (!packet.value) throw std::bad_alloc();
    for (;;) {
        const int status = av_read_frame(input.value, packet.value);
        if (status == AVERROR_EOF) break;
        check(status, "read source packet");
        if (packet.value->stream_index == videoIndex) {
            packet.value->stream_index = outgoing->index;
            av_packet_rescale_ts(packet.value, incoming->time_base, outgoing->time_base);
            packet.value->pos = -1;
            check(av_interleaved_write_frame(output.value, packet.value), "write fixture packet");
        }
        av_packet_unref(packet.value);
    }
    check(av_write_trailer(output.value), "finish fixture file");
    if (output.value->pb) check(avio_closep(&output.value->pb), "close fixture output");
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::invalid_argument("usage: rifelove-media-metadata-fixture INPUT OUTPUT");
        createFixture(argv[1], argv[2]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
