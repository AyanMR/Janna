#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <memory>

namespace Janna {

class LcuClient {
public:
    using Reply = std::function<void(QJsonDocument, QString)>;
    using BytesReply = std::function<void(QByteArray, QString)>;

    LcuClient();
    ~LcuClient();
    LcuClient(const LcuClient &) = delete;
    LcuClient &operator=(const LcuClient &) = delete;

    // Sends an authenticated request to the local client.  GET remains the
    // default convenience API, while champ-select and perks need PATCH/POST
    // requests with a JSON body.
    void request(const QString &method, const QString &path, const QByteArray &body, BytesReply reply);
    void requestJson(const QString &method, const QString &path, const QJsonObject &body, Reply reply);
    void get(const QString &path, Reply reply);
    void getBytes(const QString &path, BytesReply reply);
    void post(const QString &path, const QByteArray &body, BytesReply reply);
    void put(const QString &path, const QByteArray &body, BytesReply reply);
    void patch(const QString &path, const QByteArray &body, BytesReply reply);
    void postJson(const QString &path, const QJsonObject &body, Reply reply);
    void putJson(const QString &path, const QJsonObject &body, Reply reply);
    void patchJson(const QString &path, const QJsonObject &body, Reply reply);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Janna
