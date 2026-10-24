#pragma once

#include "media/video_writer.hpp"
#include <QVariantMap>
#include <stdexcept>

namespace rifelove::desktop {

inline int integerSetting(const QVariantMap& values, const char* key, int fallback, int minimum, int maximum) {
    bool ok = false;
    const int value = values.value(QString::fromLatin1(key), fallback).toInt(&ok);
    if (!ok || value < minimum || value > maximum)
        throw std::invalid_argument(std::string("Invalid advanced encoding setting: ") + key);
    return value;
}

inline media::VideoEncodingSettings makeVideoEncodingSettings(const QString& selectedEncoder, const QVariantMap& values) {
    const auto encoder = selectedEncoder.toLatin1().toStdString();
    if (encoder != "auto" && encoder != "libopenh264" && encoder != "libkvazaar" && encoder != "libaom-av1")
        throw std::invalid_argument("Unsupported desktop video encoder selection");

    media::VideoEncodingSettings result;
    result.encoder = encoder;
    result.gopSize = integerSetting(values, "gopSize", 120, 0, 1000);
    const int bitrateKbps = integerSetting(values, "bitrateKbps", 8000, 250, 200000);
    const auto mode = values.value(QStringLiteral("qualityMode"), QStringLiteral("bitrate")).toString();

    if (mode == QStringLiteral("bitrate")) {
        result.bitRate = static_cast<std::int64_t>(bitrateKbps) * 1000;
    } else if (mode == QStringLiteral("qp") && encoder == "libkvazaar") {
        const int qp = integerSetting(values, "quality", 30, 0, 51);
        result.bitRate = 0;
        result.encoderOptions.emplace("kvazaar-params", "qp=" + std::to_string(qp));
    } else if (mode == QStringLiteral("crf") && encoder == "libaom-av1") {
        const int crf = integerSetting(values, "quality", 32, 0, 63);
        result.bitRate = 0;
        result.encoderOptions.emplace("crf", std::to_string(crf));
    } else {
        throw std::invalid_argument("Selected encoder does not support the requested quality mode");
    }
    return result;
}

inline QVariantMap videoEncodingDefaults() {
    return {{QStringLiteral("bitrateKbps"), 8000}, {QStringLiteral("qualityMode"), QStringLiteral("bitrate")},
            {QStringLiteral("quality"), 32}, {QStringLiteral("gopSize"), 120}};
}

} // namespace rifelove::desktop
