#include "services/MatchAnalytics.h"

#include "services/ChampionRepository.h"
#include "services/GameDataRepository.h"
#include "services/RankedRepository.h"

#include <QHash>

#include <algorithm>

namespace Janna {
namespace {

int roleIndex(const MatchSummary &match)
{
    // Lane values are only meaningful for Summoner's Rift. Other maps often use
    // placeholder timeline roles, which must not be treated as Support.
    if (match.mapId != 11) return -1;
    const QString lane = match.lane.trimmed().toUpper();
    const QString role = match.role.trimmed().toUpper();
    if (lane == "TOP") return 0;
    if (lane == "JUNGLE") return 1;
    if (lane == "MIDDLE" || lane == "MID") return 2;
    if (lane == "BOTTOM" || lane == "BOT") {
        return role.contains("SUPPORT") ? 4 : 3;
    }
    if (lane == "SUPPORT") return 4;
    return -1;
}

QString championTypeKey(const QStringList &roles)
{
    for (const QString &role : roles) {
        const QString normalized = role.trimmed().toUpper();
        if (normalized == "ASSASSIN" || normalized == "FIGHTER" || normalized == "MAGE"
            || normalized == "MARKSMAN" || normalized == "SUPPORT" || normalized == "TANK") {
            return normalized;
        }
    }
    return {};
}

QString championTypeLabel(const QString &type)
{
    if (type == "ASSASSIN") return "刺客";
    if (type == "FIGHTER") return "战士";
    if (type == "MAGE") return "法师";
    if (type == "MARKSMAN") return "射手";
    if (type == "SUPPORT") return "辅助";
    if (type == "TANK") return "坦克";
    return {};
}

} // namespace

QList<RoleUsage> MatchAnalytics::roleUsage(const QList<MatchSummary> &matches)
{
    QList<RoleUsage> result = {{"上路", 0}, {"打野", 0}, {"中路", 0}, {"下路", 0}, {"辅助", 0}};
    bool hasLaneData = false;
    for (const MatchSummary &match : matches) {
        const int index = roleIndex(match);
        if (index >= 0) {
            ++result[index].games;
            hasLaneData = true;
        }
    }
    return hasLaneData ? result : QList<RoleUsage>{};
}

QList<RoleUsage> MatchAnalytics::championTypeUsage(const QList<MatchSummary> &matches,
                                                     const ChampionRepository &champions)
{
    QHash<int, QString> typeByChampion;
    for (const Champion &champion : champions.champions()) {
        const QString type = championTypeKey(champion.roles);
        if (!type.isEmpty()) typeByChampion.insert(champion.id, type);
    }

    QHash<QString, int> gamesByType;
    for (const MatchSummary &match : matches) {
        const QString type = typeByChampion.value(match.championId);
        if (!type.isEmpty()) ++gamesByType[type];
    }

    QList<RoleUsage> result;
    for (const QString &type : {QStringLiteral("ASSASSIN"), QStringLiteral("FIGHTER"), QStringLiteral("MAGE"),
                                QStringLiteral("MARKSMAN"), QStringLiteral("SUPPORT"), QStringLiteral("TANK")}) {
        const int games = gamesByType.value(type);
        if (games > 0) result.append({championTypeLabel(type), games});
    }
    std::sort(result.begin(), result.end(), [](const RoleUsage &left, const RoleUsage &right) {
        if (left.games != right.games) return left.games > right.games;
        return left.label.localeAwareCompare(right.label) < 0;
    });
    return result;
}

QList<ChampionUsage> MatchAnalytics::recentChampionUsage(const QList<MatchSummary> &matches)
{
    QList<MatchSummary> orderedMatches = matches;
    std::stable_sort(orderedMatches.begin(), orderedMatches.end(), [](const MatchSummary &left, const MatchSummary &right) {
        if (!left.createdAt.isValid()) return false;
        if (!right.createdAt.isValid()) return true;
        return left.createdAt > right.createdAt;
    });

    QList<ChampionUsage> result;
    QHash<int, int> indexByChampion;
    for (const MatchSummary &match : orderedMatches) {
        if (match.championId <= 0) continue;
        int index = indexByChampion.value(match.championId, -1);
        if (index < 0) {
            index = result.size();
            indexByChampion.insert(match.championId, index);
            ChampionUsage usage;
            usage.championId = match.championId;
            result.append(usage);
        }
        ChampionUsage &usage = result[index];
        ++usage.games;
        match.won ? ++usage.wins : ++usage.losses;
        usage.kills += match.kills;
        usage.deaths += match.deaths;
        usage.assists += match.assists;
    }
    return result;
}

QList<MatchModeStatistics> MatchAnalytics::modeStatistics(const QList<MatchSummary> &matches,
                                                            const GameDataRepository &gameData,
                                                            const RankedRepository &ranked)
{
    QList<MatchModeStatistics> result;
    QHash<int, int> indexByQueue;
    for (const MatchSummary &match : matches) {
        int index = indexByQueue.value(match.queueId, -1);
        if (index < 0) {
            index = result.size();
            indexByQueue.insert(match.queueId, index);
            MatchModeStatistics statistics;
            statistics.queueId = match.queueId;
            statistics.modeName = gameData.modeName(match.queueId, match.mapId, match.gameMode);
            result.append(statistics);
        }
        MatchModeStatistics &statistics = result[index];
        ++statistics.games;
        match.won ? ++statistics.wins : ++statistics.losses;
    }

    QList<int> rankedQueueIds = ranked.stats().keys();
    std::sort(rankedQueueIds.begin(), rankedQueueIds.end());
    for (const int queueId : rankedQueueIds) {
        const RankedQueueStats queueStats = ranked.stats().value(queueId);
        int index = indexByQueue.value(queueId, -1);
        if (index < 0) {
            index = result.size();
            indexByQueue.insert(queueId, index);
            MatchModeStatistics statistics;
            statistics.queueId = queueId;
            statistics.modeName = gameData.queueName(queueId);
            result.append(statistics);
        }
        MatchModeStatistics &statistics = result[index];
        statistics.games = queueStats.wins + queueStats.losses;
        statistics.wins = queueStats.wins;
        statistics.losses = queueStats.losses;
        statistics.rank = RankedRepository::currentRankText(queueStats);
        statistics.highestRank = RankedRepository::highestRankText(queueStats);
    }

    for (MatchModeStatistics &statistics : result) {
        if (statistics.rank.isEmpty()) statistics.rank = "---";
        if (statistics.highestRank.isEmpty()) statistics.highestRank = "---";
    }
    return result;
}

} // namespace Janna
