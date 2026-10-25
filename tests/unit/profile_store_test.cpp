#include "profile_store.hpp"

#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>
#include <gtest/gtest.h>
#include <stdexcept>

namespace {
QVariantMap validProfile() {
    return {{"rate",QStringLiteral("60000/1001")},{"model",QUrl::fromLocalFile("C:/models/rife")},
        {"runtime",QUrl::fromLocalFile("C:/runtime")},{"output",QUrl::fromLocalFile("C:/renders")},
        {"encoder",QStringLiteral("libopenh264")},{"container",QStringLiteral("mkv")},
        {"bitrateKbps",8000},{"qualityMode",QStringLiteral("bitrate")},{"quality",32},{"gopSize",120}};
}
}

TEST(LocalProfileStore, SavesListsAndRemovesPersistentNamedProfiles) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto settingsPath = temporary.filePath(QStringLiteral("preferences.ini"));
    QString id;
    {
        QSettings settings(settingsPath,QSettings::IniFormat);
        settings.setFallbacksEnabled(false);
        rifelove::desktop::LocalProfileStore profiles(settings);
        id = profiles.save(QStringLiteral("  Smooth 60  "),validProfile());
        ASSERT_FALSE(id.isEmpty());
        const auto listed = profiles.list();
        ASSERT_EQ(listed.size(),1);
        const auto value = listed.front().toMap();
        EXPECT_TRUE(value.value("valid").toBool());
        EXPECT_EQ(value.value("name").toString(),QStringLiteral("Smooth 60"));
        EXPECT_EQ(value.value("rate").toString(),QStringLiteral("60000/1001"));
        EXPECT_EQ(value.value("encoder").toString(),QStringLiteral("libopenh264"));
        EXPECT_EQ(value.value("bitrateKbps").toInt(),8000);
    }
    {
        QSettings settings(settingsPath,QSettings::IniFormat);
        settings.setFallbacksEnabled(false);
        rifelove::desktop::LocalProfileStore reopened(settings);
        const auto listed = reopened.list();
        ASSERT_EQ(listed.size(),1);
        EXPECT_EQ(listed.front().toMap().value("id").toString(),id);
        EXPECT_TRUE(reopened.remove(id));
        EXPECT_FALSE(reopened.remove(id));
        EXPECT_TRUE(reopened.list().isEmpty());
    }
}

TEST(LocalProfileStore, RejectsInvalidOrAmbiguousProfiles) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QSettings settings(temporary.filePath(QStringLiteral("preferences.ini")),QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    rifelove::desktop::LocalProfileStore profiles(settings);

    EXPECT_THROW((void)profiles.save(QStringLiteral("  "),validProfile()),std::invalid_argument);
    EXPECT_THROW((void)profiles.save(QStringLiteral("bad\nname"),validProfile()),std::invalid_argument);
    EXPECT_THROW((void)profiles.save(QStringLiteral("Bad Rate"),QVariantMap{{"rate",QStringLiteral("0")}}),std::invalid_argument);
    EXPECT_THROW((void)profiles.save(QStringLiteral("Web URL"),QVariantMap{{"model",QUrl("https://example.invalid/model")}}),std::invalid_argument);
    EXPECT_THROW((void)profiles.save(QStringLiteral("Bad Codec"),QVariantMap{{"encoder",QStringLiteral("unknown")}}),std::invalid_argument);

    (void)profiles.save(QStringLiteral("Balanced"),validProfile());
    EXPECT_THROW((void)profiles.save(QStringLiteral("balanced"),validProfile()),std::invalid_argument);
}
