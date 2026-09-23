#pragma once

#include "model/Match.h"

#include <QObject>
#include <QString>

#include <QHash>
#include <QList>
#include <QSet>

#include <functional>

namespace Janna {

class LcuClient;
class SummonerProfile;

class MatchRepository : public QObject {
    Q_OBJECT

public:
    explicit MatchRepository(LcuClient &client, QObject *parent = nullptr);

    [[nodiscard]] const QList<MatchSummary> &matches() const { return matches_; }
    [[nodiscard]] const MatchDetail *detail(qint64 gameId) const;
    [[nodiscard]] bool isDetailLoading(qint64 gameId) const { return loadingDetailGameIds_.contains(gameId); }
    [[nodiscard]] QString detailError(qint64 gameId) const { return detailErrors_.value(gameId); }

public slots:
    void refresh(const SummonerProfile &profile, int count = 20);
    void loadDetail(qint64 gameId);

signals:
    void matchesChanged();
    void loadingChanged(bool loading, const QString &message);
    void matchDetailChanged(qint64 gameId);
    void matchDetailLoadingChanged(qint64 gameId, bool loading, const QString &message);

private:
    struct ParticipantRank {
        QString tier;
        QString division;
        int leaguePoints{};
    };

    using PrivacyCallback = std::function<void(bool known, bool hidden)>;
    using RankCallback = std::function<void(const ParticipantRank &rank)>;

    void enrichDetailRanks(qint64 gameId, quint64 generation);
    void finishDetailLoad(qint64 gameId, MatchDetail detail, quint64 generation);
    void resolveDetailPrivacy(MatchDetail detail, qint64 gameId, quint64 generation);
    void resolveParticipantPrivacy(const QString &summonerId, PrivacyCallback callback);
    void resolveParticipantRank(const QString &puuid, RankCallback callback);

    LcuClient &client_;
    QList<MatchSummary> matches_;
    QHash<qint64, MatchDetail> details_;
    QHash<QString, bool> privacyBySummonerId_;
    QHash<QString, QList<PrivacyCallback>> pendingPrivacyCallbacks_;
    QHash<QString, ParticipantRank> ranksByPuuid_;
    QHash<QString, QList<RankCallback>> pendingRankCallbacks_;
    QSet<qint64> loadingDetailGameIds_;
    QSet<qint64> loadingDetailRankGameIds_;
    QHash<qint64, QString> detailErrors_;
    bool loading_{};
    quint64 dataGeneration_{};
};

} // namespace Janna
