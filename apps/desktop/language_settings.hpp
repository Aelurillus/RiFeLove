#pragma once

#include <QObject>
#include <QString>

class LanguageSettings final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString language READ language NOTIFY changed)
    Q_PROPERTY(bool restartRequired READ restartRequired NOTIFY changed)

public:
    explicit LanguageSettings(QString settingsFile, QString detectedLanguage, QObject* parent = nullptr);

    [[nodiscard]] QString language() const { return selectedLanguage_; }
    [[nodiscard]] bool restartRequired() const { return selectedLanguage_ != activeLanguage_; }

    Q_INVOKABLE bool selectLanguage(const QString& language);

    [[nodiscard]] static bool isSupportedLanguage(const QString& language);

signals:
    void changed();

private:
    QString settingsFile_;
    QString activeLanguage_;
    QString selectedLanguage_;
};
