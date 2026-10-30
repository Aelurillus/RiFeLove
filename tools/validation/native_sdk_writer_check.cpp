// Developer-only exact-pixel writer regression driver; no inference or UI.
#include "media/video_writer.hpp"
extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc != 10) {
        std::cerr << "Usage: native-sdk-writer-check reference output input-layout output-layout input-width input-height output-width output-height encoder\n";
        return 2;
    }
    try {
        using namespace rifelove;
        const auto utf8 = [](const std::filesystem::path& path) {
            const auto value = path.u8string(); return std::string(value.begin(),value.end());
        };
        const std::string inputLayout = utf8(argv[3]);
        if (inputLayout != "yuv420p" && inputLayout != "nv12" && inputLayout != "yuv420p10le" && inputLayout != "p010le")
            throw std::invalid_argument("Unsupported regression input layout");
        const int width = std::stoi(argv[5]), height = std::stoi(argv[6]);
        if (width < 16 || height < 16 || width > 8192 || height > 8192 || width % 2 || height % 2)
            throw std::invalid_argument("Invalid regression frame dimensions");
        const bool ten = inputLayout == "yuv420p10le" || inputLayout == "p010le";
        const bool semiplanar = inputLayout == "nv12" || inputLayout == "p010le";
        const auto format = av_get_pix_fmt(inputLayout.c_str());
        const int size = av_image_get_buffer_size(format,width,height,32);
        if (size < 0) throw std::runtime_error("Unable to size regression frame");
        auto storage = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(size));
        std::uint8_t* planes[4]{}; int strides[4]{};
        if (av_image_fill_arrays(planes,strides,storage->data(),format,width,height,32) < 0)
            throw std::runtime_error("Unable to lay out regression frame");
        const auto put = [&](int plane, int x, int y, int value) {
            auto* pixel = planes[plane] + y * strides[plane] + x * (ten ? 2 : 1);
            const auto sample = static_cast<std::uint16_t>(value * (inputLayout == "p010le" ? 256 : ten ? 4 : 1));
            pixel[0] = static_cast<std::uint8_t>(sample);
            if (ten) pixel[1] = static_cast<std::uint8_t>(sample >> 8);
        };
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) put(0,x,y,90);
        for (int y = 0; y < height / 2; ++y) for (int x = 0; x < width / 2; ++x) {
            const int u = x < width / 4 ? 64 : 192, v = y < height / 4 ? 80 : 176;
            if (semiplanar) { put(1,x * 2,y,u); put(1,x * 2 + 1,y,v); }
            else { put(1,x,y,u); put(2,x,y,v); }
        }
        const auto reference = utf8(argv[1]);
        const auto info = media::MediaEngine::probe(reference);
        const auto video = std::find_if(info.streams.begin(),info.streams.end(),[](const auto& s) { return s.type == "video"; });
        if (video == info.streams.end()) throw std::runtime_error("Reference video missing");
        media::DecodedFrame frame{};
        frame.width = width; frame.height = height; frame.storage = storage; frame.color = video->color;
        frame.color.pixelFormat = inputLayout; frame.color.bitDepth = ten ? 10 : 8;
        for (int i = 0; i < 4; ++i) frame.planes[i] = {planes[i],strides[i]};
        media::VideoEncodingSettings settings;
        settings.encoder = utf8(argv[9]); settings.pixelFormat = utf8(argv[4]);
        settings.outputWidth = std::stoi(argv[7]); settings.outputHeight = std::stoi(argv[8]);
        settings.timeBase = Rational(1,24000); settings.frameRate = Rational(24);
        settings.copyAuxiliaryStreams = false; settings.encoderOptions["threads"] = "2";
        media::VideoMuxWriter writer(reference,utf8(argv[2]),settings);
        writer.writeFrame(frame,Rational(0),Rational(1,24));
        const auto result = writer.finish();
        if (result.framesSubmitted != 1) throw std::runtime_error("Unexpected generated frame count");
        std::cout << inputLayout << " " << width << "x" << height << " -> " << settings.pixelFormat << " "
            << settings.outputWidth << "x" << settings.outputHeight << " via " << settings.encoder << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
