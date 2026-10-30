#include "language_settings.hpp"

#include <QSettings>

namespace {
QString normalizedLanguage(QString language) {
    language = language.trimmed().replace(QLatin1Char('-'), QLatin1Char('_'));
    if (language.compare(QStringLiteral("zh_CN"), Qt::CaseInsensitive) == 0 ||
        language.compare(QStringLiteral("zh_SG"), Qt::CaseInsensitive) == 0) return QStringLiteral("zh_CN");
    if (language.compare(QStringLiteral("zh_TW"), Qt::CaseInsensitive) == 0 ||
        language.compare(QStringLiteral("zh_HK"), Qt::CaseInsensitive) == 0 ||
        language.compare(QStringLiteral("zh_MO"), Qt::CaseInsensitive) == 0) return QStringLiteral("zh_TW");
    if (language.compare(QStringLiteral("en"), Qt::CaseInsensitive) == 0 ||
        language.startsWith(QStringLiteral("en_"), Qt::CaseInsensitive)) return QStringLiteral("en");
    if (language.compare(QStringLiteral("ja"), Qt::CaseInsensitive) == 0 ||
        language.startsWith(QStringLiteral("ja_"), Qt::CaseInsensitive)) return QStringLiteral("ja");
    return {};
}
}

LanguageSettings::LanguageSettings(QString settingsFile, QString detectedLanguage, QObject* parent)
    : QObject(parent), settingsFile_(std::move(settingsFile)),
      activeLanguage_(normalizedLanguage(std::move(detectedLanguage))) {
    if (activeLanguage_.isEmpty()) activeLanguage_ = QStringLiteral("zh_CN");

    QSettings settings(settingsFile_, QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    settings.sync();
    const auto stored = normalizedLanguage(settings.value(QStringLiteral("ui/language")).toString());
    selectedLanguage_ = stored.isEmpty() ? activeLanguage_ : stored;
    if (stored.isEmpty()) {
        settings.setValue(QStringLiteral("ui/language"), selectedLanguage_);
        settings.sync();
    }
}

bool LanguageSettings::isSupportedLanguage(const QString& language) {
    return !normalizedLanguage(language).isEmpty();
}

bool LanguageSettings::selectLanguage(const QString& language) {
    const auto normalized = normalizedLanguage(language);
    if (normalized.isEmpty()) return false;
    if (normalized == selectedLanguage_) return true;

    QSettings settings(settingsFile_, QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    settings.setValue(QStringLiteral("ui/language"), normalized);
    settings.sync();
    if (settings.status() != QSettings::NoError) return false;

    selectedLanguage_ = normalized;
    emit changed();
    return true;
}
