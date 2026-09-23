#pragma once

#include "services/GameFlowRepository.h"
#include "services/OpggCrawler.h"

#include <QHash>
#include <QJsonDocument>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>

namespace Janna {

struct AssistLaneSettings {
    int banChampionId{};
    int pickChampionId{};
    bool autoBan{};
    bool autoPick{};
    bool autoLock{};
};

struct AssistSettings {
    AssistLaneSettings defaults;
    QHash<QString, AssistLaneSettings> lanes;
};

struct ChampSelectAction {
    QString id;
    QString type;
    int actorCellId{};
    int championId{};
    bool completed{};
    bool inProgress{};
    bool allyAction{};
};

class LcuClient;

class AssistController final : public QObject {
    Q_OBJECT

public:
    using ActionReply = std::function<void(bool success, QString message)>;

    AssistController(LcuClient &client, GameFlowRepository &gameFlow, QObject *parent = nullptr);

    [[nodiscard]] const AssistSettings &settings() const { return settings_; }
    [[nodiscard]] AssistLaneSettings settingsForLane(const QString &lane) const;
    void setSettingsForLane(const QString &lane, const AssistLaneSettings &settings);

    [[nodiscard]] static QString normalizeLane(const QString &lane);
    [[nodiscard]] static QString laneLabel(const QString &lane);
    [[nodiscard]] static QString modeKey(const GameFlowSnapshot &snapshot);
    [[nodiscard]] static bool modeHasLane(const GameFlowSnapshot &snapshot);
    [[nodiscard]] static QString effectiveLane(const GameFlowSnapshot &snapshot);
    [[nodiscard]] static bool usesDefaultSettings(const GameFlowSnapshot &snapshot);
    [[nodiscard]] static QList<ChampSelectAction> parseActions(const QJsonDocument &document);

    void setClientAvailable(bool available);
    void applyBuild(const OpggBuild &build, ActionReply reply);

signals:
    void settingsChanged();
    void statusChanged(const QString &message);
    void actionObserved(const QString &message);
    void championLocked(int championId, const QString &lane, const QString &mode);
    void buildApplied(bool success, const QString &message);

private:
    void loadSettings();
    void saveSettings() const;
    void snapshotChanged();
    void requestChampSelectSession();
    void handleChampSelectSession(QJsonDocument document, QString error);
    void submitAction(const ChampSelectAction &action, int championId, bool lock, const QString &attemptKey);
    void applyRunePage(const OpggBuild &build, ActionReply reply);
    void applySummonerSpells(const OpggBuild &build, ActionReply reply);

    LcuClient &client_;
    GameFlowRepository &gameFlow_;
    AssistSettings settings_;
    bool clientAvailable_{};
    bool sessionRequestInFlight_{};
    bool actionRequestInFlight_{};
    int lastLockedChampionId_{};
    qint64 lastLockedGameId_{};
    QSet<QString> attemptedActions_;
};

} // namespace Janna
