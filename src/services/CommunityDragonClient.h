#pragma once

#include <QByteArray>
#include <QString>

#include <functional>
#include <memory>

namespace Janna {

class CommunityDragonClient {
public:
    using Reply = std::function<void(QByteArray, QString)>;

    CommunityDragonClient();
    ~CommunityDragonClient();
    CommunityDragonClient(const CommunityDragonClient &) = delete;
    CommunityDragonClient &operator=(const CommunityDragonClient &) = delete;

    void fetchProfileIconCatalog(Reply reply);
    void fetchStaticAsset(const QString &path, Reply reply);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Janna
