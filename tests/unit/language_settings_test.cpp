#include "language_settings.hpp"

#include <QSettings>
#include <QTemporaryDir>
#include <gtest/gtest.h>

TEST(LanguageSettings, SelectsPersistsAndRequiresRestartForNewLanguage) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto settingsFile = temporary.filePath(QStringLiteral("language.ini"));

    LanguageSettings settings(settingsFile, QStringLiteral("en_US"));
    EXPECT_EQ(settings.language(), QStringLiteral("en"));
    EXPECT_FALSE(settings.restartRequired());
    EXPECT_TRUE(settings.selectLanguage(QStringLiteral("ja_JP")));
    EXPECT_EQ(settings.language(), QStringLiteral("ja"));
    EXPECT_TRUE(settings.restartRequired());

    LanguageSettings reopened(settingsFile, QStringLiteral("en"));
    EXPECT_EQ(reopened.language(), QStringLiteral("ja"));
    EXPECT_TRUE(reopened.restartRequired());
    EXPECT_TRUE(reopened.selectLanguage(QStringLiteral("ja")));
    EXPECT_TRUE(reopened.restartRequired());

    LanguageSettings afterRestart(settingsFile, QStringLiteral("ja_JP"));
    EXPECT_EQ(afterRestart.language(), QStringLiteral("ja"));
    EXPECT_FALSE(afterRestart.restartRequired());
}

TEST(LanguageSettings, RejectsUnsupportedLanguageWithoutChangingSavedSelection) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto settingsFile = temporary.filePath(QStringLiteral("language.ini"));

    LanguageSettings settings(settingsFile, QStringLiteral("zh_CN"));
    EXPECT_FALSE(settings.selectLanguage(QStringLiteral("fr")));
    EXPECT_EQ(settings.language(), QStringLiteral("zh_CN"));
    EXPECT_FALSE(settings.restartRequired());

    QSettings saved(settingsFile, QSettings::IniFormat);
    EXPECT_EQ(saved.value(QStringLiteral("ui/language")).toString(), QStringLiteral("zh_CN"));
}
