#include "services/RankedRepository.h"

#include "services/LcuClient.h"
#include "services/SummonerRepository.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QUrl>

namespace Janna {
namespace {

int queueIdForType(const QString &queueType)
{
    if (queueType.compare("RANKED_SOLO_5x5", Qt::CaseInsensitive) == 0) return 420;
    if (queueType.compare("RANKED_FLEX_SR", Qt::CaseInsensitive) == 0) return 440;
    return 0;
}

int jsonInt(const QJsonValue &value)
{
    bool valid = false;
    const int result = value.toVariant().toInt(&valid);
    return valid ? result : 0;
}

QString tierText(const QString &tier, const QString &division)
{
    const QString localized = RankedRepository::localizedTier(tier);
    if (localized.isEmpty()) return {};
    const QString normalizedDivision = division.trimmed().toUpper();
    if (normalizedDivision.isEmpty() || normalizedDivision == "NONE" || normalizedDivision == "NA") return localized;
    return localized + " " + normalizedDivision;
}

} // namespace

RankedRepository::RankedRepository(LcuClient &client, QObject *parent) : QObject(parent), client_(client) {}

QString RankedRepository::localizedTier(const QString &tier)
{
    const QString normalized = tier.trimmed().toUpper();
    if (normalized == "IRON") return "黑铁";
    if (normalized == "BRONZE") return "黄铜";
    if (normalized == "SILVER") return "白银";
    if (normalized == "GOLD") return "黄金";
    if (normalized == "PLATINUM") return "铂金";
    if (normalized == "EMERALD") return "翡翠";
    if (normalized == "DIAMOND") return "钻石";
    if (normalized == "MASTER") return "大师";
    if (normalized == "GRANDMASTER") return "宗师";
    if (normalized == "CHALLENGER") return "王者";
    return {};
}

QString RankedRepository::currentRankText(const RankedQueueStats &stats)
{
    const QString rank = tierText(stats.tier, stats.division);
    return rank.isEmpty() ? "---" : rank + "：" + QString::number(qMax(0, stats.leaguePoints)) + " 胜点";
}

QString RankedRepository::highestRankText(const RankedQueueStats &stats)
{
    const QString rank = tierText(stats.highestTier, stats.highestDivision);
    return rank.isEmpty() ? "---" : rank;
}

void RankedRepository::refresh(const SummonerProfile &profile)
{
    const quint64 generation = ++refreshGeneration_;
    if (!stats_.isEmpty()) {
        stats_.clear();
        emit statsChanged();
    }
    if (!profile.isValid()) return;

    emit loadingChanged(true, {});
    const QString encodedPuuid = QString::fromLatin1(QUrl::toPercentEncoding(profile.puuid));
    const QPointer<RankedRepository> repository(this);
    client_.get("/lol-ranked/v1/ranked-stats/" + encodedPuuid, [repository, generation](QJsonDocument document, QString error) {
        if (!repository || repository->refreshGeneration_ != generation) return;
        if (!error.isEmpty()) {
            emit repository->loadingChanged(false, error);
            return;
        }

        QHash<int, RankedQueueStats> loaded;
        const QJsonObject queueMap = document.object().value("queueMap").toObject();
        for (auto entry = queueMap.constBegin(); entry != queueMap.constEnd(); ++entry) {
            const QJsonObject object = entry.value().toObject();
            const QString queueType = object.value("queueType").toString();
            const int queueId = queueIdForType(queueType);
            if (queueId == 0) continue;
            RankedQueueStats stats;
            stats.queueId = queueId;
            stats.queueType = queueType;
            stats.tier = object.value("tier").toString();
            stats.division = object.value("division").toString();
            stats.leaguePoints = jsonInt(object.value("leaguePoints"));
            stats.wins = qMax(0, jsonInt(object.value("wins")));
            stats.losses = qMax(0, jsonInt(object.value("losses")));
            stats.highestTier = object.value("highestTier").toString();
            stats.highestDivision = object.value("highestDivision").toString();
            loaded.insert(queueId, std::move(stats));
        }
        repository->stats_ = std::move(loaded);
        emit repository->statsChanged();
        emit repository->loadingChanged(false, {});
    });
}

} // namespace Janna
