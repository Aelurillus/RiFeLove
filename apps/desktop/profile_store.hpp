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

    [[nodiscard]] static QVariantMap normalize(const QString& name, const QVariantMap& values);

private:
    QSettings& settings_;
};

} // namespace rifelove::desktop
