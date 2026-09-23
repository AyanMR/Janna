#pragma once

#include <QList>
#include <QHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <memory>

namespace Janna {

class LcuClient;

struct GameFlowMatch {
    qint64 gameId{};
    int championId{};
    int kills{};
    int deaths{};
    int assists{};
    bool won{};
    int queueId{};
    QString gameMode;
    QString queueName;
    QDateTime createdAt;
    int durationSeconds{};
};

struct GameFlowRank {
    QString tier;
    QString division;
    int leaguePoints{};

    [[nodiscard]] bool isValid() const { return !tier.trimmed().isEmpty(); }
};

struct GameFlowPlayer {
    int cellId{};
    int championId{};
    int championPickIntent{};
    int spell1Id{};
    int spell2Id{};
    QString summonerId;
    QString puuid;
    QString gameName;
    QString tagLine;
    QString assignedPosition;
    bool pickCompleted{};
    bool pickInProgress{};
    GameFlowRank rank;
    QList<GameFlowMatch> recentMatches;
    bool recentMatchesLoading{};
    int teamId{};
    bool isLocalPlayer{};
    bool isBot{};
    int kills{};
    int deaths{};
    int assists{};
    bool currentKdaAvailable{};

    [[nodiscard]] QString riotId() const
    {
        return tagLine.isEmpty() ? gameName : gameName + "#" + tagLine;
    }
};

struct GameFlowSnapshot {
    QString phase;
    qint64 gameId{};
    int queueId{};
    QString queueName;
    int mapId{};
    QString mapName;
    QString gameMode;
    int secondsRemaining{-1};
    QList<GameFlowPlayer> myTeam;
    QList<GameFlowPlayer> theirTeam;
    QList<int> myBans;
    QList<int> theirBans;
    bool timerCountsDown{};
    QDateTime gameStartTime;

    [[nodiscard]] bool isActive() const;
};

class GameFlowRepository final : public QObject {
    Q_OBJECT

public:
    explicit GameFlowRepository(QObject *parent = nullptr);
    ~GameFlowRepository() override;

    [[nodiscard]] const GameFlowSnapshot &snapshot() const { return snapshot_; }
    [[nodiscard]] static GameFlowSnapshot parse(const QJsonDocument &sessionDocument,
                                                 const QJsonDocument &champSelectDocument = QJsonDocument(),
                                                 const QJsonDocument &spectatorDocument = QJsonDocument());

    void setClientAvailable(bool available);
    void refresh();

signals:
    void snapshotChanged();
    void activityObserved(bool active);
    void champSelectEntered();
    void remainingTimeChanged(int seconds);
    void loadingChanged(bool loading, const QString &message);

private:
    struct PlayerIdentity {
        QString gameName;
        QString tagLine;
        QString puuid;
    };

    void poll();
    void handleGameFlowSession(QJsonDocument document, QString error, quint64 generation);
    void requestPhaseFallback(QJsonDocument document, QString sessionError, quint64 generation);
    void handleChampSelectSession(QJsonDocument gameFlowDocument, QJsonDocument document,
                                  QString error, quint64 generation);
    void requestSpectatorGame(QJsonDocument gameFlowDocument, quint64 generation);
    void handleSpectatorGame(QJsonDocument gameFlowDocument, QJsonDocument document,
                             QString error, quint64 generation);
    void applySnapshot(GameFlowSnapshot snapshot);
    void requestCurrentPlayer();
    void requestMissingPlayerNames();
    void resolvePlayerName(const QString &summonerId);
    void requestPlayerData(const QString &summonerId, const QString &puuid);
    void requestPlayerHistory(const QString &summonerId, const QString &puuid);
    void requestPlayerRank(const QString &summonerId, const QString &puuid);
    void setLoading(bool loading, const QString &message = {});
    void updateLocalClock();

    std::unique_ptr<LcuClient> client_;
    QTimer *pollTimer_{};
    QTimer *clockTimer_{};
    GameFlowSnapshot snapshot_;
    QHash<QString, PlayerIdentity> identities_;
    QHash<QString, QList<GameFlowMatch>> recentMatchesBySummonerId_;
    QHash<QString, GameFlowRank> ranksBySummonerId_;
    QSet<QString> pendingIdentityIds_;
    QSet<QString> pendingHistoryIds_;
    QSet<QString> pendingRankIds_;
    QSet<QString> unavailableIdentityIds_;
    QString currentSummonerId_;
    QString currentPuuid_;
    bool clientAvailable_{};
    bool requestInFlight_{};
    bool currentPlayerRequestInFlight_{};
    bool loading_{};
    bool spectatorRosterLoaded_{};
    QString spectatorRosterPhase_;
    int missingSessionPolls_{};
    quint64 generation_{};
    bool clockInitialized_{};
    QDateTime clockAnchorTime_;
    int clockAnchorSeconds_{-1};
    bool clockCountsDown_{};
    qint64 clockGameId_{};
    QString clockPhase_;
    bool lastActivityState_{};
    QString lastObservedPhase_;
};

} // namespace Janna
