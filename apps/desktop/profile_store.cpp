#include "profile_store.hpp"
#include "encoding_settings.hpp"
#include "common/rational.hpp"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace rifelove::desktop {
namespace {
std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }
QStringList knownEncoders() { return {"auto","libopenh264","libkvazaar","libaom-av1","h264_nvenc"}; }
QStringList knownContainers() { return {"mkv","mp4","mov"}; }
constexpr auto profileFormat = "rifelove-local-profile";
constexpr int profileFormatVersion = 1;
constexpr qint64 maxProfileFileBytes = 64 * 1024;

QJsonObject profileValuesToJson(const QVariantMap& profile) {
    auto localUrl = [&](const QString& key) {
        const auto value = profile.value(key);
        if (!value.isValid() || value.isNull()) return QString();
        const auto url = value.canConvert<QUrl>() ? value.toUrl() : QUrl(value.toString());
        return url.isEmpty() ? QString() : url.toString(QUrl::FullyEncoded);
    };
    return {{"name",profile.value("name").toString()}, {"rate",profile.value("rate").toString()},
        {"modelUrl",localUrl(QStringLiteral("model"))}, {"runtimeUrl",localUrl(QStringLiteral("runtime"))},
        {"outputUrl",localUrl(QStringLiteral("output"))}, {"encoder",profile.value("encoder").toString()},
        {"container",profile.value("container").toString()}, {"bitrateKbps",profile.value("bitrateKbps").toInt()},
        {"qualityMode",profile.value("qualityMode").toString()}, {"quality",profile.value("quality").toInt()},
        {"gopSize",profile.value("gopSize").toInt()}};
}

QUrl localUrlFromJson(const QJsonObject& object, const char* key) {
    const auto value = object.value(QLatin1StringView(key));
    if (!value.isString()) throw std::invalid_argument("Profile local path fields must be strings");
    const auto encoded = value.toString();
    if (encoded.isEmpty()) return {};
    const auto url = QUrl::fromEncoded(encoded.toUtf8(),QUrl::StrictMode);
    if (!url.isValid() || !url.isLocalFile())
        throw std::invalid_argument("Profile paths must use local file URLs");
    return url;
}

QVariantMap profileValuesFromJson(const QJsonObject& object) {
    const QStringList fields{"name","rate","modelUrl","runtimeUrl","outputUrl","encoder","container",
        "bitrateKbps","qualityMode","quality","gopSize"};
    for (const auto& key : object.keys())
        if (!fields.contains(key)) throw std::invalid_argument("Profile file contains an unsupported field");
    for (const auto& key : fields)
        if (!object.contains(key)) throw std::invalid_argument("Profile file is missing a required field");
    if (!object.value("name").isString() || !object.value("rate").isString() ||
        !object.value("encoder").isString() || !object.value("container").isString() ||
        !object.value("qualityMode").isString() || !object.value("bitrateKbps").isDouble() ||
        !object.value("quality").isDouble() || !object.value("gopSize").isDouble())
        throw std::invalid_argument("Profile file contains a field with the wrong type");
    return {{"name",object.value("name").toString()}, {"rate",object.value("rate").toString()},
        {"model",localUrlFromJson(object,"modelUrl")}, {"runtime",localUrlFromJson(object,"runtimeUrl")},
        {"output",localUrlFromJson(object,"outputUrl")}, {"encoder",object.value("encoder").toString()},
        {"container",object.value("container").toString()}, {"bitrateKbps",object.value("bitrateKbps").toInt()},
        {"qualityMode",object.value("qualityMode").toString()}, {"quality",object.value("quality").toInt()},
        {"gopSize",object.value("gopSize").toInt()}};
}

void requireLocalOrEmpty(const QVariantMap& values, const QString& key) {
    const auto value = values.value(key);
    if (!value.isValid() || value.isNull()) return;
    const auto url = value.canConvert<QUrl>() ? value.toUrl() : QUrl(value.toString());
    if (!url.isEmpty() && !url.isLocalFile())
        throw std::invalid_argument((key + QStringLiteral(" must refer to a local file or folder")).toStdString());
}

QVariantMap loadValues(QSettings& settings) {
    QVariantMap values;
    values["rate"] = settings.value("rate");
    values["model"] = settings.value("model");
    values["runtime"] = settings.value("runtime");
    values["output"] = settings.value("output");
    values["encoder"] = settings.value("encoder");
    values["container"] = settings.value("container");
    values["bitrateKbps"] = settings.value("bitrateKbps");
    values["qualityMode"] = settings.value("qualityMode");
    values["quality"] = settings.value("quality");
    values["gopSize"] = settings.value("gopSize");
    return values;
}

void saveValues(QSettings& settings, const QVariantMap& values) {
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        settings.setValue(it.key(), it.value());
}
}

QVariantMap LocalProfileStore::normalize(const QString& rawName, const QVariantMap& values) {
    const auto name = rawName.trimmed();
    if (name.isEmpty() || name.size() > 64 || name.contains(QChar::Null) || name.contains('\n') || name.contains('\r'))
        throw std::invalid_argument("Profile name must contain 1 to 64 characters on one line");

    auto rate = values.value("rate", QStringLiteral("60")).toString().trimmed();
    if (Rational::parse(utf8(rate)) <= Rational(0))
        throw std::invalid_argument("Profile frame rate must be positive");

    const auto encoder = values.value("encoder", QStringLiteral("auto")).toString();
    const auto container = values.value("container", QStringLiteral("mkv")).toString();
    if (!knownEncoders().contains(encoder)) throw std::invalid_argument("Profile encoder is not supported");
    if (!knownContainers().contains(container)) throw std::invalid_argument("Profile container is not supported");

    QVariantMap options{{"bitrateKbps",values.value("bitrateKbps",8000)},
        {"qualityMode",values.value("qualityMode",QStringLiteral("bitrate"))},
        {"quality",values.value("quality",32)}, {"gopSize",values.value("gopSize",120)}};
    (void)makeVideoEncodingSettings(encoder,options);
    for (const auto* key : {"model","runtime","output"}) requireLocalOrEmpty(values,QString::fromLatin1(key));

    QVariantMap result{{"name",name},{"rate",rate},{"model",values.value("model",QUrl())},
        {"runtime",values.value("runtime",QUrl())},{"output",values.value("output",QUrl())},
        {"encoder",encoder},{"container",container}};
    for (auto it = options.cbegin(); it != options.cend(); ++it) result[it.key()] = it.value();
    return result;
}

QVariantList LocalProfileStore::list() {
    settings_.sync();
    if (settings_.status() != QSettings::NoError) throw std::runtime_error("Could not read local processing profiles");
    QVariantList result;
    settings_.beginGroup(QStringLiteral("profiles"));
    const auto groups = settings_.childGroups();
    for (const auto& id : groups) {
        settings_.beginGroup(id);
        const auto stored = loadValues(settings_);
        const auto name = settings_.value("name").toString();
        settings_.endGroup();

        QVariantMap profile{{"id",id},{"name",name},{"valid",false}};
        try {
            const auto normalized = normalize(name,stored);
            for (auto it = normalized.cbegin(); it != normalized.cend(); ++it) profile[it.key()] = it.value();
            profile["valid"] = true;
        } catch (const std::exception& error) {
            profile["error"] = QString::fromUtf8(error.what());
        }
        result.push_back(profile);
    }
    settings_.endGroup();
    std::sort(result.begin(),result.end(),[](const QVariant& left,const QVariant& right) {
        return left.toMap().value("name").toString().compare(right.toMap().value("name").toString(),Qt::CaseInsensitive) < 0;
    });
    return result;
}

QString LocalProfileStore::save(const QString& name, const QVariantMap& values) {
    const auto normalized = normalize(name,values);
    const auto profiles = list();
    if (profiles.size() >= 32) throw std::invalid_argument("A maximum of 32 local profiles is supported");
    for (const auto& value : profiles) {
        if (value.toMap().value("name").toString().compare(normalized.value("name").toString(),Qt::CaseInsensitive) == 0)
            throw std::invalid_argument("A local profile already uses this name");
    }

    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    settings_.beginGroup(QStringLiteral("profiles"));
    settings_.beginGroup(id);
    settings_.setValue("name",normalized.value("name"));
    saveValues(settings_,normalized);
    settings_.endGroup();
    settings_.endGroup();
    settings_.sync();
    if (settings_.status() != QSettings::NoError) throw std::runtime_error("Could not save the local profile");
    return id;
}

bool LocalProfileStore::remove(const QString& id) {
    if (QUuid(id).isNull()) return false;
    const auto profiles = list();
    if (std::none_of(profiles.cbegin(),profiles.cend(),[&](const QVariant& value) { return value.toMap().value("id").toString() == id; }))
        return false;
    settings_.beginGroup(QStringLiteral("profiles"));
    settings_.remove(id);
    settings_.endGroup();
    settings_.sync();
    if (settings_.status() != QSettings::NoError) throw std::runtime_error("Could not remove the local profile");
    return true;
}

void LocalProfileStore::exportToFile(const QString& id, const QString& localPath) {
    if (localPath.trimmed().isEmpty()) throw std::invalid_argument("Choose a local profile file");
    const auto profiles = list();
    const auto found = std::find_if(profiles.cbegin(),profiles.cend(),[&](const QVariant& value) {
        const auto profile = value.toMap();
        return profile.value("id").toString() == id && profile.value("valid").toBool();
    });
    if (found == profiles.cend()) throw std::invalid_argument("A valid local profile must be selected");

    const QJsonObject document{{"format",QString::fromLatin1(profileFormat)}, {"version",profileFormatVersion},
        {"profile",profileValuesToJson(found->toMap())}};
    const auto payload = QJsonDocument(document).toJson(QJsonDocument::Indented);
    QSaveFile file(localPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Could not create the local profile file");
    if (file.write(payload) != payload.size()) {
        file.cancelWriting();
        throw std::runtime_error("Could not write the local profile file");
    }
    if (!file.commit()) throw std::runtime_error("Could not finish saving the local profile file");
}

QString LocalProfileStore::importFromFile(const QString& localPath) {
    if (localPath.trimmed().isEmpty()) throw std::invalid_argument("Choose a local profile file");
    QFileInfo info(localPath);
    if (!info.exists() || !info.isFile() || info.size() > maxProfileFileBytes)
        throw std::invalid_argument("The local profile file is missing or too large");
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Could not read the local profile file");
    const auto payload = file.read(maxProfileFileBytes + 1);
    if (payload.size() > maxProfileFileBytes || file.error() != QFileDevice::NoError)
        throw std::invalid_argument("The local profile file is too large or could not be read");
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(payload,&parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        throw std::invalid_argument("The local profile file is not valid JSON");
    const auto root = document.object();
    if (root.value("format").toString() != QString::fromLatin1(profileFormat) ||
        root.value("version").toInt(-1) != profileFormatVersion || !root.value("profile").isObject())
        throw std::invalid_argument("The local profile file format or version is not supported");
    const auto values = profileValuesFromJson(root.value("profile").toObject());
    const auto normalized = normalize(values.value("name").toString(),values);
    return save(normalized.value("name").toString(),normalized);
}

} // namespace rifelove::desktop
