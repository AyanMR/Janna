#include "services/GameFlowRepository.h"

#include "services/LcuClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Janna {
namespace {

int intValue(const QJsonValue &value)
{
    bool valid = false;
    const int result = value.toVariant().toInt(&valid);
    return valid ? result : 0;
}

int teamIdValue(const QJsonValue &value)
{
    const int numeric = intValue(value);
    if (numeric != 0) return numeric;
    const QString text = value.toString().trimmed().toUpper();
    if (text == "BLUE" || text == "BLUE_SIDE" || text == "ALLY" || text == "ORDER" || text == "ONE") return 100;
    if (text == "RED" || text == "RED_SIDE" || text == "ENEMY" || text == "CHAOS" || text == "TWO") return 200;
    return 0;
}

qint64 longValue(const QJsonValue &value)
{
    bool valid = false;
    const qint64 result = value.toVariant().toLongLong(&valid);
    return valid ? result : 0;
}

QString idValue(const QJsonValue &value)
{
    if (value.isString()) {
        const QString text = value.toString().trimmed();
        bool numeric = false;
        const qint64 number = text.toLongLong(&numeric);
        return numeric && number <= 0 ? QString{} : text;
    }
    if (value.isDouble()) {
        const qint64 numeric = static_cast<qint64>(value.toDouble());
        return numeric > 0 ? QString::number(numeric) : QString{};
    }
    return {};
}

QDateTime dateTimeValue(const QJsonValue &value)
{
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (const char *key : {"milliseconds", "timestamp", "seconds", "value"}) {
            const QJsonValue nested = object.value(QLatin1String(key));
            if (!nested.isUndefined()) return dateTimeValue(nested);
        }
        return {};
    }
    if (value.isDouble()) {
        const double numeric = value.toDouble();
        if (numeric <= 0.0) return {};
        // LCU history payloads have used both Unix seconds and milliseconds.
        const qint64 timestamp = static_cast<qint64>(numeric);
        return timestamp < 100000000000LL
            ? QDateTime::fromSecsSinceEpoch(timestamp)
            : QDateTime::fromMSecsSinceEpoch(timestamp);
    }
    const QString text = value.toString().trimmed();
    if (text.isEmpty()) return {};
    bool numeric = false;
    const qint64 timestamp = text.toLongLong(&numeric);
    if (numeric && timestamp > 0) {
        return timestamp < 100000000000LL
            ? QDateTime::fromSecsSinceEpoch(timestamp)
            : QDateTime::fromMSecsSinceEpoch(timestamp);
    }
    QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!parsed.isValid()) parsed = QDateTime::fromString(text, Qt::ISODate);
    return parsed;
}

QDateTime firstDateTime(const QJsonObject &object, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        const QDateTime value = dateTimeValue(object.value(QLatin1String(key)));
        if (value.isValid()) return value;
    }
    return {};
}

QString firstText(const QJsonObject &object, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        const QString text = object.value(QLatin1String(key)).toString().trimmed();
        if (!text.isEmpty()) return text;
    }
    return {};
}

bool boolValue(const QJsonValue &value)
{
    if (value.isBool()) return value.toBool();
    if (value.isDouble()) return !qFuzzyIsNull(value.toDouble());
    const QString text = value.toString().trimmed();
    return text.compare("true", Qt::CaseInsensitive) == 0 || text == "1";
}

bool isEmptyJsonContainer(const QJsonValue &value)
{
    return value.isUndefined() || value.isNull()
        || (value.isArray() && value.toArray().isEmpty())
        || (value.isObject() && value.toObject().isEmpty());
}

void mergeNonEmptyFields(QJsonObject &target, const QJsonObject &fallback)
{
    for (auto it = fallback.constBegin(); it != fallback.constEnd(); ++it) {
        const QJsonValue current = target.value(it.key());
        if (!target.contains(it.key())
            || (isEmptyJsonContainer(current) && !isEmptyJsonContainer(it.value()))) {
            target.insert(it.key(), it.value());
        }
    }
}

QJsonArray playerEntriesFrom(const QJsonValue &value)
{
    if (value.isArray()) return value.toArray();

    const QJsonObject object = value.toObject();
    if (object.isEmpty()) return {};
    // Different LCU versions wrap the same team in one of these containers.
    // Keep the unwrapping here so both champ-select and in-game payloads use
    // exactly the same player parser.
    for (const char *key : {"players", "participants", "members", "team", "teamOne", "teamTwo",
                            "myTeam", "theirTeam", "blueTeam", "redTeam", "allPlayers", "playerList",
                            "playerChampionSelections", "gameData", "currentGame", "game", "data"}) {
        const QJsonValue nested = object.value(QLatin1String(key));
        if (nested.isArray()) return nested.toArray();
        if (nested.isObject()) {
            const QJsonArray entries = playerEntriesFrom(nested);
            if (!entries.isEmpty()) return entries;
        }
    }
    if (object.contains("championId") || object.contains("summonerId") || object.contains("cellId")
        || object.contains("puuid") || object.contains("gameName") || object.contains("isBot")
        || object.contains("bot") || object.contains("isAI")) {
        return QJsonArray{object};
    }
    // A few in-game payloads use a map keyed by player/cell id rather than a
    // JSON array.  Preserve those values as team entries as well.
    QJsonArray mappedPlayers;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!it.value().isObject()) continue;
        const QJsonObject candidate = it.value().toObject();
        if (candidate.contains("championId") || candidate.contains("summonerId")
            || candidate.contains("cellId") || candidate.contains("puuid")
            || candidate.contains("gameName") || candidate.contains("isBot")
            || candidate.contains("bot") || candidate.contains("isAI")) {
            mappedPlayers.append(candidate);
        }
    }
    if (!mappedPlayers.isEmpty()) return mappedPlayers;
    return {};
}

QJsonArray playersFrom(const QJsonValue &value)
{
    return playerEntriesFrom(value);
}

GameFlowPlayer playerFrom(const QJsonObject &object)
{
    GameFlowPlayer player;
    const QJsonObject riotId = object.value("riotId").toObject();
    const QJsonObject summoner = object.value("summoner").toObject();
    const QJsonObject customization = object.value("gameCustomizationData").toObject();
    player.cellId = intValue(object.value("cellId"));
    if (player.cellId == 0) player.cellId = intValue(object.value("actorCellId"));
    player.teamId = teamIdValue(object.value("teamId"));
    if (player.teamId == 0) player.teamId = teamIdValue(object.value("team"));
    if (player.teamId == 0) player.teamId = teamIdValue(object.value("teamType"));
    if (player.teamId == 0) player.teamId = teamIdValue(object.value("side"));
    player.championId = intValue(object.value("championId"));
    if (player.championId <= 0) player.championId = intValue(object.value("championID"));
    player.championPickIntent = intValue(object.value("championPickIntent"));
    player.spell1Id = intValue(object.value("spell1Id"));
    if (player.spell1Id <= 0) player.spell1Id = intValue(object.value("spell1ID"));
    if (player.spell1Id <= 0) player.spell1Id = intValue(object.value("summonerSpell1Id"));
    player.spell2Id = intValue(object.value("spell2Id"));
    if (player.spell2Id <= 0) player.spell2Id = intValue(object.value("spell2ID"));
    if (player.spell2Id <= 0) player.spell2Id = intValue(object.value("summonerSpell2Id"));
    player.summonerId = idValue(object.value("summonerId"));
    if (player.summonerId.isEmpty()) player.summonerId = idValue(object.value("summonerID"));
    if (player.summonerId.isEmpty()) player.summonerId = idValue(object.value("id"));
    if (player.summonerId.isEmpty()) player.summonerId = idValue(summoner.value("summonerId"));
    if (player.summonerId.isEmpty()) player.summonerId = idValue(summoner.value("id"));
    player.puuid = firstText(object, {"puuid", "playerPuuid", "playerUUID"});
    if (player.puuid.isEmpty()) player.puuid = firstText(summoner, {"puuid", "playerPuuid", "playerUUID"});
    player.gameName = firstText(object, {"gameName", "riotIdGameName", "summonerName", "displayName", "name"});
    if (player.gameName.isEmpty()) player.gameName = firstText(riotId, {"gameName", "riotIdGameName", "name"});
    if (player.gameName.isEmpty()) player.gameName = firstText(summoner, {"gameName", "summonerName", "displayName", "name"});
    player.tagLine = firstText(object, {"tagLine", "riotIdTagline", "riotIdTagLine"});
    if (player.tagLine.isEmpty()) player.tagLine = firstText(riotId, {"tagLine", "tagline", "riotIdTagline"});
    if (player.tagLine.isEmpty()) player.tagLine = firstText(summoner, {"tagLine", "riotIdTagline"});
    player.assignedPosition = firstText(object, {"assignedPosition", "selectedPosition", "position", "teamPosition", "lane", "role"});
    const QJsonObject stats = object.value("stats").toObject();
    const QJsonObject gameStats = object.value("gameStats").toObject();
    const QJsonObject scoreboard = object.value("scoreboard").toObject();
    const QJsonObject kda = object.value("kda").toObject();
    const auto readKdaValue = [&object, &stats, &gameStats, &scoreboard, &kda](const char *key, const char *fallbackKey) {
        for (const QJsonObject *source : {&object, &stats, &gameStats, &scoreboard, &kda}) {
            if (source->contains(QLatin1String(key))) return intValue(source->value(QLatin1String(key)));
            if (fallbackKey != nullptr && source->contains(QLatin1String(fallbackKey))) {
                return intValue(source->value(QLatin1String(fallbackKey)));
            }
        }
        return 0;
    };
    player.kills = readKdaValue("kills", "killCount");
    player.deaths = readKdaValue("deaths", "deathCount");
    player.assists = readKdaValue("assists", "assistCount");
    const auto hasKdaField = [&object, &stats, &gameStats, &scoreboard, &kda](const char *key, const char *fallbackKey) {
        for (const QJsonObject *source : {&object, &stats, &gameStats, &scoreboard, &kda}) {
            if (source->contains(QLatin1String(key)) || source->contains(QLatin1String(fallbackKey))) return true;
        }
        return false;
    };
    player.currentKdaAvailable = hasKdaField("kills", "killCount")
        || hasKdaField("deaths", "deathCount") || hasKdaField("assists", "assistCount");
    player.isBot = boolValue(object.value("isBot")) || boolValue(object.value("bot"))
        || boolValue(object.value("isAI")) || boolValue(object.value("isComputer"))
        || boolValue(object.value("botPlayer")) || boolValue(summoner.value("isBot"))
        || boolValue(summoner.value("isAI")) || boolValue(customization.value("isBot"))
        || boolValue(customization.value("bot"));
    QString playerType = firstText(object, {"playerType", "type", "participantType"});
    if (playerType.isEmpty()) playerType = firstText(summoner, {"playerType", "type", "participantType"});
    if (playerType.compare("BOT", Qt::CaseInsensitive) == 0
        || playerType.compare("AI", Qt::CaseInsensitive) == 0
        || playerType.compare("COMPUTER", Qt::CaseInsensitive) == 0) {
        player.isBot = true;
    }
    player.isLocalPlayer = boolValue(object.value("isPlayer"))
        || boolValue(object.value("isLocalPlayer")) || boolValue(object.value("isMe"))
        || boolValue(object.value("isLocal")) || boolValue(object.value("isSelf"))
        || boolValue(object.value("isCurrentPlayer"));
    return player;
}

QList<GameFlowPlayer> playersFrom(const QJsonArray &entries)
{
    QList<GameFlowPlayer> players;
    players.reserve(entries.size());
    for (const QJsonValue &entry : entries) {
        const QJsonObject object = entry.toObject();
        if (object.isEmpty()) continue;
        players.append(playerFrom(object));
    }
    std::stable_sort(players.begin(), players.end(), [](const GameFlowPlayer &left, const GameFlowPlayer &right) {
        return left.cellId < right.cellId;
    });
    return players;
}

void splitParticipantEntries(const QJsonArray &entries, QList<GameFlowPlayer> &myTeam,
                             QList<GameFlowPlayer> &theirTeam)
{
    const QList<GameFlowPlayer> players = playersFrom(entries);
    if (players.isEmpty()) return;

    int localTeamId = 0;
    for (const GameFlowPlayer &player : players) {
        if (player.isLocalPlayer && player.teamId != 0) {
            localTeamId = player.teamId;
            break;
        }
    }

    int firstTeamId = 0;
    int secondTeamId = 0;
    for (const GameFlowPlayer &player : players) {
        if (player.teamId == 0) continue;
        if (firstTeamId == 0) {
            firstTeamId = player.teamId;
        } else if (player.teamId != firstTeamId && secondTeamId == 0) {
            secondTeamId = player.teamId;
        }
    }
    if (localTeamId == 0) localTeamId = firstTeamId;

    for (int index = 0; index < players.size(); ++index) {
        const GameFlowPlayer &player = players.at(index);
        bool enemy = false;
        if (localTeamId != 0 && player.teamId != 0) {
            enemy = player.teamId != localTeamId;
        } else if (secondTeamId != 0 && player.teamId != 0) {
            enemy = player.teamId == secondTeamId;
        } else {
            // A few payloads omit teamId entirely but retain the standard
            // five-player ordering.
            enemy = index >= 5;
        }
        (enemy ? theirTeam : myTeam).append(player);
    }
}

QList<int> championIdsFrom(const QJsonValue &value);
void appendChampionId(QList<int> &target, int championId);

void appendSpectatorBans(const QJsonValue &value, QList<int> &myBans, QList<int> &theirBans)
{
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        if (object.contains("myTeamBans") || object.contains("theirTeamBans")) {
            for (const int id : championIdsFrom(object.value("myTeamBans"))) appendChampionId(myBans, id);
            for (const int id : championIdsFrom(object.value("theirTeamBans"))) appendChampionId(theirBans, id);
            return;
        }
        if (object.contains("championId") || object.contains("championID")) {
            const int championId = intValue(object.value("championId"));
            const int normalizedId = championId > 0 ? championId : intValue(object.value("championID"));
            if (normalizedId <= 0) return;
            int teamId = teamIdValue(object.value("teamId"));
            if (teamId == 0) teamId = teamIdValue(object.value("team"));
            if (teamId == 200 || teamId == 2) appendChampionId(theirBans, normalizedId);
            else appendChampionId(myBans, normalizedId);
            return;
        }
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            appendSpectatorBans(it.value(), myBans, theirBans);
        }
        return;
    }
    if (!value.isArray()) return;
    for (const QJsonValue &entry : value.toArray()) appendSpectatorBans(entry, myBans, theirBans);
}

QList<int> championIdsFrom(const QJsonValue &value)
{
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (const char *key : {"bans", "champions", "myTeamBans", "theirTeamBans"}) {
            const QJsonValue nested = object.value(QLatin1String(key));
            if (!nested.isUndefined()) return championIdsFrom(nested);
        }
    }
    QList<int> championIds;
    for (const QJsonValue &entry : value.toArray()) {
        const int championId = entry.isObject()
            ? intValue(entry.toObject().value("championId"))
            : intValue(entry);
        if (championId > 0 && !championIds.contains(championId)) championIds.append(championId);
    }
    return championIds;
}

QList<GameFlowMatch> historyMatchesFrom(const QJsonDocument &document, const QString &puuid)
{
    QJsonArray games;
    if (document.isArray()) games = document.array();
    else {
        const QJsonObject root = document.object();
        games = root.value("games").toObject().value("games").toArray();
        if (games.isEmpty()) games = root.value("games").toArray();
    }

    QList<GameFlowMatch> matches;
    matches.reserve(qMin(10, games.size()));
    for (const QJsonValue &gameValue : games) {
        const QJsonObject game = gameValue.toObject();
        const QJsonArray identities = game.value("participantIdentities").toArray();
        int participantId = 0;
        for (const QJsonValue &identityValue : identities) {
            const QJsonObject identity = identityValue.toObject();
            const QJsonObject player = identity.value("player").toObject();
            if (!puuid.isEmpty() && player.value("puuid").toString().compare(puuid, Qt::CaseInsensitive) == 0) {
                participantId = intValue(identity.value("participantId"));
                break;
            }
        }
        const QJsonArray participants = game.value("participants").toArray();
        QJsonObject participant;
        for (const QJsonValue &participantValue : participants) {
            const QJsonObject candidate = participantValue.toObject();
            if (participantId > 0 && intValue(candidate.value("participantId")) == participantId) {
                participant = candidate;
                break;
            }
            if (participantId == 0 && !puuid.isEmpty()
                && candidate.value("puuid").toString().compare(puuid, Qt::CaseInsensitive) == 0) {
                participant = candidate;
                break;
            }
        }
        // Only use the first participant when the caller did not provide an
        // identity.  Guessing here makes every teammate display someone else's
        // KDA when the LCU redacts participant identities.
        if (participant.isEmpty() && puuid.isEmpty() && !participants.isEmpty()) {
            participant = participants.at(0).toObject();
        }
        if (participant.isEmpty()) continue;
        const QJsonObject stats = participant.value("stats").toObject();
        GameFlowMatch match;
        match.gameId = longValue(game.value("gameId"));
        if (match.gameId == 0) match.gameId = longValue(game.value("gameIdLong"));
        match.championId = intValue(participant.value("championId"));
        match.kills = intValue(stats.value("kills"));
        match.deaths = intValue(stats.value("deaths"));
        match.assists = intValue(stats.value("assists"));
        match.won = boolValue(stats.value("win"));
        match.queueId = intValue(game.value("queueId"));
        if (match.queueId <= 0) match.queueId = intValue(game.value("gameQueueConfigId"));
        const QJsonObject queue = game.value("queue").toObject();
        if (match.queueId <= 0) match.queueId = intValue(queue.value("id"));
        match.gameMode = firstText(game, {"gameMode", "gameType", "mode"});
        match.queueName = firstText(game, {"queueName", "queueDescription", "queueType"});
        if (match.queueName.isEmpty()) match.queueName = firstText(queue, {"name", "description", "queueType"});
        match.createdAt = firstDateTime(game, {"gameCreationDate", "gameCreation", "gameStartTime", "createdAt"});
        match.durationSeconds = intValue(game.value("gameDuration"));
        if (match.durationSeconds <= 0) match.durationSeconds = intValue(game.value("durationSeconds"));
        if (match.championId > 0) matches.append(match);
        if (matches.size() >= 10) break;
    }
    return matches;
}

GameFlowRank rankFrom(const QJsonDocument &document)
{
    GameFlowRank rank;
    const QJsonObject root = document.object();
    const QJsonObject queueMap = root.value("queueMap").toObject();
    int selectedPriority = std::numeric_limits<int>::max();
    const auto considerQueue = [&rank, &selectedPriority](const QJsonObject &queue, const QString &fallbackType) {
        const QString queueType = queue.value("queueType").toString().isEmpty()
            ? fallbackType : queue.value("queueType").toString();
        if (queueType.compare("RANKED_SOLO_5x5", Qt::CaseInsensitive) != 0
            && queueType.compare("RANKED_FLEX_SR", Qt::CaseInsensitive) != 0) return;
        QString tier = queue.value("tier").toString().trimmed();
        if (tier.isEmpty()) tier = queue.value("currentTier").toString().trimmed();
        if (tier.isEmpty() || tier.compare("NONE", Qt::CaseInsensitive) == 0
            || tier.compare("UNRANKED", Qt::CaseInsensitive) == 0) return;
        const int priority = queueType.compare("RANKED_SOLO_5x5", Qt::CaseInsensitive) == 0 ? 0 : 1;
        if (priority >= selectedPriority) return;
        rank.tier = tier;
        rank.division = queue.value("division").toString().trimmed();
        if (rank.division.isEmpty()) rank.division = queue.value("rank").toString().trimmed();
        rank.leaguePoints = qMax(0, intValue(queue.value("leaguePoints")));
        selectedPriority = priority;
    };
    for (auto it = queueMap.constBegin(); it != queueMap.constEnd(); ++it) {
        considerQueue(it.value().toObject(), it.key());
    }
    // Some client builds expose the same records as a `queues` array instead
    // of a queueMap object.
    for (const QJsonValue &value : root.value("queues").toArray()) {
        considerQueue(value.toObject(), {});
    }
    return rank;
}

void appendChampionId(QList<int> &target, const int championId)
{
    if (championId > 0 && !target.contains(championId)) target.append(championId);
}

void applyPickActions(QList<GameFlowPlayer> &players, const QJsonArray &actions)
{
    for (const QJsonValue &turnValue : actions) {
        const QJsonArray turn = turnValue.isArray() ? turnValue.toArray() : QJsonArray{turnValue};
        for (const QJsonValue &actionValue : turn) {
            const QJsonObject action = actionValue.toObject();
            if (action.value("type").toString().compare("pick", Qt::CaseInsensitive) != 0) continue;
            const int actorCellId = intValue(action.value("actorCellId"));
            const int championId = intValue(action.value("championId"));
            for (GameFlowPlayer &player : players) {
                if (player.cellId != actorCellId) continue;
                if (championId > 0) player.championId = championId;
                player.pickCompleted = boolValue(action.value("completed"));
                player.pickInProgress = boolValue(action.value("isInProgress"));
                break;
            }
        }
    }
}

void appendActionBans(QList<int> &myBans, QList<int> &theirBans, const QJsonArray &actions,
                      const QList<GameFlowPlayer> &myTeam)
{
    QSet<int> myCellIds;
    for (const GameFlowPlayer &player : myTeam) myCellIds.insert(player.cellId);

    for (const QJsonValue &turnValue : actions) {
        const QJsonArray turn = turnValue.isArray() ? turnValue.toArray() : QJsonArray{turnValue};
        for (const QJsonValue &actionValue : turn) {
            const QJsonObject action = actionValue.toObject();
            if (action.value("type").toString().compare("ban", Qt::CaseInsensitive) != 0) continue;
            const int championId = intValue(action.value("championId"));
            if (championId <= 0) continue;
            const int actorCellId = intValue(action.value("actorCellId"));
            const bool isAlly = action.contains("isAllyAction")
                ? action.value("isAllyAction").toBool()
                : myCellIds.contains(actorCellId);
            appendChampionId(isAlly ? myBans : theirBans, championId);
        }
    }
}

bool samePlayer(const GameFlowPlayer &left, const GameFlowPlayer &right)
{
    if (left.recentMatches.size() != right.recentMatches.size()) return false;
    for (int index = 0; index < left.recentMatches.size(); ++index) {
        const GameFlowMatch &leftMatch = left.recentMatches.at(index);
        const GameFlowMatch &rightMatch = right.recentMatches.at(index);
        if (leftMatch.gameId != rightMatch.gameId || leftMatch.championId != rightMatch.championId
            || leftMatch.kills != rightMatch.kills || leftMatch.deaths != rightMatch.deaths
            || leftMatch.assists != rightMatch.assists || leftMatch.won != rightMatch.won
            || leftMatch.queueId != rightMatch.queueId || leftMatch.gameMode != rightMatch.gameMode
            || leftMatch.queueName != rightMatch.queueName || leftMatch.createdAt != rightMatch.createdAt
            || leftMatch.durationSeconds != rightMatch.durationSeconds) return false;
    }
    return left.cellId == right.cellId
        && left.teamId == right.teamId
        && left.championId == right.championId
        && left.championPickIntent == right.championPickIntent
        && left.spell1Id == right.spell1Id
        && left.spell2Id == right.spell2Id
        && left.summonerId == right.summonerId
        && left.puuid == right.puuid
        && left.gameName == right.gameName
        && left.tagLine == right.tagLine
        && left.assignedPosition == right.assignedPosition
        && left.isLocalPlayer == right.isLocalPlayer
        && left.isBot == right.isBot
        && left.kills == right.kills
        && left.deaths == right.deaths
        && left.assists == right.assists
        && left.currentKdaAvailable == right.currentKdaAvailable
        && left.pickCompleted == right.pickCompleted
        && left.pickInProgress == right.pickInProgress
        && left.rank.tier == right.rank.tier
        && left.rank.division == right.rank.division
        && left.rank.leaguePoints == right.rank.leaguePoints
        && left.recentMatchesLoading == right.recentMatchesLoading;
}

bool sameTeam(const QList<GameFlowPlayer> &left, const QList<GameFlowPlayer> &right)
{
    if (left.size() != right.size()) return false;
    for (int index = 0; index < left.size(); ++index) {
        if (!samePlayer(left.at(index), right.at(index))) return false;
    }
    return true;
}

bool sameSnapshotContent(const GameFlowSnapshot &left, const GameFlowSnapshot &right)
{
    return left.phase == right.phase
        && left.gameId == right.gameId
        && left.queueId == right.queueId
        && left.queueName == right.queueName
        && left.mapId == right.mapId
        && left.mapName == right.mapName
        && left.gameMode == right.gameMode
        && (left.secondsRemaining >= 0) == (right.secondsRemaining >= 0)
        && left.timerCountsDown == right.timerCountsDown
        && left.gameStartTime == right.gameStartTime
        && sameTeam(left.myTeam, right.myTeam)
        && sameTeam(left.theirTeam, right.theirTeam)
        && left.myBans == right.myBans
        && left.theirBans == right.theirBans;
}

bool canReuseTeams(const GameFlowSnapshot &previous, const GameFlowSnapshot &next)
{
    if (!previous.isActive() || previous.myTeam.isEmpty()) return false;
    return previous.gameId == 0 || next.gameId == 0 || previous.gameId == next.gameId;
}

void reuseMissingTeams(const GameFlowSnapshot &previous, GameFlowSnapshot &next)
{
    if (!canReuseTeams(previous, next)) return;
    if (next.myTeam.isEmpty()) next.myTeam = previous.myTeam;
    if (next.theirTeam.isEmpty()) next.theirTeam = previous.theirTeam;
    if (next.myBans.isEmpty()) next.myBans = previous.myBans;
    if (next.theirBans.isEmpty()) next.theirBans = previous.theirBans;

    // During the same live phase the session endpoint may omit metadata while
    // the game process is taking over.  Keep the authoritative spectator
    // values instead of briefly resetting the page to an empty queue/game id.
    if (previous.phase.compare(next.phase, Qt::CaseInsensitive) == 0) {
        if (next.gameId == 0) next.gameId = previous.gameId;
        if (next.queueId == 0) next.queueId = previous.queueId;
        if (next.queueName.isEmpty()) next.queueName = previous.queueName;
        if (next.mapId == 0) next.mapId = previous.mapId;
        if (next.mapName.isEmpty()) next.mapName = previous.mapName;
        if (next.gameMode.isEmpty()) next.gameMode = previous.gameMode;
        if (next.secondsRemaining < 0) next.secondsRemaining = previous.secondsRemaining;
        next.timerCountsDown = previous.timerCountsDown;
        if (!next.gameStartTime.isValid()) next.gameStartTime = previous.gameStartTime;
    }
}

bool sameId(const QString &left, const QString &right)
{
    return !left.trimmed().isEmpty() && !right.trimmed().isEmpty()
        && left.trimmed().compare(right.trimmed(), Qt::CaseInsensitive) == 0;
}

void normalizeLocalTeam(GameFlowSnapshot &snapshot, const QString &summonerId, const QString &puuid)
{
    for (QList<GameFlowPlayer> *team : {&snapshot.myTeam, &snapshot.theirTeam}) {
        for (GameFlowPlayer &player : *team) {
            if (sameId(player.summonerId, summonerId) || sameId(player.puuid, puuid)) {
                player.isLocalPlayer = true;
            }
        }
    }

    const auto hasLocalPlayer = [](const QList<GameFlowPlayer> &players) {
        return std::any_of(players.cbegin(), players.cend(), [](const GameFlowPlayer &player) {
            return player.isLocalPlayer;
        });
    };
    if (!hasLocalPlayer(snapshot.myTeam) && hasLocalPlayer(snapshot.theirTeam)) {
        std::swap(snapshot.myTeam, snapshot.theirTeam);
        std::swap(snapshot.myBans, snapshot.theirBans);
    }
}

bool isLiveGamePhase(const QString &phase)
{
    return phase.compare("GameStart", Qt::CaseInsensitive) == 0
        || phase.compare("InProgress", Qt::CaseInsensitive) == 0
        || phase.compare("Reconnect", Qt::CaseInsensitive) == 0;
}

bool hasSpectatorParticipants(const GameFlowSnapshot &snapshot)
{
    return !snapshot.myTeam.isEmpty() || !snapshot.theirTeam.isEmpty();
}

int teamDataQuality(const QList<GameFlowPlayer> &players)
{
    int quality = 0;
    for (const GameFlowPlayer &player : players) {
        if (player.championId > 0) quality += 4;
        if (!player.summonerId.isEmpty() || !player.puuid.isEmpty()) quality += 2;
        if (!player.gameName.isEmpty()) quality += 1;
    }
    return quality;
}

bool hasUsableRoster(const GameFlowSnapshot &snapshot)
{
    if (snapshot.myTeam.isEmpty() || snapshot.theirTeam.isEmpty()) return false;
    for (const QList<GameFlowPlayer> *team : {&snapshot.myTeam, &snapshot.theirTeam}) {
        for (const GameFlowPlayer &player : *team) {
            if (player.championId <= 0 && player.summonerId.isEmpty()
                && player.puuid.isEmpty() && player.gameName.isEmpty()) return false;
        }
    }
    return true;
}

bool hasCompleteLiveRoster(const GameFlowSnapshot &snapshot)
{
    if (snapshot.myTeam.isEmpty() || snapshot.theirTeam.isEmpty()) return false;
    for (const QList<GameFlowPlayer> *team : {&snapshot.myTeam, &snapshot.theirTeam}) {
        for (const GameFlowPlayer &player : *team) {
            if (player.championId <= 0) return false;
        }
    }
    return true;
}

} // namespace

bool GameFlowSnapshot::isActive() const
{
    return phase.compare("ReadyCheck", Qt::CaseInsensitive) == 0
        || phase.compare("ChampSelect", Qt::CaseInsensitive) == 0
        || phase.compare("GameStart", Qt::CaseInsensitive) == 0
        || phase.compare("InProgress", Qt::CaseInsensitive) == 0
        || phase.compare("Reconnect", Qt::CaseInsensitive) == 0
        || phase.compare("WaitingForStats", Qt::CaseInsensitive) == 0
        || phase.compare("PreEndOfGame", Qt::CaseInsensitive) == 0
        || phase.compare("EndOfGame", Qt::CaseInsensitive) == 0;
}

GameFlowRepository::GameFlowRepository(QObject *parent)
    : QObject(parent), client_(std::make_unique<LcuClient>()), pollTimer_(new QTimer(this))
{
    pollTimer_->setInterval(1000);
    connect(pollTimer_, &QTimer::timeout, this, &GameFlowRepository::poll);
    clockTimer_ = new QTimer(this);
    clockTimer_->setInterval(250);
    connect(clockTimer_, &QTimer::timeout, this, &GameFlowRepository::updateLocalClock);
}

GameFlowRepository::~GameFlowRepository() = default;

void GameFlowRepository::setClientAvailable(const bool available)
{
    if (clientAvailable_ == available) return;

    clientAvailable_ = available;
    ++generation_;
    requestInFlight_ = false;
    if (!available) {
        pollTimer_->stop();
        pendingIdentityIds_.clear();
        pendingHistoryIds_.clear();
        pendingRankIds_.clear();
        unavailableIdentityIds_.clear();
        identities_.clear();
        recentMatchesBySummonerId_.clear();
        ranksBySummonerId_.clear();
        currentSummonerId_.clear();
        currentPuuid_.clear();
        currentPlayerRequestInFlight_ = false;
        spectatorRosterLoaded_ = false;
        spectatorRosterPhase_.clear();
        missingSessionPolls_ = 0;
        clockTimer_->stop();
        clockInitialized_ = false;
        clockAnchorTime_ = {};
        clockAnchorSeconds_ = -1;
        clockPhase_.clear();
        clockGameId_ = 0;
        applySnapshot({});
        lastActivityState_ = false;
        lastObservedPhase_.clear();
        setLoading(false);
        return;
    }

    pollTimer_->start();
    clockTimer_->start();
    requestCurrentPlayer();
    poll();
}

void GameFlowRepository::refresh()
{
    poll();
}

void GameFlowRepository::poll()
{
    if (!clientAvailable_ || requestInFlight_) return;
    if (currentSummonerId_.isEmpty() && currentPuuid_.isEmpty()) requestCurrentPlayer();
    requestInFlight_ = true;
    setLoading(true);
    const quint64 requestGeneration = generation_;
    const QPointer<GameFlowRepository> repository(this);
    client_->get("/lol-gameflow/v1/session", [repository, requestGeneration](QJsonDocument document, QString error) {
        if (repository) repository->handleGameFlowSession(std::move(document), std::move(error), requestGeneration);
    });
}

void GameFlowRepository::handleGameFlowSession(QJsonDocument document, QString error, const quint64 requestGeneration)
{
    if (requestGeneration != generation_ || !clientAvailable_) return;
    if (!error.isEmpty() || !document.isObject()) {
        // The session endpoint can briefly disappear while the game client is
        // taking over.  The phase endpoint remains available in that window,
        // so use it to keep automatic navigation and the last team snapshot
        // alive instead of treating the response as an immediate lobby state.
        requestPhaseFallback({}, error.isEmpty() ? "LCU 未返回游戏流程数据。" : error, requestGeneration);
        return;
    }
    missingSessionPolls_ = 0;

    const GameFlowSnapshot parsed = parse(document);
    if (parsed.phase.trimmed().isEmpty()) {
        requestPhaseFallback(std::move(document), {}, requestGeneration);
        return;
    }
    if (parsed.phase.compare("ChampSelect", Qt::CaseInsensitive) == 0) {
        // A new queue can begin directly after the previous game.  Do not
        // let the old spectator roster suppress the first fetch for it.
        spectatorRosterLoaded_ = false;
        spectatorRosterPhase_.clear();
    }
    if (parsed.phase.compare("ChampSelect", Qt::CaseInsensitive) != 0) {
        if (isLiveGamePhase(parsed.phase)) {
            // During the hand-off to the game process gameflow/session often
            // contains only the phase and queue metadata.  Even when it has
            // two placeholder team arrays, the spectator endpoint is the
            // authoritative source for participants once the client has
            // entered GameStart/InProgress.  Fetch it once per live phase and
            // retry only while the session still lacks a usable roster.  This
            // avoids starving the per-player rank/history requests while
            // still covering the short game-process hand-off.
            const bool phaseChanged = spectatorRosterPhase_.compare(parsed.phase, Qt::CaseInsensitive) != 0;
            const bool rosterAvailable = hasUsableRoster(parsed)
                || (spectatorRosterLoaded_ && hasUsableRoster(snapshot_));
            if (!spectatorRosterLoaded_ || phaseChanged || !rosterAvailable) {
                requestSpectatorGame(std::move(document), requestGeneration);
                return;
            }
        } else {
            spectatorRosterLoaded_ = false;
            spectatorRosterPhase_.clear();
        }
        requestInFlight_ = false;
        GameFlowSnapshot next = parsed;
        reuseMissingTeams(snapshot_, next);
        applySnapshot(std::move(next));
        setLoading(false);
        return;
    }

    const QPointer<GameFlowRepository> repository(this);
    client_->get("/lol-champ-select/v1/session", [repository, document = std::move(document), requestGeneration](QJsonDocument champSelect, QString champSelectError) mutable {
        if (repository) repository->handleChampSelectSession(std::move(document), std::move(champSelect),
                                                              std::move(champSelectError), requestGeneration);
    });
}

void GameFlowRepository::requestPhaseFallback(QJsonDocument document, QString sessionError,
                                              const quint64 requestGeneration)
{
    const QPointer<GameFlowRepository> repository(this);
    client_->getBytes("/lol-gameflow/v1/gameflow-phase",
                      [repository, document = std::move(document), sessionError = std::move(sessionError), requestGeneration]
                      (QByteArray bytes, QString phaseError) mutable {
        if (!repository || requestGeneration != repository->generation_ || !repository->clientAvailable_) return;

        QString phase;
        const QJsonDocument phaseDocument = QJsonDocument::fromJson(bytes);
        if (phaseDocument.isObject()) phase = phaseDocument.object().value("phase").toString().trimmed();
        if (phase.isEmpty()) phase = QString::fromUtf8(bytes).trimmed();
        if (phase.size() >= 2 && phase.startsWith('"') && phase.endsWith('"')) {
            phase = phase.mid(1, phase.size() - 2).trimmed();
        }
        if (phase.isEmpty() || !phaseError.isEmpty()) {
            repository->requestInFlight_ = false;
            const QString combinedError = !phaseError.isEmpty() ? phaseError : sessionError;
            if (combinedError.contains("HTTP 404", Qt::CaseInsensitive)) {
                // Two consecutive misses avoid flashing the overview page for
                // the short hand-off between the client and the game process.
                if (++repository->missingSessionPolls_ >= 2) repository->applySnapshot({});
            }
            repository->setLoading(false, combinedError);
            return;
        }

        repository->missingSessionPolls_ = 0;
        QJsonObject root = document.object();
        root.insert("phase", phase);
        document.setObject(root);
        repository->handleGameFlowSession(std::move(document), {}, requestGeneration);
    });
}

void GameFlowRepository::handleChampSelectSession(QJsonDocument gameFlowDocument, QJsonDocument document,
                                                   QString error, const quint64 requestGeneration)
{
    if (requestGeneration != generation_ || !clientAvailable_) return;
    requestInFlight_ = false;
    GameFlowSnapshot next = parse(gameFlowDocument, error.isEmpty() ? document : QJsonDocument{});
    reuseMissingTeams(snapshot_, next);
    applySnapshot(std::move(next));
    setLoading(false, error);
}

void GameFlowRepository::requestSpectatorGame(QJsonDocument gameFlowDocument, const quint64 requestGeneration)
{
    const QPointer<GameFlowRepository> repository(this);
    client_->get("/lol-spectator/v1/current-game",
                 [repository, gameFlowDocument = std::move(gameFlowDocument), requestGeneration]
                 (QJsonDocument document, QString error) mutable {
        if (repository) repository->handleSpectatorGame(std::move(gameFlowDocument), std::move(document),
                                                        std::move(error), requestGeneration);
    });
}

void GameFlowRepository::handleSpectatorGame(QJsonDocument gameFlowDocument, QJsonDocument document,
                                             QString error, const quint64 requestGeneration)
{
    if (requestGeneration != generation_ || !clientAvailable_) return;
    requestInFlight_ = false;

    GameFlowSnapshot next = parse(gameFlowDocument);
    if (error.isEmpty() && document.isObject()) {
        const GameFlowSnapshot enriched = parse(gameFlowDocument, QJsonDocument(), document);
        // `/lol-gameflow/v1/session` can briefly retain the previous game id
        // while `/lol-spectator/v1/current-game` already exposes the new one.
        // The spectator roster is authoritative in a live game; do not throw
        // it away solely because those transitional ids differ.  Preserve
        // session-side teams/bans when the endpoint returns only metadata.
        if (hasSpectatorParticipants(enriched)) {
            if (enriched.gameId > 0) next.gameId = enriched.gameId;
            if (enriched.queueId > 0) next.queueId = enriched.queueId;
            if (!enriched.queueName.isEmpty()) next.queueName = enriched.queueName;
            if (enriched.mapId > 0) next.mapId = enriched.mapId;
            if (!enriched.mapName.isEmpty()) next.mapName = enriched.mapName;
            if (!enriched.gameMode.isEmpty()) next.gameMode = enriched.gameMode;
            if (!enriched.myTeam.isEmpty()) next.myTeam = enriched.myTeam;
            if (!enriched.theirTeam.isEmpty()) next.theirTeam = enriched.theirTeam;
            if (!enriched.myBans.isEmpty()) next.myBans = enriched.myBans;
            if (!enriched.theirBans.isEmpty()) next.theirBans = enriched.theirBans;
        }
    }
    reuseMissingTeams(snapshot_, next);
    // A failed/empty spectator response must not permanently mark a pair of
    // placeholder session teams as loaded.  Keep retrying until every live
    // player has a champion, while still accepting a complete session roster
    // when the spectator endpoint is unavailable.
    spectatorRosterLoaded_ = hasCompleteLiveRoster(next);
    spectatorRosterPhase_ = next.phase;
    applySnapshot(std::move(next));
    setLoading(false, error);
}

void GameFlowRepository::applySnapshot(GameFlowSnapshot next)
{
    const GameFlowSnapshot previousSnapshot = snapshot_;

    for (QList<GameFlowPlayer> *team : {&next.myTeam, &next.theirTeam}) {
        for (GameFlowPlayer &player : *team) {
            const auto identity = identities_.constFind(player.summonerId);
            if (identity != identities_.cend()) {
                if (player.gameName.isEmpty()) player.gameName = identity->gameName;
                if (player.tagLine.isEmpty()) player.tagLine = identity->tagLine;
                if (player.puuid.isEmpty()) player.puuid = identity->puuid;
            }
            if (player.isBot) continue;
            const QString playerKey = !player.summonerId.isEmpty() ? player.summonerId : player.puuid;
            if (!playerKey.isEmpty()) {
                const auto matches = recentMatchesBySummonerId_.constFind(playerKey);
                if (matches != recentMatchesBySummonerId_.cend()) {
                    player.recentMatches = matches.value();
                    player.recentMatchesLoading = false;
                } else {
                    player.recentMatchesLoading = pendingHistoryIds_.contains(playerKey);
                }
                const auto rank = ranksBySummonerId_.constFind(playerKey);
                if (rank != ranksBySummonerId_.cend()) player.rank = rank.value();
            }
        }
    }

    // The spectator payload normally has no explicit local-player flag.  Use
    // the current summoner identity to orient the two teams and keep the
    // local side stable when the client is playing on red side.
    normalizeLocalTeam(next, currentSummonerId_, currentPuuid_);

    const bool nextHasClock = next.isActive() && next.secondsRemaining >= 0;
    const QDateTime now = QDateTime::currentDateTime();
    const bool livePhaseContinuation = isLiveGamePhase(previousSnapshot.phase)
        && isLiveGamePhase(next.phase)
        && (previousSnapshot.gameId == 0 || next.gameId == 0
            || previousSnapshot.gameId == next.gameId)
        && previousSnapshot.timerCountsDown == next.timerCountsDown;
    const bool sameClock = clockInitialized_
        && (clockGameId_ == 0 || next.gameId == 0 || clockGameId_ == next.gameId)
        && clockCountsDown_ == next.timerCountsDown
        && (clockPhase_.compare(next.phase, Qt::CaseInsensitive) == 0 || livePhaseContinuation);
    if (!nextHasClock) {
        clockInitialized_ = false;
        clockAnchorTime_ = {};
        clockAnchorSeconds_ = -1;
        clockPhase_.clear();
        clockGameId_ = 0;
    } else if (!sameClock) {
        clockInitialized_ = true;
        clockAnchorTime_ = now;
        clockAnchorSeconds_ = next.secondsRemaining;
        clockCountsDown_ = next.timerCountsDown;
        clockPhase_ = next.phase;
        clockGameId_ = next.gameId;
    } else {
        // Keep interpolation between polls, but accept a meaningful server
        // correction after reconnects or a clock drift.
        const qint64 elapsedSeconds = qMax<qint64>(0, clockAnchorTime_.secsTo(now));
        const int predicted = clockCountsDown_
            ? qMax(0, clockAnchorSeconds_ - static_cast<int>(elapsedSeconds))
            : clockAnchorSeconds_ + static_cast<int>(elapsedSeconds);
        if (qAbs(predicted - next.secondsRemaining) > 2) {
            clockAnchorTime_ = now;
            clockAnchorSeconds_ = next.secondsRemaining;
        }
        if (clockGameId_ == 0) clockGameId_ = next.gameId;
        clockPhase_ = next.phase;
    }

    const bool contentChanged = !sameSnapshotContent(snapshot_, next);
    snapshot_ = std::move(next);
    if (contentChanged) emit snapshotChanged();
    const bool active = snapshot_.isActive();
    const bool enteredChampSelect = active
        && snapshot_.phase.compare("ChampSelect", Qt::CaseInsensitive) == 0
        && lastObservedPhase_.compare("ChampSelect", Qt::CaseInsensitive) != 0;
    lastObservedPhase_ = snapshot_.phase;
    if (lastActivityState_ != active) {
        lastActivityState_ = active;
        emit activityObserved(active);
    }
    if (enteredChampSelect) emit champSelectEntered();
    updateLocalClock();
    if (snapshot_.isActive()) requestMissingPlayerNames();
}

void GameFlowRepository::updateLocalClock()
{
    if (!clockInitialized_ || !snapshot_.isActive() || snapshot_.secondsRemaining < 0) return;
    const qint64 elapsedSeconds = qMax<qint64>(0, clockAnchorTime_.secsTo(QDateTime::currentDateTime()));
    const int current = clockCountsDown_
        ? qMax(0, clockAnchorSeconds_ - static_cast<int>(elapsedSeconds))
        : clockAnchorSeconds_ + static_cast<int>(elapsedSeconds);
    if (current == snapshot_.secondsRemaining) return;
    snapshot_.secondsRemaining = current;
    emit remainingTimeChanged(current);
}

void GameFlowRepository::requestCurrentPlayer()
{
    if (!clientAvailable_ || currentPlayerRequestInFlight_) return;
    currentPlayerRequestInFlight_ = true;
    const quint64 requestGeneration = generation_;
    const QPointer<GameFlowRepository> repository(this);
    client_->get("/lol-summoner/v1/current-summoner",
                 [repository, requestGeneration](QJsonDocument document, QString error) {
        if (!repository) return;
        if (requestGeneration != repository->generation_ || !repository->clientAvailable_) {
            repository->currentPlayerRequestInFlight_ = false;
            return;
        }
        repository->currentPlayerRequestInFlight_ = false;
        if (!error.isEmpty() || !document.isObject()) return;
        const QJsonObject object = document.object();
        QString summonerId = idValue(object.value("summonerId"));
        if (summonerId.isEmpty()) summonerId = idValue(object.value("id"));
        const QString puuid = firstText(object, {"puuid", "playerPuuid", "playerUUID"});
        if (summonerId.isEmpty() && puuid.isEmpty()) return;
        const bool changed = repository->currentSummonerId_ != summonerId
            || repository->currentPuuid_ != puuid;
        repository->currentSummonerId_ = summonerId;
        repository->currentPuuid_ = puuid;
        if (changed && repository->snapshot_.isActive()) repository->applySnapshot(repository->snapshot_);
    });
}

void GameFlowRepository::requestMissingPlayerNames()
{
    for (const QList<GameFlowPlayer> *team : {&snapshot_.myTeam, &snapshot_.theirTeam}) {
        for (const GameFlowPlayer &player : *team) {
            if (player.isBot) continue;
            if (player.summonerId.isEmpty() && player.puuid.isEmpty()) continue;
            const auto identity = identities_.constFind(player.summonerId);
            if (identity == identities_.cend() && (player.gameName.isEmpty() || player.puuid.isEmpty())
                && !player.summonerId.isEmpty()
                && !pendingIdentityIds_.contains(player.summonerId)
                && !unavailableIdentityIds_.contains(player.summonerId)) {
                resolvePlayerName(player.summonerId);
                continue;
            }
            const QString puuid = !player.puuid.isEmpty()
                ? player.puuid
                : identity == identities_.cend() ? QString{} : identity->puuid;
            const QString playerKey = !player.summonerId.isEmpty() ? player.summonerId : puuid;
            if (!playerKey.isEmpty() && !puuid.isEmpty()) requestPlayerData(playerKey, puuid);
        }
    }
}

void GameFlowRepository::resolvePlayerName(const QString &summonerId)
{
    pendingIdentityIds_.insert(summonerId);
    const QString encodedId = QString::fromLatin1(QUrl::toPercentEncoding(summonerId));
    const QPointer<GameFlowRepository> repository(this);
    client_->get("/lol-summoner/v1/summoners/" + encodedId, [repository, summonerId](QJsonDocument document, QString error) {
        if (!repository) return;
        repository->pendingIdentityIds_.remove(summonerId);
        if (!error.isEmpty() || !document.isObject()) {
            repository->unavailableIdentityIds_.insert(summonerId);
            return;
        }

        const QJsonObject object = document.object();
        PlayerIdentity identity{
            firstText(object, {"gameName", "summonerName", "displayName", "name"}),
            firstText(object, {"tagLine", "riotIdTagline"}),
            firstText(object, {"puuid", "playerPuuid", "playerUUID"})
        };
        if (identity.gameName.isEmpty()) {
            repository->unavailableIdentityIds_.insert(summonerId);
            return;
        }
        repository->identities_.insert(summonerId, std::move(identity));
        repository->applySnapshot(repository->snapshot_);
    });
}

void GameFlowRepository::requestPlayerData(const QString &summonerId, const QString &puuid)
{
    requestPlayerHistory(summonerId, puuid);
    requestPlayerRank(summonerId, puuid);
}

void GameFlowRepository::requestPlayerHistory(const QString &summonerId, const QString &puuid)
{
    if (summonerId.isEmpty() || puuid.isEmpty()) return;
    if (recentMatchesBySummonerId_.contains(summonerId) || pendingHistoryIds_.contains(summonerId)) return;
    pendingHistoryIds_.insert(summonerId);
    const QString encodedPuuid = QString::fromLatin1(QUrl::toPercentEncoding(puuid));
    const QPointer<GameFlowRepository> repository(this);
    client_->get("/lol-match-history/v1/products/lol/" + encodedPuuid + "/matches?begIndex=0&endIndex=9",
                 [repository, summonerId, puuid](QJsonDocument document, QString) {
        if (!repository) return;
        repository->pendingHistoryIds_.remove(summonerId);
        repository->recentMatchesBySummonerId_.insert(summonerId, historyMatchesFrom(document, puuid));
        repository->applySnapshot(repository->snapshot_);
    });
}

void GameFlowRepository::requestPlayerRank(const QString &summonerId, const QString &puuid)
{
    if (summonerId.isEmpty() || puuid.isEmpty()) return;
    if (ranksBySummonerId_.contains(summonerId) || pendingRankIds_.contains(summonerId)) return;
    pendingRankIds_.insert(summonerId);
    const QString encodedPuuid = QString::fromLatin1(QUrl::toPercentEncoding(puuid));
    const QPointer<GameFlowRepository> repository(this);
    client_->get("/lol-ranked/v1/ranked-stats/" + encodedPuuid, [repository, summonerId](QJsonDocument document, QString) {
        if (!repository) return;
        repository->pendingRankIds_.remove(summonerId);
        repository->ranksBySummonerId_.insert(summonerId, rankFrom(document));
        repository->applySnapshot(repository->snapshot_);
    });
}

void GameFlowRepository::setLoading(const bool loading, const QString &message)
{
    if (loading_ == loading && message.isEmpty()) return;
    loading_ = loading;
    emit loadingChanged(loading, message);
}

GameFlowSnapshot GameFlowRepository::parse(const QJsonDocument &sessionDocument,
                                          const QJsonDocument &champSelectDocument,
                                          const QJsonDocument &spectatorDocument)
{
    GameFlowSnapshot snapshot;
    const QJsonObject session = sessionDocument.object();
    snapshot.phase = session.value("phase").toString().trimmed();
    QJsonObject gameData = session.value("gameData").toObject();
    if (snapshot.phase.isEmpty()) snapshot.phase = gameData.value("phase").toString().trimmed();
    // A few client builds flatten gameData during the hand-off to the game
    // process.  Merge those top-level fields so the same parser works for
    // both shapes instead of producing an active but empty page.
    mergeNonEmptyFields(gameData, session);
    const QJsonObject spectatorRoot = spectatorDocument.object();
    QJsonObject spectatorData = spectatorRoot.value("gameData").toObject();
    if (snapshot.phase.isEmpty()) snapshot.phase = spectatorRoot.value("phase").toString().trimmed();
    if (snapshot.phase.isEmpty()) snapshot.phase = spectatorData.value("phase").toString().trimmed();
    mergeNonEmptyFields(spectatorData, spectatorRoot);
    const auto spectatorValue = [&spectatorRoot, &spectatorData](const char *key) {
        const QJsonValue nested = spectatorData.value(QLatin1String(key));
        return nested.isUndefined() && !spectatorData.isEmpty()
            ? spectatorRoot.value(QLatin1String(key)) : nested;
    };
    snapshot.gameId = longValue(gameData.value("gameId"));
    if (snapshot.gameId == 0) snapshot.gameId = longValue(spectatorValue("gameId"));
    snapshot.gameMode = gameData.value("gameMode").toString().trimmed();
    if (snapshot.gameMode.isEmpty()) snapshot.gameMode = spectatorValue("gameMode").toString().trimmed();
    snapshot.mapId = intValue(gameData.value("mapId"));
    if (snapshot.mapId <= 0) snapshot.mapId = intValue(gameData.value("mapID"));
    if (snapshot.mapId <= 0) snapshot.mapId = intValue(spectatorValue("mapId"));
    if (snapshot.mapId <= 0) snapshot.mapId = intValue(spectatorValue("mapID"));
    snapshot.mapName = gameData.value("mapName").toString().trimmed();
    if (snapshot.mapName.isEmpty()) snapshot.mapName = gameData.value("mapStringId").toString().trimmed();
    if (snapshot.mapName.isEmpty()) snapshot.mapName = spectatorValue("mapName").toString().trimmed();
    if (snapshot.mapName.isEmpty()) snapshot.mapName = spectatorValue("mapStringId").toString().trimmed();
    const QJsonObject queue = gameData.value("queue").toObject();
    snapshot.queueId = intValue(queue.value("id"));
    if (snapshot.queueId <= 0) snapshot.queueId = intValue(gameData.value("queueId"));
    if (snapshot.queueId <= 0) snapshot.queueId = intValue(gameData.value("gameQueueConfigId"));
    if (snapshot.queueId <= 0) snapshot.queueId = intValue(spectatorValue("queueId"));
    if (snapshot.queueId <= 0) snapshot.queueId = intValue(spectatorValue("gameQueueConfigId"));
    QJsonObject spectatorQueue = spectatorData.value("queue").toObject();
    if (spectatorQueue.isEmpty()) spectatorQueue = spectatorRoot.value("queue").toObject();
    if (snapshot.queueId <= 0) snapshot.queueId = intValue(spectatorQueue.value("id"));
    snapshot.queueName = queue.value("name").toString().trimmed();
    if (snapshot.queueName.isEmpty()) snapshot.queueName = firstText(gameData, {"queueName", "queueDescription"});
    if (snapshot.queueName.isEmpty()) snapshot.queueName = spectatorQueue.value("name").toString().trimmed();
    if (snapshot.queueName.isEmpty()) snapshot.queueName = firstText(spectatorData, {"queueName", "queueDescription"});
    if (snapshot.queueName.isEmpty()) snapshot.queueName = firstText(spectatorRoot, {"queueName", "queueDescription"});
    if (snapshot.gameMode.isEmpty()) snapshot.gameMode = gameData.value("gameType").toString().trimmed();
    if (snapshot.gameMode.isEmpty()) snapshot.gameMode = spectatorValue("gameType").toString().trimmed();
    snapshot.gameStartTime = firstDateTime(gameData, {"gameStartTime", "gameStartTimestamp", "startTime", "gameCreation", "gameCreationDate"});
    if (!snapshot.gameStartTime.isValid()) {
        snapshot.gameStartTime = firstDateTime(spectatorData, {"gameStartTime", "gameStartTimestamp", "startTime", "gameCreation", "gameCreationDate"});
    }

    const QJsonObject champSelect = champSelectDocument.object();
    if (!champSelect.isEmpty()) {
        snapshot.myTeam = playersFrom(playersFrom(champSelect.value("myTeam")));
        snapshot.theirTeam = playersFrom(playersFrom(champSelect.value("theirTeam")));
        const QJsonObject bans = champSelect.value("bans").toObject();
        snapshot.myBans = championIdsFrom(bans.value("myTeamBans"));
        snapshot.theirBans = championIdsFrom(bans.value("theirTeamBans"));
        const QJsonArray actions = champSelect.value("actions").toArray();
        applyPickActions(snapshot.myTeam, actions);
        applyPickActions(snapshot.theirTeam, actions);
        appendActionBans(snapshot.myBans, snapshot.theirBans, actions, snapshot.myTeam);
        const QJsonObject timer = champSelect.value("timer").toObject();
        const double milliseconds = timer.value("adjustedTimeLeftInPhase").toDouble(-1.0);
        if (milliseconds >= 0.0 && !boolValue(timer.value("isInfinite"))) {
            snapshot.secondsRemaining = static_cast<int>(std::ceil(milliseconds / 1000.0));
            snapshot.timerCountsDown = true;
        }
        return snapshot;
    }

    if (isLiveGamePhase(snapshot.phase)) {
        const auto gameLengthValue = [&gameData, &spectatorValue](const char *key) {
            QJsonValue value = gameData.value(QLatin1String(key));
            const QJsonValue spectator = spectatorValue(key);
            if (value.isUndefined() || value.isNull()
                || (intValue(value) <= 0 && intValue(spectator) > 0)) {
                value = spectator;
            }
            if (value.isObject()) {
                const QJsonObject object = value.toObject();
                for (const char *nestedKey : {"seconds", "value", "duration", "elapsed"}) {
                    if (object.contains(QLatin1String(nestedKey))) return object.value(QLatin1String(nestedKey));
                }
            }
            return value;
        };
        QJsonValue gameLengthJson = gameLengthValue("gameLength");
        if (gameLengthJson.isUndefined()) gameLengthJson = gameLengthValue("gameLengthSeconds");
        if (gameLengthJson.isUndefined()) gameLengthJson = gameLengthValue("gameTime");
        const bool hasGameLength = !gameLengthJson.isUndefined() && !gameLengthJson.isNull();
        int gameLength = intValue(gameLengthJson);
        if (gameLength > 100000) gameLength = static_cast<int>(std::lround(gameLength / 1000.0));
        if (hasGameLength && (gameLength > 0 || !snapshot.gameStartTime.isValid())) {
            snapshot.secondsRemaining = gameLength;
        } else if (snapshot.gameStartTime.isValid()) {
            snapshot.secondsRemaining = qMax<qint64>(0, snapshot.gameStartTime.secsTo(QDateTime::currentDateTime()));
        }
        snapshot.timerCountsDown = false;
    }

    const auto teamPlayers = [&gameData, &spectatorData](const char *key) {
        const QList<GameFlowPlayer> sessionPlayers = playersFrom(playersFrom(gameData.value(QLatin1String(key))));
        const QList<GameFlowPlayer> spectatorPlayers = playersFrom(playersFrom(spectatorData.value(QLatin1String(key))));
        if (teamDataQuality(spectatorPlayers) > teamDataQuality(sessionPlayers)) return spectatorPlayers;
        return sessionPlayers.isEmpty() ? spectatorPlayers : sessionPlayers;
    };
    snapshot.myTeam = teamPlayers("teamOne");
    snapshot.theirTeam = teamPlayers("teamTwo");
    if (snapshot.myTeam.isEmpty() && snapshot.theirTeam.isEmpty()) {
        snapshot.myTeam = teamPlayers("myTeam");
        snapshot.theirTeam = teamPlayers("theirTeam");
    }
    if (snapshot.myTeam.isEmpty() && snapshot.theirTeam.isEmpty()) {
        snapshot.myTeam = teamPlayers("blueTeam");
        snapshot.theirTeam = teamPlayers("redTeam");
    }
    if (snapshot.myTeam.isEmpty() && snapshot.theirTeam.isEmpty()) {
        const QJsonValue teamsValue = !gameData.value("teams").isUndefined()
            ? gameData.value("teams") : spectatorValue("teams");
        if (teamsValue.isArray()) {
            const QJsonArray teams = teamsValue.toArray();
            if (teams.size() > 0) snapshot.myTeam = playersFrom(playersFrom(teams.at(0)));
            if (teams.size() > 1) snapshot.theirTeam = playersFrom(playersFrom(teams.at(1)));
        } else if (teamsValue.isObject()) {
            const QJsonObject teams = teamsValue.toObject();
            const QJsonValue my = !teams.value("myTeam").isUndefined()
                ? teams.value("myTeam") : !teams.value("teamOne").isUndefined()
                ? teams.value("teamOne") : teams.value("blue");
            const QJsonValue enemy = !teams.value("theirTeam").isUndefined()
                ? teams.value("theirTeam") : !teams.value("teamTwo").isUndefined()
                ? teams.value("teamTwo") : teams.value("red");
            snapshot.myTeam = playersFrom(playersFrom(my));
            snapshot.theirTeam = playersFrom(playersFrom(enemy));
        }
    }
    // When gameflow/session exposes placeholder team members, the spectator
    // participant list can still contain the authoritative champion/player
    // data.  Prefer it whenever its combined quality is higher.
    {
        QJsonArray participantEntries = playerEntriesFrom(gameData.value("participants"));
        if (participantEntries.isEmpty()) participantEntries = playerEntriesFrom(spectatorValue("participants"));
        if (!participantEntries.isEmpty()) {
            QList<GameFlowPlayer> participantMyTeam;
            QList<GameFlowPlayer> participantTheirTeam;
            splitParticipantEntries(participantEntries, participantMyTeam, participantTheirTeam);
            const int currentQuality = teamDataQuality(snapshot.myTeam) + teamDataQuality(snapshot.theirTeam);
            const int participantQuality = teamDataQuality(participantMyTeam) + teamDataQuality(participantTheirTeam);
            if (participantQuality > currentQuality) {
                snapshot.myTeam = std::move(participantMyTeam);
                snapshot.theirTeam = std::move(participantTheirTeam);
            }
        }
    }
    if (snapshot.myTeam.isEmpty() && snapshot.theirTeam.isEmpty()) {
        QJsonValue selectionsValue = gameData.value("playerChampionSelections");
        QJsonArray selections = playerEntriesFrom(selectionsValue);
        if (selections.isEmpty()) selections = playerEntriesFrom(spectatorValue("playerChampionSelections"));
        splitParticipantEntries(selections, snapshot.myTeam, snapshot.theirTeam);
    }
    if (snapshot.myTeam.isEmpty() && snapshot.theirTeam.isEmpty()) {
        QJsonValue participantsValue = gameData.value("participants");
        QJsonArray participants = playerEntriesFrom(participantsValue);
        if (participants.isEmpty()) participants = playerEntriesFrom(spectatorValue("participants"));
        splitParticipantEntries(participants, snapshot.myTeam, snapshot.theirTeam);
    }
    appendSpectatorBans(gameData.value("bannedChampions"), snapshot.myBans, snapshot.theirBans);
    appendSpectatorBans(spectatorValue("bannedChampions"), snapshot.myBans, snapshot.theirBans);
    const auto localTeamId = [&snapshot] {
        for (const QList<GameFlowPlayer> *team : {&snapshot.myTeam, &snapshot.theirTeam}) {
            for (const GameFlowPlayer &player : *team) {
                if (player.isLocalPlayer && player.teamId != 0) return player.teamId;
            }
        }
        return 0;
    }();
    // Spectator bans are keyed to the fixed blue/red team ids.  If the
    // current player is on red, the participant lists are already oriented to
    // the local side, so orient the bans the same way before rendering.
    if (localTeamId == 200 || localTeamId == 2) std::swap(snapshot.myBans, snapshot.theirBans);
    normalizeLocalTeam(snapshot, {}, {});
    return snapshot;
}

} // namespace Janna
