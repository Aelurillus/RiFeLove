#include "profile_store.hpp"
#include "encoding_settings.hpp"
#include "common/rational.hpp"

#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace rifelove::desktop {
namespace {
std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }
QStringList knownEncoders() { return {"auto","libopenh264","libkvazaar","libaom-av1","h264_nvenc"}; }
QStringList knownContainers() { return {"mkv","mp4","mov"}; }

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

} // namespace rifelove::desktop
