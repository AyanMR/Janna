#pragma once

#include "services/MatchAnalytics.h"

#include <QSet>
#include <QWidget>

class QHBoxLayout;

namespace Janna {

class ChampionRepository;

class RecentChampionUsageWidget final : public QWidget {
    Q_OBJECT

public:
    explicit RecentChampionUsageWidget(ChampionRepository &champions, QWidget *parent = nullptr);

    void setUsage(QList<ChampionUsage> usage);
    void setSelectedChampionIds(QSet<int> selected);

signals:
    void selectedChampionIdsChanged(const QSet<int> &selected);

private:
    void rebuild();
    [[nodiscard]] QString championName(int championId) const;

    ChampionRepository &champions_;
    QHBoxLayout *layout_{};
    QList<ChampionUsage> usage_;
    QSet<int> selectedChampionIds_;
    bool selectionNotificationQueued_{};
};

} // namespace Janna
