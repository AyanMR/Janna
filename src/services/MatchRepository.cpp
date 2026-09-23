#include "services/MatchRepository.h"

#include "services/LcuClient.h"
#include "services/SummonerRepository.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QUrl>

#include <algorithm>
#include <limits>
#include <memory>

namespace Janna {
namespace {

qint64 integerValue(const QJsonValue &value)
{
    if (value.isDouble()) return static_cast<qint64>(value.toDouble());
    bool valid = false;
    const qint64 result = value.toString().toLongLong(&valid);
    return valid ? result : 0;
}

QString stringValue(const QJsonValue &value)
{
    if (value.isString()) return value.toString();
    if (value.isDouble()) return QString::number(static_cast<qint64>(value.toDouble()));
    return {};
}

int intValue(const QJsonValue &value)
{
    const qint64 number = integerValue(value);
    const qint64 minimum = static_cast<qint64>(std::numeric_limits<int>::min());
    const qint64 maximum = static_cast<qint64>(std::numeric_limits<int>::max());
    return static_cast<int>(std::clamp(number, minimum, maximum));
}

bool boolValue(const QJsonValue &value)
{
    if (value.isBool()) return value.toBool();
    if (value.isString()) {
        const QString text = value.toString();
        return text.compare("win", Qt::CaseInsensitive) == 0 || text.compare("true", Qt::CaseInsensitive) == 0 || text == "1";
    }
    return intValue(value) != 0;
}

QString normalizedRankTier(const QString &value)
{
    const QString tier = value.trimmed().toUpper();
    static const QSet<QString> knownTiers = {
        "IRON", "BRONZE", "SILVER", "GOLD", "PLATINUM", "EMERALD", "DIAMOND", "MASTER", "GRANDMASTER", "CHALLENGER"
    };
    return knownTiers.contains(tier) ? tier : QString{};
}

void appendUniquePositive(QList<int> &values, const int value)
{
    if (value > 0 && !values.contains(value)) values.append(value);
}

QList<int> itemIdsFor(const QJsonObject &stats)
{
    QList<int> itemIds;
    for (int itemIndex = 0; itemIndex <= 6; ++itemIndex) {
        const int itemId = intValue(stats.value("item" + QString::number(itemIndex)));
        if (itemId > 0) itemIds.append(itemId);
    }
    return itemIds;
}

int primaryRuneFor(const QJsonObject &stats)
{
    const int keystone = intValue(stats.value("perk0"));
    return keystone > 0 ? keystone : intValue(stats.value("perkPrimaryStyle"));
}

QList<int> runeIdsFor(const QJsonObject &stats)
{
    QList<int> runeIds;
    for (int perkIndex = 0; perkIndex <= 5; ++perkIndex) {
        appendUniquePositive(runeIds, intValue(stats.value("perk" + QString::number(perkIndex))));
    }
    if (runeIds.isEmpty()) {
        appendUniquePositive(runeIds, intValue(stats.value("perkPrimaryStyle")));
        appendUniquePositive(runeIds, intValue(stats.value("perkSubStyle")));
    }
    return runeIds;
}

QJsonObject participantFor(const QJsonObject &game, const SummonerProfile &profile)
{
    int participantId = 0;
    const QJsonArray identities = game.value("participantIdentities").toArray();
    for (const QJsonValue &identityValue : identities) {
        const QJsonObject identity = identityValue.toObject();
        const QJsonObject player = identity.value("player").toObject();
        if (player.value("puuid").toString() == profile.puuid) {
            participantId = intValue(identity.value("participantId"));
            break;
        }
        const bool matchingName = player.value("gameName").toString() == profile.gameName
            || player.value("summonerName").toString() == profile.gameName;
        const QString tagLine = player.value("tagLine").toString();
        if (matchingName && (profile.tagLine.isEmpty() || tagLine.isEmpty() || tagLine == profile.tagLine)) {
            participantId = intValue(identity.value("participantId"));
            break;
        }
    }

    const QJsonArray participants = game.value("participants").toArray();
    for (const QJsonValue &participantValue : participants) {
        const QJsonObject participant = participantValue.toObject();
        if (participantId > 0 && intValue(participant.value("participantId")) == participantId) return participant;
    }
    return participants.size() == 1 ? participants.at(0).toObject() : QJsonObject{};
}

MatchSummary parseMatch(const QJsonObject &game, const SummonerProfile &profile)
{
    MatchSummary match;
    const QJsonObject participant = participantFor(game, profile);
    const QJsonObject stats = participant.value("stats").toObject();

    match.gameId = integerValue(game.value("gameId"));
    match.won = boolValue(stats.value("win"));
    match.gameMode = game.value("gameMode").toString();
    match.queueId = intValue(game.value("queueId"));
    match.mapId = intValue(game.value("mapId"));
    match.durationSeconds = intValue(game.value("gameDuration"));
    const qint64 creationTime = integerValue(game.value("gameCreationDate"));
    if (creationTime > 0) match.createdAt = QDateTime::fromMSecsSinceEpoch(creationTime);
    match.championId = intValue(participant.value("championId"));
    match.championLevel = intValue(stats.value("champLevel"));
    const QJsonObject timeline = participant.value("timeline").toObject();
    match.lane = timeline.value("lane").toString();
    match.role = timeline.value("role").toString();
    match.primaryRuneId = primaryRuneFor(stats);
    match.runeIds = runeIdsFor(stats);
    match.spell1Id = intValue(participant.value("spell1Id"));
    match.spell2Id = intValue(participant.value("spell2Id"));
    match.kills = intValue(stats.value("kills"));
    match.deaths = intValue(stats.value("deaths"));
    match.assists = intValue(stats.value("assists"));
    match.minionKills = intValue(stats.value("totalMinionsKilled")) + intValue(stats.value("neutralMinionsKilled"));
    match.goldEarned = intValue(stats.value("goldEarned"));
    match.itemIds = itemIdsFor(stats);
    return match;
}

struct PlayerIdentity {
    QString gameName;
    QString tagLine;
    QString puuid;
    QString summonerId;
    bool hiddenMatchHistory{};
    bool present{};
};

struct DetailPrivacyLookup {
    MatchDetail detail;
    qint64 gameId{};
    quint64 generation{};
    int remaining{};
};

QHash<int, PlayerIdentity> playerIdentitiesFor(const QJsonObject &game)
{
    QHash<int, PlayerIdentity> identities;
    const QJsonArray identityEntries = game.value("participantIdentities").toArray();
    const bool historyUriCanRepresentPrivacy = std::any_of(identityEntries.cbegin(), identityEntries.cend(), [](const QJsonValue &identityValue) {
        return !identityValue.toObject().value("player").toObject().value("matchHistoryUri").toString().trimmed().isEmpty();
    });
    for (const QJsonValue &identityValue : identityEntries) {
        const QJsonObject identity = identityValue.toObject();
        const int participantId = intValue(identity.value("participantId"));
        if (participantId <= 0) continue;
        const QJsonObject player = identity.value("player").toObject();
        PlayerIdentity playerIdentity;
        playerIdentity.present = !player.isEmpty();
        playerIdentity.gameName = player.value("gameName").toString();
        if (playerIdentity.gameName.isEmpty()) playerIdentity.gameName = player.value("summonerName").toString();
        playerIdentity.tagLine = player.value("tagLine").toString();
        playerIdentity.puuid = player.value("puuid").toString();
        playerIdentity.summonerId = stringValue(player.value("summonerId"));
        playerIdentity.hiddenMatchHistory = historyUriCanRepresentPrivacy && player.contains("matchHistoryUri")
            && player.value("matchHistoryUri").toString().trimmed().isEmpty();
        identities.insert(participantId, std::move(playerIdentity));
    }
    return identities;
}

MatchParticipant parseParticipant(const QJsonObject &participant, const QHash<int, PlayerIdentity> &identities)
{
    MatchParticipant parsed;
    const QJsonObject stats = participant.value("stats").toObject();
    parsed.participantId = intValue(participant.value("participantId"));
    parsed.teamId = intValue(participant.value("teamId"));
    parsed.championId = intValue(participant.value("championId"));
    parsed.championLevel = intValue(stats.value("champLevel"));
    const QJsonObject timeline = participant.value("timeline").toObject();
    parsed.lane = timeline.value("lane").toString();
    parsed.role = timeline.value("role").toString();
    parsed.primaryRuneId = primaryRuneFor(stats);
    parsed.runeIds = runeIdsFor(stats);
    parsed.spell1Id = intValue(participant.value("spell1Id"));
    parsed.spell2Id = intValue(participant.value("spell2Id"));
    const PlayerIdentity identity = identities.value(parsed.participantId);
    parsed.gameName = identity.gameName;
    parsed.tagLine = identity.tagLine;
    parsed.puuid = identity.puuid;
    parsed.summonerId = identity.summonerId;
    parsed.hiddenMatchHistory = identity.present && identity.hiddenMatchHistory;
    parsed.highestAchievedSeasonTier = participant.value("highestAchievedSeasonTier").toString();
    parsed.itemIds = itemIdsFor(stats);
    parsed.kills = intValue(stats.value("kills"));
    parsed.deaths = intValue(stats.value("deaths"));
    parsed.assists = intValue(stats.value("assists"));
    parsed.laneMinionKills = intValue(stats.value("totalMinionsKilled"));
    parsed.neutralMinionKills = intValue(stats.value("neutralMinionsKilled"));
    parsed.goldEarned = intValue(stats.value("goldEarned"));
    parsed.damageDealtToChampions = intValue(stats.value("totalDamageDealtToChampions"));
    parsed.damageTaken = intValue(stats.value("totalDamageTaken"));
    parsed.visionScore = intValue(stats.value("visionScore"));
    parsed.wardsPlaced = intValue(stats.value("wardsPlaced"));
    parsed.wardsKilled = intValue(stats.value("wardsKilled"));
    return parsed;
}

MatchTeam parseTeam(const QJsonObject &team)
{
    MatchTeam parsed;
    parsed.teamId = intValue(team.value("teamId"));
    parsed.won = boolValue(team.value("win"));
    parsed.towerKills = intValue(team.value("towerKills"));
    parsed.inhibitorKills = intValue(team.value("inhibitorKills"));
    parsed.dragonKills = intValue(team.value("dragonKills"));
    parsed.baronKills = intValue(team.value("baronKills"));
    parsed.riftHeraldKills = intValue(team.value("riftHeraldKills"));
    return parsed;
}

MatchDetail parseDetail(const QJsonObject &game, const MatchSummary &summary)
{
    MatchDetail detail;
    detail.gameId = integerValue(game.value("gameId"));
    if (detail.gameId == 0) detail.gameId = summary.gameId;
    detail.gameMode = game.value("gameMode").toString();
    if (detail.gameMode.isEmpty()) detail.gameMode = summary.gameMode;
    detail.queueId = intValue(game.value("queueId"));
    if (detail.queueId == 0) detail.queueId = summary.queueId;
    detail.mapId = intValue(game.value("mapId"));
    if (detail.mapId == 0) detail.mapId = summary.mapId;
    detail.durationSeconds = intValue(game.value("gameDuration"));
    if (detail.durationSeconds == 0) detail.durationSeconds = summary.durationSeconds;
    qint64 createdAt = integerValue(game.value("gameCreationDate"));
    if (createdAt == 0) createdAt = integerValue(game.value("gameCreation"));
    detail.createdAt = createdAt > 0 ? QDateTime::fromMSecsSinceEpoch(createdAt) : summary.createdAt;

    const QHash<int, PlayerIdentity> identities = playerIdentitiesFor(game);
    for (const QJsonValue &participantValue : game.value("participants").toArray()) {
        const MatchParticipant participant = parseParticipant(participantValue.toObject(), identities);
        if (participant.participantId > 0) detail.participants.append(participant);
    }
    for (const QJsonValue &teamValue : game.value("teams").toArray()) {
        const MatchTeam team = parseTeam(teamValue.toObject());
        if (team.teamId > 0) detail.teams.append(team);
    }

    std::sort(detail.participants.begin(), detail.participants.end(), [](const MatchParticipant &left, const MatchParticipant &right) {
        if (left.teamId != right.teamId) return left.teamId < right.teamId;
        return left.participantId < right.participantId;
    });
    return detail;
}

MatchSummary summaryForGame(const QList<MatchSummary> &matches, const qint64 gameId)
{
    for (const MatchSummary &match : matches) {
        if (match.gameId == gameId) return match;
    }
    MatchSummary summary;
    summary.gameId = gameId;
    return summary;
}

} // namespace

MatchRepository::MatchRepository(LcuClient &client, QObject *parent) : QObject(parent), client_(client) {}

const MatchDetail *MatchRepository::detail(const qint64 gameId) const
{
    const auto found = details_.constFind(gameId);
    return found == details_.cend() ? nullptr : &found.value();
}

void MatchRepository::refresh(const SummonerProfile &profile, int count)
{
    if (!profile.isValid()) return;
    count = qBound(1, count, 20);
    const quint64 generation = ++dataGeneration_;
    loading_ = true;
    // Keep the visible snapshot until the replacement has been parsed. This
    // avoids a blank match panel while changing to another player's profile.
    loadingDetailGameIds_.clear();
    emit loadingChanged(true, {});

    const QString encodedPuuid = QString::fromLatin1(QUrl::toPercentEncoding(profile.puuid));
    const QString path = "/lol-match-history/v1/products/lol/" + encodedPuuid
        + "/matches?begIndex=0&endIndex=" + QString::number(count - 1);
    const QPointer<MatchRepository> repository(this);
    client_.get(path, [repository, profile, generation](QJsonDocument document, QString error) {
        if (!repository || repository->dataGeneration_ != generation) return;
        repository->loading_ = false;
        if (!error.isEmpty()) {
            emit repository->loadingChanged(false, std::move(error));
            return;
        }

        const QJsonArray games = document.object().value("games").toObject().value("games").toArray();
        QList<MatchSummary> loaded;
        loaded.reserve(games.size());
        for (const QJsonValue &gameValue : games) {
            const MatchSummary match = parseMatch(gameValue.toObject(), profile);
            if (match.gameId != 0 && match.championId > 0) loaded.append(match);
        }
        repository->details_.clear();
        repository->detailErrors_.clear();
        repository->loadingDetailGameIds_.clear();
        repository->loadingDetailRankGameIds_.clear();
        repository->ranksByPuuid_.clear();
        repository->pendingRankCallbacks_.clear();
        repository->matches_ = std::move(loaded);
        emit repository->matchesChanged();
        emit repository->loadingChanged(false, {});
    });
}

void MatchRepository::loadDetail(const qint64 gameId)
{
    if (gameId == 0 || details_.contains(gameId) || loadingDetailGameIds_.contains(gameId)) return;

    const quint64 generation = dataGeneration_;
    const MatchSummary summary = summaryForGame(matches_, gameId);
    loadingDetailGameIds_.insert(gameId);
    detailErrors_.remove(gameId);
    emit matchDetailLoadingChanged(gameId, true, {});

    const QPointer<MatchRepository> repository(this);
    client_.get("/lol-match-history/v1/games/" + QString::number(gameId), [repository, gameId, generation, summary](QJsonDocument document, QString error) {
        if (!repository || repository->dataGeneration_ != generation) return;
        if (!error.isEmpty()) {
            repository->loadingDetailGameIds_.remove(gameId);
            repository->detailErrors_.insert(gameId, error);
            emit repository->matchDetailLoadingChanged(gameId, false, error);
            return;
        }

        MatchDetail loaded = parseDetail(document.object(), summary);
        if (!loaded.isValid()) {
            repository->loadingDetailGameIds_.remove(gameId);
            const QString message = "LCU 未返回完整对局记录。";
            repository->detailErrors_.insert(gameId, message);
            emit repository->matchDetailLoadingChanged(gameId, false, message);
            return;
        }

        repository->resolveDetailPrivacy(std::move(loaded), gameId, generation);
    });
}

void MatchRepository::resolveParticipantPrivacy(const QString &summonerId, PrivacyCallback callback)
{
    if (summonerId.isEmpty()) {
        callback(false, false);
        return;
    }
    const auto cached = privacyBySummonerId_.constFind(summonerId);
    if (cached != privacyBySummonerId_.cend()) {
        callback(true, cached.value());
        return;
    }
    auto pending = pendingPrivacyCallbacks_.find(summonerId);
    if (pending != pendingPrivacyCallbacks_.end()) {
        pending->append(std::move(callback));
        return;
    }

    QList<PrivacyCallback> callbacks;
    callbacks.append(std::move(callback));
    pendingPrivacyCallbacks_.insert(summonerId, std::move(callbacks));
    const QPointer<MatchRepository> repository(this);
    const QString encodedId = QString::fromLatin1(QUrl::toPercentEncoding(summonerId));
    client_.get("/lol-summoner/v1/summoners/" + encodedId, [repository, summonerId](QJsonDocument document, QString error) {
        if (!repository) return;
        const QString privacy = error.isEmpty()
            ? document.object().value("privacy").toString().trimmed()
            : QString();
        const bool known = privacy.compare("PRIVATE", Qt::CaseInsensitive) == 0
            || privacy.compare("PUBLIC", Qt::CaseInsensitive) == 0;
        const bool hidden = privacy.compare("PRIVATE", Qt::CaseInsensitive) == 0;
        if (known) repository->privacyBySummonerId_.insert(summonerId, hidden);

        const QList<PrivacyCallback> callbacks = repository->pendingPrivacyCallbacks_.take(summonerId);
        for (const PrivacyCallback &pendingCallback : callbacks) pendingCallback(known, hidden);
    });
}

void MatchRepository::resolveParticipantRank(const QString &puuid, RankCallback callback)
{
    if (puuid.isEmpty()) {
        callback({});
        return;
    }
    const auto cached = ranksByPuuid_.constFind(puuid);
    if (cached != ranksByPuuid_.cend()) {
        callback(cached.value());
        return;
    }
    auto pending = pendingRankCallbacks_.find(puuid);
    if (pending != pendingRankCallbacks_.end()) {
        pending->append(std::move(callback));
        return;
    }

    QList<RankCallback> callbacks;
    callbacks.append(std::move(callback));
    pendingRankCallbacks_.insert(puuid, std::move(callbacks));
    const QPointer<MatchRepository> repository(this);
    const QString encodedPuuid = QString::fromLatin1(QUrl::toPercentEncoding(puuid));
    client_.get("/lol-ranked/v1/ranked-stats/" + encodedPuuid, [repository, puuid](QJsonDocument document, QString error) {
        if (!repository) return;

        ParticipantRank rank;
        if (error.isEmpty()) {
            int selectedQueuePriority = 2;
            const QJsonObject queueMap = document.object().value("queueMap").toObject();
            for (auto entry = queueMap.constBegin(); entry != queueMap.constEnd(); ++entry) {
                const QJsonObject queue = entry.value().toObject();
                const QString queueType = queue.value("queueType").toString().isEmpty()
                    ? entry.key()
                    : queue.value("queueType").toString();
                if (queueType.compare("RANKED_SOLO_5x5", Qt::CaseInsensitive) != 0
                    && queueType.compare("RANKED_FLEX_SR", Qt::CaseInsensitive) != 0) {
                    continue;
                }

                const QString tier = normalizedRankTier(queue.value("tier").toString());
                if (tier.isEmpty()) continue;

                const int queuePriority = queueType.compare("RANKED_SOLO_5x5", Qt::CaseInsensitive) == 0 ? 0 : 1;
                if (queuePriority >= selectedQueuePriority) continue;
                rank.tier = tier;
                rank.division = queue.value("division").toString().trimmed();
                rank.leaguePoints = qMax(0, intValue(queue.value("leaguePoints")));
                selectedQueuePriority = queuePriority;
            }
            repository->ranksByPuuid_.insert(puuid, rank);
        }

        const QList<RankCallback> callbacks = repository->pendingRankCallbacks_.take(puuid);
        for (const RankCallback &pendingCallback : callbacks) pendingCallback(rank);
    });
}

void MatchRepository::resolveDetailPrivacy(MatchDetail detail, const qint64 gameId, const quint64 generation)
{
    auto lookup = std::make_shared<DetailPrivacyLookup>();
    lookup->detail = std::move(detail);
    lookup->gameId = gameId;
    lookup->generation = generation;
    lookup->remaining = lookup->detail.participants.size();
    const QPointer<MatchRepository> repository(this);
    const auto resolved = [repository, lookup](const int participantIndex, const bool known, const bool hidden) {
        if (!repository || repository->dataGeneration_ != lookup->generation) return;
        if (participantIndex >= 0 && participantIndex < lookup->detail.participants.size() && known) {
            lookup->detail.participants[participantIndex].hiddenMatchHistory = hidden;
        }
        if (--lookup->remaining == 0) repository->finishDetailLoad(lookup->gameId, std::move(lookup->detail), lookup->generation);
    };
    if (lookup->remaining == 0) {
        finishDetailLoad(gameId, std::move(lookup->detail), generation);
        return;
    }
    for (int index = 0; index < lookup->detail.participants.size(); ++index) {
        resolveParticipantPrivacy(lookup->detail.participants.at(index).summonerId,
                                  [resolved, index](const bool known, const bool hidden) {
                                      resolved(index, known, hidden);
                                  });
    }
}

void MatchRepository::enrichDetailRanks(const qint64 gameId, const quint64 generation)
{
    if (dataGeneration_ != generation || loadingDetailRankGameIds_.contains(gameId)) return;
    const auto detailIt = details_.constFind(gameId);
    if (detailIt == details_.cend() || detailIt->participants.isEmpty()) return;

    struct DetailRankLookup {
        qint64 gameId{};
        quint64 generation{};
        int remaining{};
        QList<ParticipantRank> ranks;
    };
    auto lookup = std::make_shared<DetailRankLookup>();
    lookup->gameId = gameId;
    lookup->generation = generation;
    lookup->remaining = detailIt->participants.size();
    lookup->ranks.resize(lookup->remaining);
    loadingDetailRankGameIds_.insert(gameId);

    const QPointer<MatchRepository> repository(this);
    const auto resolved = [repository, lookup](const int participantIndex, const ParticipantRank &rank) {
        if (!repository || repository->dataGeneration_ != lookup->generation) return;
        if (participantIndex >= 0 && participantIndex < lookup->ranks.size()) lookup->ranks[participantIndex] = rank;
        if (--lookup->remaining != 0) return;

        repository->loadingDetailRankGameIds_.remove(lookup->gameId);
        auto detail = repository->details_.find(lookup->gameId);
        if (detail == repository->details_.end()) return;
        for (int index = 0; index < detail->participants.size() && index < lookup->ranks.size(); ++index) {
            const ParticipantRank &resolvedRank = lookup->ranks.at(index);
            if (resolvedRank.tier.isEmpty()) continue;
            detail->participants[index].currentRankTier = resolvedRank.tier;
            detail->participants[index].currentRankDivision = resolvedRank.division;
            detail->participants[index].currentRankLeaguePoints = resolvedRank.leaguePoints;
        }
        emit repository->matchDetailChanged(lookup->gameId);
    };

    for (int index = 0; index < detailIt->participants.size(); ++index) {
        resolveParticipantRank(detailIt->participants.at(index).puuid,
                               [resolved, index](const ParticipantRank &rank) { resolved(index, rank); });
    }
}

void MatchRepository::finishDetailLoad(const qint64 gameId, MatchDetail detail, const quint64 generation)
{
    if (dataGeneration_ != generation) return;
    loadingDetailGameIds_.remove(gameId);
    details_.insert(gameId, std::move(detail));
    emit matchDetailChanged(gameId);
    emit matchDetailLoadingChanged(gameId, false, {});
    enrichDetailRanks(gameId, generation);
}

} // namespace Janna
