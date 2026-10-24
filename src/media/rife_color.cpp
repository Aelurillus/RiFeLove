#include "media/rife_color.hpp"
#include "media/ffmpeg_helpers.hpp"
extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <cmath>
#include <cstring>

namespace rifelove::media {
namespace {
struct ScaleDeleter { void operator()(SwsContext* value) const { sws_freeContext(value); } };
struct FrameDeleter { void operator()(AVFrame* value) const { av_frame_free(&value); } };
AVPixelFormat checkColor(const ColorInfo& color) {
    const auto format = av_get_pix_fmt(color.pixelFormat.c_str());
    const auto* descriptor = av_pix_fmt_desc_get(format);
    if (!descriptor || color.bitDepth != 8 || descriptor->comp[0].depth != 8 || color.hdrTransfer || color.masteringDisplay || color.contentLight ||
        color.primaries != "bt709" || (color.transfer != "bt709" && color.transfer != "iec61966-2-1"))
        throw std::invalid_argument("RIFE SDR path requires explicit 8-bit BT.709 primaries and SDR transfer; HDR/unknown color needs a separately validated path");
    if (descriptor->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_ALPHA | AV_PIX_FMT_FLAG_FLOAT))
        throw std::invalid_argument("RIFE SDR path does not accept hardware, palette, alpha or floating source pixels");
    const bool rgb = (descriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    if ((rgb && (color.matrix != "gbr" || color.range != "pc")) || (!rgb && color.matrix != "bt709") || (color.range != "tv" && color.range != "pc"))
        throw std::invalid_argument("RIFE SDR path requires explicit matching RGB/YUV matrix and range");
    return format;
}
void dimensions(int width, int height) {
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) throw std::invalid_argument("Invalid RIFE SDR dimensions");
}
std::unique_ptr<SwsContext, ScaleDeleter> converter(int width, int height, AVPixelFormat input, AVPixelFormat output, int sourceRange, int outputRange) {
    std::unique_ptr<SwsContext, ScaleDeleter> context(sws_getContext(width,height,input,width,height,output,SWS_BILINEAR | SWS_ACCURATE_RND,nullptr,nullptr,nullptr));
    if (!context) throw std::runtime_error("Could not create explicit SDR pixel converter");
    const int* coefficients = sws_getCoefficients(SWS_CS_ITU709);
    detail::check(sws_setColorspaceDetails(context.get(),coefficients,sourceRange,coefficients,outputRange,0,1 << 16,1 << 16), "configure BT.709 SDR matrix/range");
    return context;
}
}
void validateRifeSdrColor(const ColorInfo& color) { (void)checkColor(color); }
RifeSdrFrame prepareRifeSdr(const DecodedFrame& frame, std::size_t floatBufferLimit, std::stop_token stop) {
    detail::checkStopped(stop);
    dimensions(frame.width, frame.height);
    const auto inputFormat = checkColor(frame.color);
    if (!frame.storage) throw std::invalid_argument("RIFE preprocessing requires owned source planes");
    for (int plane = 0; plane < 4; ++plane) {
        const int lineBytes = av_image_get_linesize(inputFormat,frame.width,plane);
        if (lineBytes > 0 && (!frame.planes[plane].data || std::abs(static_cast<std::int64_t>(frame.planes[plane].stride)) < lineBytes))
            throw std::invalid_argument("RIFE source plane is missing or its stride is too small");
    }
    const int paddedWidth = ((frame.width + 63) / 64) * 64, paddedHeight = ((frame.height + 63) / 64) * 64;
    const auto planeSize = static_cast<std::size_t>(paddedWidth) * static_cast<std::size_t>(paddedHeight);
    if (planeSize * 3 * sizeof(float) > floatBufferLimit) throw std::invalid_argument("RIFE host float buffer exceeds its configured limit");
    RifeSdrFrame output{frame.width,frame.height,paddedWidth,paddedHeight,frame.color,std::vector<float>(planeSize * 3)};
    std::vector<std::uint16_t> rgb(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 3);
    auto context = converter(frame.width,frame.height,inputFormat,AV_PIX_FMT_RGB48LE,frame.color.range == "pc",1);
    const std::uint8_t* source[4]{}; int strides[4]{};
    for (int plane = 0; plane < 4; ++plane) { source[plane] = frame.planes[plane].data; strides[plane] = frame.planes[plane].stride; }
    std::uint8_t* destination[4] = {reinterpret_cast<std::uint8_t*>(rgb.data())}; const int destinationStride[4] = {frame.width * 6};
    if (sws_scale(context.get(),source,strides,0,frame.height,destination,destinationStride) != frame.height) throw std::runtime_error("Incomplete RIFE SDR preprocessing");
    for (int y = 0; y < paddedHeight; ++y) {
        detail::checkStopped(stop);
        for (int x = 0; x < paddedWidth; ++x) {
            const auto input = (static_cast<std::size_t>(std::min(y,frame.height - 1)) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(std::min(x,frame.width - 1))) * 3;
            const auto pixel = static_cast<std::size_t>(y) * static_cast<std::size_t>(paddedWidth) + static_cast<std::size_t>(x);
            for (std::size_t channel = 0; channel < 3; ++channel) output.pixels[channel * planeSize + pixel] = static_cast<float>(rgb[input + channel]) / 65535.0F;
        }
    }
    return output;
}
DecodedFrame restoreRifeSdr(const RifeSdrFrame& frame, std::stop_token stop) {
    detail::checkStopped(stop);
    dimensions(frame.width,frame.height); dimensions(frame.paddedWidth,frame.paddedHeight);
    if (frame.paddedWidth < frame.width || frame.paddedHeight < frame.height || frame.paddedWidth % 64 || frame.paddedHeight % 64)
        throw std::invalid_argument("Invalid RIFE crop/padding dimensions");
    const auto format = checkColor(frame.sourceColor);
    const auto planeSize = static_cast<std::size_t>(frame.paddedWidth) * static_cast<std::size_t>(frame.paddedHeight);
    if (frame.pixels.size() != planeSize * 3) throw std::invalid_argument("RIFE output float buffer has the wrong size");
    std::vector<std::uint16_t> rgb(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 3);
    for (int y = 0; y < frame.height; ++y) {
        detail::checkStopped(stop);
        for (int x = 0; x < frame.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.paddedWidth) + static_cast<std::size_t>(x);
            const auto destination = (static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x)) * 3;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto value = frame.pixels[channel * planeSize + pixel];
                if (!std::isfinite(value) || value < -1e-6F || value > 1.000001F) throw std::invalid_argument("RIFE generated invalid normalized RGB values");
                rgb[destination + channel] = static_cast<std::uint16_t>(std::lround(std::clamp(value,0.0F,1.0F) * 65535.0F));
            }
        }
    }
    std::unique_ptr<AVFrame,FrameDeleter> native(av_frame_alloc());
    if (!native) throw std::bad_alloc();
    native->width = frame.width; native->height = frame.height; native->format = format;
    detail::check(av_frame_get_buffer(native.get(),32), "allocate restored RIFE video planes");
    native->color_primaries = AVCOL_PRI_BT709;
    native->color_trc = frame.sourceColor.transfer == "bt709" ? AVCOL_TRC_BT709 : AVCOL_TRC_IEC61966_2_1;
    native->colorspace = frame.sourceColor.matrix == "gbr" ? AVCOL_SPC_RGB : AVCOL_SPC_BT709;
    native->color_range = frame.sourceColor.range == "pc" ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    auto context = converter(frame.width,frame.height,AV_PIX_FMT_RGB48LE,format,1,frame.sourceColor.range == "pc");
    const std::uint8_t* source[4] = {reinterpret_cast<const std::uint8_t*>(rgb.data())}; const int stride[4] = {frame.width * 6};
    if (sws_scale(context.get(),source,stride,0,frame.height,native->data,native->linesize) != frame.height) throw std::runtime_error("Incomplete RIFE SDR postprocessing");
    DecodedFrame output{};
    output.width = frame.width; output.height = frame.height; output.color = detail::color(native.get());
    for (int plane = 0; plane < 4; ++plane) output.planes[plane] = {native->data[plane],native->linesize[plane]};
    output.storage = std::shared_ptr<const void>(native.release(), [](const void* pointer) { auto* owned = const_cast<AVFrame*>(static_cast<const AVFrame*>(pointer)); av_frame_free(&owned); });
    return output;
}
} // namespace rifelove::media
