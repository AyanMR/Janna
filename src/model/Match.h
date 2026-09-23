#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

namespace Janna {

struct MatchSummary {
    qint64 gameId{};
    bool won{};
    QString gameMode;
    int queueId{};
    int mapId{};
    int durationSeconds{};
    QDateTime createdAt;
    int championId{};
    int championLevel{};
    QString lane;
    QString role;
    int primaryRuneId{};
    QList<int> runeIds;
    int spell1Id{};
    int spell2Id{};
    int kills{};
    int deaths{};
    int assists{};
    int minionKills{};
    int goldEarned{};
    QList<int> itemIds;
};

struct MatchParticipant {
    int participantId{};
    int teamId{};
    int championId{};
    int championLevel{};
    QString lane;
    QString role;
    int primaryRuneId{};
    QList<int> runeIds;
    int spell1Id{};
    int spell2Id{};
    QString gameName;
    QString tagLine;
    QString puuid;
    QString summonerId;
    bool hiddenMatchHistory{};
    QString highestAchievedSeasonTier;
    QString currentRankTier;
    QString currentRankDivision;
    int currentRankLeaguePoints{};
    QList<int> itemIds;
    int kills{};
    int deaths{};
    int assists{};
    int laneMinionKills{};
    int neutralMinionKills{};
    int goldEarned{};
    int damageDealtToChampions{};
    int damageTaken{};
    int visionScore{};
    int wardsPlaced{};
    int wardsKilled{};

    [[nodiscard]] int minionKills() const { return laneMinionKills + neutralMinionKills; }
    [[nodiscard]] QString riotId() const
    {
        return tagLine.isEmpty() ? gameName : gameName + "#" + tagLine;
    }
};

struct MatchTeam {
    int teamId{};
    bool won{};
    int towerKills{};
    int inhibitorKills{};
    int dragonKills{};
    int baronKills{};
    int riftHeraldKills{};
};

struct MatchDetail {
    qint64 gameId{};
    QString gameMode;
    int queueId{};
    int mapId{};
    int durationSeconds{};
    QDateTime createdAt;
    QList<MatchTeam> teams;
    QList<MatchParticipant> participants;

    [[nodiscard]] bool isValid() const { return gameId != 0 && !participants.isEmpty(); }
};

} // namespace Janna
