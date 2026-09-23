#pragma once

#include "services/SummonerRepository.h"

#include <QHash>
#include <QObject>
#include <QString>

namespace Janna {

class LcuClient;

struct RankedQueueStats {
    int queueId{};
    QString queueType;
    QString tier;
    QString division;
    int leaguePoints{};
    int wins{};
    int losses{};
    QString highestTier;
    QString highestDivision;
};

class RankedRepository final : public QObject {
    Q_OBJECT

public:
    explicit RankedRepository(LcuClient &client, QObject *parent = nullptr);

    [[nodiscard]] const QHash<int, RankedQueueStats> &stats() const { return stats_; }
    [[nodiscard]] static QString localizedTier(const QString &tier);
    [[nodiscard]] static QString currentRankText(const RankedQueueStats &stats);
    [[nodiscard]] static QString highestRankText(const RankedQueueStats &stats);

public:
    void refresh(const SummonerProfile &profile);

signals:
    void statsChanged();
    void loadingChanged(bool loading, const QString &message);

private:
    LcuClient &client_;
    QHash<int, RankedQueueStats> stats_;
    quint64 refreshGeneration_{};
};

} // namespace Janna
