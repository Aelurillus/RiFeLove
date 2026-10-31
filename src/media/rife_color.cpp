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
    const bool supportedPrimaries = color.primaries == "bt709" || color.primaries == "bt470bg" || color.primaries == "smpte170m";
    const bool supportedTransfer = color.transfer == "bt709" || color.transfer == "smpte170m" || color.transfer == "iec61966-2-1";
    const bool supportedBitDepth = color.bitDepth == 8 || color.bitDepth == 10;
    if (!descriptor || !supportedBitDepth || descriptor->comp[0].depth != color.bitDepth || color.hdrTransfer || color.masteringDisplay || color.contentLight ||
        !supportedPrimaries || !supportedTransfer)
        throw std::invalid_argument("RIFE SDR path requires tagged 8-bit or 10-bit BT.709 or BT.470BG/SMPTE 170M primaries and SDR transfer; HDR/unknown color needs a separately validated path");
    if (descriptor->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_ALPHA | AV_PIX_FMT_FLAG_FLOAT))
        throw std::invalid_argument("RIFE SDR path does not accept hardware, palette, alpha or floating source pixels");
    const bool rgb = (descriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    const bool bt709Yuv = color.matrix == "bt709" && color.primaries == "bt709";
    const bool bt601Yuv = (color.matrix == "smpte170m" || color.matrix == "bt470bg") &&
        (color.primaries == "bt470bg" || color.primaries == "smpte170m");
    if ((rgb && (color.matrix != "gbr" || color.range != "pc")) || (!rgb && !bt709Yuv && !bt601Yuv) ||
        (color.range != "tv" && color.range != "pc"))
        throw std::invalid_argument("RIFE SDR path requires explicit matching RGB, BT.709 YUV or BT.601-family YUV matrix and range");
    return format;
}
bool isBt601Matrix(const std::string& matrix) { return matrix == "smpte170m" || matrix == "bt470bg"; }
bool isBt601Primaries(const std::string& primaries) { return primaries == "bt470bg" || primaries == "smpte170m"; }
bool isBt601SdrTransfer(const std::string& transfer) { return transfer == "bt709" || transfer == "smpte170m"; }
int swscaleMatrix(const ColorInfo& color) {
    return color.matrix == "smpte170m" || color.matrix == "bt470bg" ? SWS_CS_ITU601 : SWS_CS_ITU709;
}
void dimensions(int width, int height) {
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) throw std::invalid_argument("Invalid RIFE SDR dimensions");
}
std::unique_ptr<SwsContext, ScaleDeleter> converter(int width, int height, AVPixelFormat input, AVPixelFormat output,
                                                    const ColorInfo& color, int sourceRange, int outputRange) {
    std::unique_ptr<SwsContext, ScaleDeleter> context(sws_getContext(width,height,input,width,height,output,SWS_BILINEAR | SWS_ACCURATE_RND,nullptr,nullptr,nullptr));
    if (!context) throw std::runtime_error("Could not create explicit SDR pixel converter");
    const int* coefficients = sws_getCoefficients(swscaleMatrix(color));
    detail::check(sws_setColorspaceDetails(context.get(),coefficients,sourceRange,coefficients,outputRange,0,1 << 16,1 << 16), "configure tagged SDR matrix/range");
    return context;
}
}
void validateRifeSdrColor(const ColorInfo& color) { (void)checkColor(color); }
std::string rifeSdrInputProfile(const ColorInfo& color) {
    (void)checkColor(color);
    const auto family = color.primaries == "bt709" ? "bt709" : "bt470bg";
    return std::string(family) + "-sdr" + std::to_string(color.bitDepth);
}
ColorInfo normalizeDecodedSdrColor(const ColorInfo& decodedColor, const ColorInfo& streamColor) {
    try {
        (void)checkColor(decodedColor);
        (void)checkColor(streamColor);
    } catch (const std::invalid_argument&) {
        return decodedColor;
    }

    // Hardware frame transfer can change the host pixel layout (for example,
    // yuv420p to nv12) without changing the decoded color interpretation.
    // Reconcile tags independently of layout, but never across bit depths.
    const bool sameBitDepth = decodedColor.bitDepth == streamColor.bitDepth;
    const bool samePrimariesAndRange = decodedColor.primaries == streamColor.primaries && decodedColor.range == streamColor.range;
    const bool sameBt601Family = isBt601Primaries(decodedColor.primaries) && isBt601Matrix(decodedColor.matrix) &&
        isBt601Matrix(streamColor.matrix) && isBt601SdrTransfer(decodedColor.transfer) && isBt601SdrTransfer(streamColor.transfer);
    if (!sameBitDepth || !samePrimariesAndRange || !sameBt601Family || decodedColor.hdrTransfer || streamColor.hdrTransfer ||
        decodedColor.masteringDisplay || streamColor.masteringDisplay || decodedColor.contentLight || streamColor.contentLight)
        return decodedColor;

    // Some files carry BT.601-family VUI tags that disagree with the container tags.
    // Keep the container stream as the single output metadata contract after verifying
    // that both descriptions are explicit, supported SDR, and agree on primaries and range.
    auto normalized = decodedColor;
    normalized.transfer = streamColor.transfer;
    normalized.matrix = streamColor.matrix;
    return normalized;
}
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
    auto context = converter(frame.width,frame.height,inputFormat,AV_PIX_FMT_RGB48LE,frame.color,frame.color.range == "pc",1);
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
    native->color_primaries = static_cast<AVColorPrimaries>(av_color_primaries_from_name(frame.sourceColor.primaries.c_str()));
    native->color_trc = static_cast<AVColorTransferCharacteristic>(av_color_transfer_from_name(frame.sourceColor.transfer.c_str()));
    native->colorspace = static_cast<AVColorSpace>(av_color_space_from_name(frame.sourceColor.matrix.c_str()));
    native->color_range = frame.sourceColor.range == "pc" ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    auto context = converter(frame.width,frame.height,AV_PIX_FMT_RGB48LE,format,frame.sourceColor,1,frame.sourceColor.range == "pc");
    const std::uint8_t* source[4] = {reinterpret_cast<const std::uint8_t*>(rgb.data())}; const int stride[4] = {frame.width * 6};
    if (sws_scale(context.get(),source,stride,0,frame.height,native->data,native->linesize) != frame.height) throw std::runtime_error("Incomplete RIFE SDR postprocessing");
    DecodedFrame output{};
    output.width = frame.width; output.height = frame.height; output.color = detail::color(native.get());
    for (int plane = 0; plane < 4; ++plane) output.planes[plane] = {native->data[plane],native->linesize[plane],std::nullopt};
    output.storage = std::shared_ptr<const void>(native.release(), [](const void* pointer) { auto* owned = const_cast<AVFrame*>(static_cast<const AVFrame*>(pointer)); av_frame_free(&owned); });
    return output;
}
} // namespace rifelove::media
