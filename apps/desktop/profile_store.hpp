#pragma once

#include <QSettings>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace rifelove::desktop {

class LocalProfileStore final {
public:
    explicit LocalProfileStore(QSettings& settings) : settings_(settings) {}

    [[nodiscard]] QVariantList list();
    [[nodiscard]] QString save(const QString& name, const QVariantMap& values);
    [[nodiscard]] bool remove(const QString& id);
    void exportToFile(const QString& id, const QString& localPath);
    [[nodiscard]] QString importFromFile(const QString& localPath);

    [[nodiscard]] static QVariantMap normalize(const QString& name, const QVariantMap& values);

private:
    QSettings& settings_;
};

} // namespace rifelove::desktop
