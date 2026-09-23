#pragma once

#include "model/Match.h"

#include <QList>
#include <QString>

namespace Janna {

class GameDataRepository;
class RankedRepository;
class ChampionRepository;

struct RoleUsage {
    QString label;
    int games{};
};

struct ChampionUsage {
    int championId{};
    int games{};
    int wins{};
    int losses{};
    int kills{};
    int deaths{};
    int assists{};
};

struct MatchModeStatistics {
    int queueId{};
    QString modeName;
    int games{};
    int wins{};
    int losses{};
    QString rank;
    QString highestRank;
};

class MatchAnalytics final {
public:
    [[nodiscard]] static QList<RoleUsage> roleUsage(const QList<MatchSummary> &matches);
    [[nodiscard]] static QList<RoleUsage> championTypeUsage(const QList<MatchSummary> &matches,
                                                              const ChampionRepository &champions);
    [[nodiscard]] static QList<ChampionUsage> recentChampionUsage(const QList<MatchSummary> &matches);
    [[nodiscard]] static QList<MatchModeStatistics> modeStatistics(const QList<MatchSummary> &matches,
                                                                     const GameDataRepository &gameData,
                                                                     const RankedRepository &ranked);
};

} // namespace Janna
