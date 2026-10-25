#include "profile_store.hpp"

#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>
#include <QFile>
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

TEST(LocalProfileStore, ExportsAndImportsOneLocalProfileWithoutItsDatabaseId) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto settingsPath = temporary.filePath(QStringLiteral("preferences.ini"));
    const auto profilePath = temporary.filePath(QStringLiteral("Smooth.json"));
    QString id;
    {
        QSettings settings(settingsPath,QSettings::IniFormat);
        settings.setFallbacksEnabled(false);
        rifelove::desktop::LocalProfileStore profiles(settings);
        id = profiles.save(QStringLiteral("Smooth 60"),validProfile());
        ASSERT_NO_THROW(profiles.exportToFile(id,profilePath));
        QFile exported(profilePath);
        ASSERT_TRUE(exported.open(QIODevice::ReadOnly));
        const auto json = exported.readAll();
        EXPECT_FALSE(json.contains(id.toUtf8()));
        EXPECT_TRUE(json.contains("rifelove-local-profile"));
        ASSERT_TRUE(profiles.remove(id));
        const auto importedId = profiles.importFromFile(profilePath);
        EXPECT_FALSE(importedId.isEmpty());
        const auto listed = profiles.list();
        ASSERT_EQ(listed.size(),1);
        const auto imported = listed.front().toMap();
        EXPECT_EQ(imported.value("id").toString(),importedId);
        EXPECT_EQ(imported.value("name").toString(),QStringLiteral("Smooth 60"));
        EXPECT_EQ(imported.value("rate").toString(),QStringLiteral("60000/1001"));
        EXPECT_EQ(imported.value("model").toUrl(),validProfile().value("model").toUrl());
        EXPECT_EQ(imported.value("runtime").toUrl(),validProfile().value("runtime").toUrl());
        EXPECT_EQ(imported.value("output").toUrl(),validProfile().value("output").toUrl());
        EXPECT_EQ(imported.value("encoder").toString(),QStringLiteral("libopenh264"));
        EXPECT_EQ(imported.value("gopSize").toInt(),120);
    }
}

TEST(LocalProfileStore, RejectsUnsupportedOrNonLocalProfileFilesWithoutChangingSettings) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QSettings settings(temporary.filePath(QStringLiteral("preferences.ini")),QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    rifelove::desktop::LocalProfileStore profiles(settings);
    (void)profiles.save(QStringLiteral("Existing"),validProfile());

    const auto badVersionPath = temporary.filePath(QStringLiteral("bad-version.json"));
    QFile badVersion(badVersionPath);
    ASSERT_TRUE(badVersion.open(QIODevice::WriteOnly));
    ASSERT_GT(badVersion.write(R"({"format":"rifelove-local-profile","version":2,"profile":{}})"),0);
    badVersion.close();
    EXPECT_THROW((void)profiles.importFromFile(badVersionPath),std::invalid_argument);
    EXPECT_EQ(profiles.list().size(),1);

    const auto remotePath = temporary.filePath(QStringLiteral("remote-path.json"));
    QFile remote(remotePath);
    ASSERT_TRUE(remote.open(QIODevice::WriteOnly));
    const QByteArray remoteJson = R"({"format":"rifelove-local-profile","version":1,"profile":{"name":"Remote","rate":"60","modelUrl":"https://example.invalid/model.onnx","runtimeUrl":"","outputUrl":"","encoder":"auto","container":"mkv","bitrateKbps":8000,"qualityMode":"bitrate","quality":32,"gopSize":120}})";
    ASSERT_EQ(remote.write(remoteJson),remoteJson.size());
    remote.close();
    EXPECT_THROW((void)profiles.importFromFile(remotePath),std::invalid_argument);
    EXPECT_EQ(profiles.list().size(),1);
}

TEST(LocalProfileStore, RejectsImportNameCollisionsAndExportOfInvalidSelections) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QSettings settings(temporary.filePath(QStringLiteral("preferences.ini")),QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    rifelove::desktop::LocalProfileStore profiles(settings);
    const auto id = profiles.save(QStringLiteral("Balanced"),validProfile());
    const auto profilePath = temporary.filePath(QStringLiteral("Balanced.json"));
    ASSERT_NO_THROW(profiles.exportToFile(id,profilePath));
    EXPECT_THROW((void)profiles.importFromFile(profilePath),std::invalid_argument);
    EXPECT_THROW(profiles.exportToFile(QStringLiteral("missing"),temporary.filePath(QStringLiteral("missing.json"))),std::invalid_argument);
    EXPECT_EQ(profiles.list().size(),1);
}
