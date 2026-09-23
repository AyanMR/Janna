#include "services/GameDataRepository.h"

#include "services/LcuClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>

#include <algorithm>

namespace Janna {
namespace {

int catalogId(const QJsonValue &value)
{
    bool valid = false;
    const int id = value.toVariant().toInt(&valid);
    return valid ? id : 0;
}

bool isGenericRandomMapName(const QString &name)
{
    const QString normalized = name.trimmed().toUpper();
    return normalized == QString::fromUtf8("随机地图") || normalized == "RANDOM MAP";
}

} // namespace

GameDataRepository::GameDataRepository(LcuClient &client, QObject *parent) : QObject(parent), client_(client) {}

QString GameDataRepository::queueName(const int queueId) const
{
    const QString name = queueNames_.value(queueId).trimmed();
    return !name.isEmpty() ? name : queueId > 0 ? "队列 " + QString::number(queueId) : "未知队列";
}

QString GameDataRepository::mapName(const int mapId) const
{
    const QString name = mapNames_.value(mapId).trimmed();
    return !name.isEmpty() ? name : mapId > 0 ? "地图 " + QString::number(mapId) : "未知地图";
}

QString GameDataRepository::modeName(const int queueId, const int mapId, const QString &) const
{
    const QString queue = queueNames_.value(queueId).trimmed();
    const QString map = mapNames_.value(mapId).trimmed();
    // Some LCU queues are labelled only as a random map. The actual match
    // still has a map ID, whose catalog entry is the useful localized name.
    if (!map.isEmpty() && (queue.isEmpty() || isGenericRandomMapName(queue))) return map;
    if (!queue.isEmpty()) return queue;
    if (!map.isEmpty()) return map + " 对局";
    return queueName(queueId);
}

QList<GameQueue> GameDataRepository::queues() const
{
    QList<GameQueue> result;
    result.reserve(queueNames_.size());
    for (auto it = queueNames_.cbegin(); it != queueNames_.cend(); ++it) {
        const QString name = it.value().trimmed();
        if (it.key() > 0 && !name.isEmpty()) result.append({it.key(), name});
    }
    std::sort(result.begin(), result.end(), [](const GameQueue &left, const GameQueue &right) {
        const int compared = left.name.localeAwareCompare(right.name);
        return compared == 0 ? left.id < right.id : compared < 0;
    });
    return result;
}

void GameDataRepository::refresh()
{
    const quint64 generation = ++refreshGeneration_;
    pendingCatalogs_ = 2;
    catalogChangedDuringRefresh_ = false;
    const QPointer<GameDataRepository> repository(this);
    client_.get("/lol-game-data/assets/v1/queues.json", [repository, generation](QJsonDocument document, QString error) {
        if (repository) repository->applyCatalog(document, true, generation, error);
    });
    client_.get("/lol-game-data/assets/v1/maps.json", [repository, generation](QJsonDocument document, QString error) {
        if (repository) repository->applyCatalog(document, false, generation, error);
    });
}

void GameDataRepository::applyCatalog(const QJsonDocument &document, const bool queueCatalog, const quint64 generation, const QString &error)
{
    if (generation != refreshGeneration_) return;
    if (error.isEmpty()) {
        QHash<int, QString> &target = queueCatalog ? queueNames_ : mapNames_;
        const QJsonArray entries = document.isArray() ? document.array() : QJsonArray{};
        for (const QJsonValue &entry : entries) {
            const QJsonObject object = entry.toObject();
            const int id = catalogId(object.value("id"));
            const QString name = object.value("name").toString().trimmed();
            if (id <= 0 || name.isEmpty() || target.value(id) == name) continue;
            target.insert(id, name);
            catalogChangedDuringRefresh_ = true;
        }
    }

    --pendingCatalogs_;
    if (pendingCatalogs_ == 0 && catalogChangedDuringRefresh_) emit catalogChanged();
}

} // namespace Janna
