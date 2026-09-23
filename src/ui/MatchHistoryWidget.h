#pragma once

#include "model/Match.h"

#include <QList>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QWidget>

class QFrame;
class QPaintEvent;
class QTimer;
class QVBoxLayout;

namespace Janna {

class ChampionRepository;
class GameDataRepository;
class MatchRepository;

class MatchHistoryWidget : public QWidget {
    Q_OBJECT

public:
    explicit MatchHistoryWidget(ChampionRepository &champions, MatchRepository &matchRepository,
                                GameDataRepository &gameData, QWidget *parent = nullptr);

    void setMatches(QList<MatchSummary> matches);
    void collapseDetails();

signals:
    void playerSelected(const QString &puuid, const QString &summonerId, const QString &riotId);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void addDetails(qint64 gameId, bool animated);
    [[nodiscard]] bool hasRunningDetailAnimation() const;
    void removeDetails(qint64 gameId, bool animated);
    void queueAssetRefresh();
    void rebuild();
    void refreshDetails(qint64 gameId, bool animated);
    void toggleDetails(qint64 gameId);
    void saveFavorites() const;

    ChampionRepository &champions_;
    MatchRepository &matchRepository_;
    GameDataRepository &gameData_;
    QList<MatchSummary> matches_;
    QVBoxLayout *layout_{};
    QSet<qint64> favoriteGameIds_;
    QList<qint64> expandedGameIds_;
    QHash<qint64, QPointer<QFrame>> rowFrames_;
    QHash<qint64, QPointer<QFrame>> detailFrames_;
    QTimer *assetRebuildTimer_{};
};

} // namespace Janna
