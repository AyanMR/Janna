#include "services/AssistController.h"

#include "services/LcuClient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QSettings>

#include <algorithm>

namespace Janna {
namespace {

int intValue(const QJsonValue &value)
{
    bool ok = false;
    const int number = value.toVariant().toInt(&ok);
    return ok ? number : 0;
}

QString stringValue(const QJsonValue &value)
{
    if (value.isString()) return value.toString().trimmed();
    if (value.isDouble()) return QString::number(value.toInt());
    return {};
}

bool boolValue(const QJsonValue &value)
{
    if (value.isBool()) return value.toBool();
    if (value.isDouble()) return value.toInt() != 0;
    return value.toString().trimmed().compare("true", Qt::CaseInsensitive) == 0
        || value.toString().trimmed() == "1";
}

AssistLaneSettings readLaneSettings(QSettings &settings, const QString &prefix)
{
    AssistLaneSettings result;
    result.banChampionId = settings.value(prefix + "/banChampionId", 0).toInt();
    result.pickChampionId = settings.value(prefix + "/pickChampionId", 0).toInt();
    result.autoBan = settings.value(prefix + "/autoBan", false).toBool();
    result.autoPick = settings.value(prefix + "/autoPick", false).toBool();
    result.autoLock = settings.value(prefix + "/autoLock", false).toBool();
    return result;
}

void writeLaneSettings(QSettings &settings, const QString &prefix, const AssistLaneSettings &value)
{
    settings.setValue(prefix + "/banChampionId", qMax(0, value.banChampionId));
    settings.setValue(prefix + "/pickChampionId", qMax(0, value.pickChampionId));
    settings.setValue(prefix + "/autoBan", value.autoBan);
    settings.setValue(prefix + "/autoPick", value.autoPick);
    settings.setValue(prefix + "/autoLock", value.autoLock);
}

void flattenActions(const QJsonValue &value, QList<ChampSelectAction> &result, int &ordinal)
{
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) flattenActions(entry, result, ordinal);
        return;
    }
    if (!value.isObject()) return;
    const QJsonObject object = value.toObject();
    const QString type = object.value("type").toString().trimmed().toLower();
    if (type == "ban" || type == "pick") {
        ChampSelectAction action;
        action.id = stringValue(object.value("id"));
        if (action.id.isEmpty()) action.id = stringValue(object.value("actionId"));
        if (action.id.isEmpty()) action.id = QString::number(ordinal);
        ++ordinal;
        action.type = type;
        action.actorCellId = intValue(object.value("actorCellId"));
        action.championId = intValue(object.value("championId"));
        if (action.championId <= 0) action.championId = intValue(object.value("championID"));
        action.completed = boolValue(object.value("completed"));
        action.inProgress = boolValue(object.value("isInProgress"))
            || boolValue(object.value("inProgress")) || boolValue(object.value("isCurrentTurn"));
        action.allyAction = object.contains("isAllyAction") ? boolValue(object.value("isAllyAction")) : true;
        result.append(std::move(action));
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.key() == "type" || it.key() == "id" || it.key() == "actionId") continue;
        if (it.value().isArray() || it.value().isObject()) flattenActions(it.value(), result, ordinal);
    }
}

int localCellId(const QJsonObject &root, const GameFlowSnapshot &snapshot)
{
    for (const QJsonValue &value : root.value("myTeam").toArray()) {
        const QJsonObject object = value.toObject();
        if (boolValue(object.value("isLocalPlayer")) || boolValue(object.value("isPlayer"))
            || boolValue(object.value("isMe"))) {
            return intValue(object.value("cellId"));
        }
    }
    for (const GameFlowPlayer &player : snapshot.myTeam) {
        if (player.isLocalPlayer && player.cellId != 0) return player.cellId;
    }
    const int direct = intValue(root.value("localPlayerCellId"));
    return direct;
}

QJsonObject runePagePayload(const OpggBuild &build)
{
    QJsonArray perks;
    // Stat shards are separate slots in the LCU schema.  Some OP.GG payloads
    // also mirror them in selectedPerkIds, and two slots can legitimately use
    // the same id (for example two adaptive-force shards).  Filter mirrored
    // shard ids from the main tree, then append every shard slot in order.
    for (const int id : build.runeIds) {
        if (id > 0 && !(id >= 5000 && id < 5100)) perks.append(id);
    }
    for (const int id : build.statShardIds) if (id > 0) perks.append(id);
    QJsonObject payload;
    payload.insert("name", QString("OP.GG · %1").arg(build.championName.trimmed().isEmpty()
        ? QStringLiteral("推荐配置") : build.championName.trimmed()).left(40));
    payload.insert("primaryStyleId", build.primaryStyleId);
    payload.insert("subStyleId", build.subStyleId);
    payload.insert("selectedPerkIds", perks);
    payload.insert("current", true);
    payload.insert("isActive", true);
    return payload;
}

} // namespace

AssistController::AssistController(LcuClient &client, GameFlowRepository &gameFlow, QObject *parent)
    : QObject(parent), client_(client), gameFlow_(gameFlow)
{
    loadSettings();
    connect(&gameFlow_, &GameFlowRepository::snapshotChanged, this, &AssistController::snapshotChanged);
}

QString AssistController::normalizeLane(const QString &lane)
{
    const QString value = lane.trimmed().toUpper();
    if (value == "TOP" || value == QString::fromUtf8("上路")) return "top";
    if (value == "JUNGLE" || value == "JG" || value == QString::fromUtf8("打野")) return "jungle";
    if (value == "MIDDLE" || value == "MID" || value == QString::fromUtf8("中路")) return "mid";
    if (value == "BOTTOM" || value == "BOT" || value == "ADC" || value == QString::fromUtf8("下路")) return "bot";
    if (value == "UTILITY" || value == "SUPPORT" || value == QString::fromUtf8("辅助")) return "support";
    if (value == "DEFAULT" || value == QString::fromUtf8("默认")) return "default";
    return {};
}

QString AssistController::laneLabel(const QString &lane)
{
    const QString normalized = normalizeLane(lane);
    if (normalized == "top") return QString::fromUtf8("上路");
    if (normalized == "jungle") return QString::fromUtf8("打野");
    if (normalized == "mid") return QString::fromUtf8("中路");
    if (normalized == "bot") return QString::fromUtf8("下路");
    if (normalized == "support") return QString::fromUtf8("辅助");
    return QString::fromUtf8("默认");
}

QString AssistController::modeKey(const GameFlowSnapshot &snapshot)
{
    switch (snapshot.queueId) {
    case 420: return "SOLORANKED";
    case 440: return "FLEXRANKED";
    case 450: return "ARAM";
    case 400:
    case 430: return "NORMAL";
    case 1020: return "ONEFORALL";
    case 1400: return "ULTBOOK";
    case 1700:
    case 1710: return "ARENA";
    default: break;
    }
    const QString mode = snapshot.gameMode.trimmed().toUpper();
    if (mode.contains("ARAM")) return "ARAM";
    if (mode.contains("URF")) return "URF";
    if (mode.contains("ONE")) return "ONEFORALL";
    if (mode.contains("ULT")) return "ULTBOOK";
    if (mode.contains("ARENA")) return "ARENA";
    if (mode.contains("RANKED") && mode.contains("FLEX")) return "FLEXRANKED";
    if (mode.contains("RANKED")) return "SOLORANKED";
    if (mode == "CLASSIC" || mode == "SUMMONERS_RIFT") return "NORMAL";
    return mode.isEmpty() ? QStringLiteral("NORMAL") : mode;
}

bool AssistController::modeHasLane(const GameFlowSnapshot &snapshot)
{
    if (snapshot.queueId == 400 || snapshot.queueId == 420 || snapshot.queueId == 430 || snapshot.queueId == 440) return true;
    const QString mode = modeKey(snapshot);
    return mode == "NORMAL" || mode == "SOLORANKED" || mode == "FLEXRANKED";
}

QString AssistController::effectiveLane(const GameFlowSnapshot &snapshot)
{
    if (!modeHasLane(snapshot)) return "default";
    for (const GameFlowPlayer &player : snapshot.myTeam) {
        if (!player.isLocalPlayer) continue;
        const QString lane = normalizeLane(player.assignedPosition);
        if (!lane.isEmpty() && lane != "default") return lane;
    }
    return "default";
}

bool AssistController::usesDefaultSettings(const GameFlowSnapshot &snapshot)
{
    return effectiveLane(snapshot) == "default";
}

AssistLaneSettings AssistController::settingsForLane(const QString &lane) const
{
    const QString normalized = normalizeLane(lane);
    if (normalized.isEmpty() || normalized == "default") return settings_.defaults;
    return settings_.lanes.value(normalized, settings_.defaults);
}

void AssistController::setSettingsForLane(const QString &lane, const AssistLaneSettings &value)
{
    const QString normalized = normalizeLane(lane);
    if (normalized.isEmpty() || normalized == "default") settings_.defaults = value;
    else settings_.lanes.insert(normalized, value);
    saveSettings();
    emit settingsChanged();
    snapshotChanged();
}

void AssistController::loadSettings()
{
    QSettings settings;
    settings_.defaults = readLaneSettings(settings, "assist/default");
    for (const QString &lane : {"top", "jungle", "mid", "bot", "support"}) {
        settings_.lanes.insert(lane, readLaneSettings(settings, "assist/" + lane));
    }
}

void AssistController::saveSettings() const
{
    QSettings settings;
    writeLaneSettings(settings, "assist/default", settings_.defaults);
    for (const QString &lane : {"top", "jungle", "mid", "bot", "support"}) {
        writeLaneSettings(settings, "assist/" + lane, settings_.lanes.value(lane));
    }
}

void AssistController::setClientAvailable(const bool available)
{
    if (clientAvailable_ == available) return;
    clientAvailable_ = available;
    sessionRequestInFlight_ = false;
    actionRequestInFlight_ = false;
    attemptedActions_.clear();
    if (!available) {
        lastLockedChampionId_ = 0;
        lastLockedGameId_ = 0;
        emit statusChanged(QStringLiteral("等待 League Client"));
    }
}

QList<ChampSelectAction> AssistController::parseActions(const QJsonDocument &document)
{
    QList<ChampSelectAction> result;
    int ordinal = 1;
    const QJsonObject root = document.object();
    flattenActions(root.value("actions"), result, ordinal);
    if (result.isEmpty()) flattenActions(document.isArray() ? QJsonValue(document.array()) : QJsonValue(root), result, ordinal);
    return result;
}

void AssistController::snapshotChanged()
{
    const GameFlowSnapshot &snapshot = gameFlow_.snapshot();
    if (snapshot.phase.compare("ChampSelect", Qt::CaseInsensitive) != 0) {
        // A number of queues omit gameId in champ select.  Resetting on the
        // phase edge keeps a same-champion pick in the next queue observable.
        lastLockedChampionId_ = 0;
        attemptedActions_.clear();
    }
    if (snapshot.gameId != 0 && snapshot.gameId != lastLockedGameId_) {
        lastLockedGameId_ = snapshot.gameId;
        lastLockedChampionId_ = 0;
        attemptedActions_.clear();
    }

    int lockedChampion = 0;
    for (const GameFlowPlayer &player : snapshot.myTeam) {
        if (player.isLocalPlayer && player.pickCompleted && player.championId > 0) {
            lockedChampion = player.championId;
            break;
        }
    }
    // `pickCompleted` remains true after champ select while the game is
    // loading.  Only the ChampSelect edge represents a new lock; otherwise
    // resetting lastLockedChampionId_ on every live-game poll would emit the
    // signal repeatedly and reopen the OPGG window.
    if (snapshot.phase.compare("ChampSelect", Qt::CaseInsensitive) == 0
        && lockedChampion > 0 && lockedChampion != lastLockedChampionId_) {
        lastLockedChampionId_ = lockedChampion;
        emit championLocked(lockedChampion, effectiveLane(snapshot), modeKey(snapshot));
        emit statusChanged(QStringLiteral("已锁定 %1，正在读取 OP.GG 推荐配置").arg(lockedChampion));
    }

    if (!clientAvailable_ || snapshot.phase.compare("ChampSelect", Qt::CaseInsensitive) != 0) return;
    const AssistLaneSettings config = settingsForLane(effectiveLane(snapshot));
    if ((!config.autoBan || config.banChampionId <= 0) && (!config.autoPick || config.pickChampionId <= 0)) return;
    requestChampSelectSession();
}

void AssistController::requestChampSelectSession()
{
    if (sessionRequestInFlight_ || actionRequestInFlight_) return;
    sessionRequestInFlight_ = true;
    const QPointer<AssistController> controller(this);
    client_.get("/lol-champ-select/v1/session", [controller](QJsonDocument document, QString error) {
        if (controller) controller->handleChampSelectSession(std::move(document), std::move(error));
    });
}

void AssistController::handleChampSelectSession(QJsonDocument document, QString error)
{
    sessionRequestInFlight_ = false;
    if (!error.isEmpty() || !document.isObject() || !clientAvailable_) return;
    const GameFlowSnapshot &snapshot = gameFlow_.snapshot();
    if (snapshot.phase.compare("ChampSelect", Qt::CaseInsensitive) != 0) return;
    const AssistLaneSettings config = settingsForLane(effectiveLane(snapshot));
    const int cellId = localCellId(document.object(), snapshot);
    QList<ChampSelectAction> actions = parseActions(document);
    for (const ChampSelectAction &action : actions) {
        if (!action.allyAction || action.completed || !action.inProgress) continue;
        if (cellId != 0 && action.actorCellId != 0 && action.actorCellId != cellId) continue;
        const bool ban = action.type == "ban";
        const bool enabled = ban ? config.autoBan : config.autoPick;
        const int target = ban ? config.banChampionId : config.pickChampionId;
        if (!enabled || target <= 0) continue;
        if (ban && (snapshot.myBans.contains(target) || snapshot.theirBans.contains(target))) continue;
        const QString key = QString::number(snapshot.gameId) + ":" + action.id + ":" + QString::number(target);
        if (attemptedActions_.contains(key)) continue;
        attemptedActions_.insert(key);
        submitAction(action, target, ban || config.autoLock, key);
        return;
    }
}

void AssistController::submitAction(const ChampSelectAction &action, const int championId, const bool lock,
                                    const QString &attemptKey)
{
    if (action.id.isEmpty()) return;
    actionRequestInFlight_ = true;
    QJsonObject payload;
    payload.insert("championId", championId);
    if (action.type == "pick" && lock) payload.insert("completed", true);
    if (action.type == "ban") payload.insert("completed", true);
    const QString path = "/lol-champ-select/v1/session/actions/" + action.id;
    const QPointer<AssistController> controller(this);
    const auto finish = [controller, attemptKey](bool success, QString message) {
        if (!controller) return;
        controller->actionRequestInFlight_ = false;
        if (!success) controller->attemptedActions_.remove(attemptKey);
        if (success) {
            emit controller->actionObserved(message);
            emit controller->statusChanged(message);
        } else {
            emit controller->actionObserved(message);
            emit controller->statusChanged(QStringLiteral("自动操作失败：") + message);
        }
    };
    const auto retryPost = [controller, path, payload, finish](QString) {
        if (!controller) return;
        controller->client_.postJson(path, payload, [finish](QJsonDocument, QString error) {
            finish(error.isEmpty(), error.isEmpty() ? QStringLiteral("已提交自动选择操作") : error);
        });
    };
    client_.patchJson(path, payload, [finish, retryPost](QJsonDocument, QString error) mutable {
        if (error.isEmpty()) finish(true, QStringLiteral("已提交自动选择操作"));
        else retryPost(error);
    });
}

void AssistController::applyBuild(const OpggBuild &build, ActionReply reply)
{
    if (!clientAvailable_) return reply(false, QStringLiteral("League Client 不可用。"));
    if (build.primaryStyleId <= 0 || build.runeIds.isEmpty()) {
        return reply(false, QStringLiteral("OP.GG 推荐配置缺少完整符文，未写入客户端。"));
    }
    const QPointer<AssistController> controller(this);
    applyRunePage(build, [controller, build, reply = std::move(reply)](bool success, QString message) mutable {
        if (!controller) return;
        if (!success) {
            emit controller->buildApplied(false, message);
            return reply(false, std::move(message));
        }
        controller->applySummonerSpells(build, [controller, reply = std::move(reply), message = std::move(message)](bool spellsOk, QString spellMessage) mutable {
            if (!controller) return;
            QString combined = message;
            if (!spellMessage.isEmpty()) combined += "\n" + spellMessage;
            const bool success = spellsOk;
            emit controller->buildApplied(success, combined);
            reply(success, std::move(combined));
        });
    });
}

void AssistController::applyRunePage(const OpggBuild &build, ActionReply reply)
{
    const QJsonObject payload = runePagePayload(build);
    const QPointer<AssistController> controller(this);
    client_.postJson("/lol-perks/v1/pages", payload, [controller, build, payload, reply = std::move(reply)](QJsonDocument, QString error) mutable {
        if (!controller) return;
        if (error.isEmpty()) return reply(true, QStringLiteral("已将 OP.GG 符文设为当前页面。"));
        // Older clients reject a second page with the same name.  Reuse the
        // current page when possible, keeping one-click setup idempotent.
        const QString createError = error;
        controller->client_.get("/lol-perks/v1/pages", [controller, payload, createError, reply = std::move(reply)](QJsonDocument pages, QString listError) mutable {
            if (!controller) return;
            if (!listError.isEmpty() || !pages.isArray()) return reply(false, QStringLiteral("符文页写入失败：") + listError);
            for (const QJsonValue &value : pages.array()) {
                const QJsonObject page = value.toObject();
                const int id = intValue(page.value("id"));
                if (id <= 0 || (!boolValue(page.value("current")) && !boolValue(page.value("isActive")))) continue;
                const QString path = "/lol-perks/v1/pages/" + QString::number(id);
                QJsonObject updatePayload = page;
                for (auto payloadIt = payload.constBegin(); payloadIt != payload.constEnd(); ++payloadIt) {
                    updatePayload.insert(payloadIt.key(), payloadIt.value());
                }
                controller->client_.putJson(path, updatePayload,
                                             [controller, path, updatePayload, reply = std::move(reply)](QJsonDocument, QString putError) mutable {
                    if (!controller) return;
                    if (putError.isEmpty()) return reply(true, QStringLiteral("已更新 OP.GG 符文页。"));
                    controller->client_.patchJson(path, updatePayload,
                                                   [reply = std::move(reply)](QJsonDocument, QString patchError) mutable {
                        reply(patchError.isEmpty(), patchError.isEmpty()
                            ? QStringLiteral("已更新 OP.GG 符文页。") : QStringLiteral("符文页更新失败：") + patchError);
                    });
                });
                return;
            }
            reply(false, QStringLiteral("符文页写入失败：") + createError);
        });
    });
}

void AssistController::applySummonerSpells(const OpggBuild &build, ActionReply reply)
{
    if (build.summonerSpellIds.size() < 2) return reply(true, QStringLiteral("OP.GG 未提供召唤师技能，已保留当前技能。"));
    QJsonObject payload;
    payload.insert("spell1Id", build.summonerSpellIds.at(0));
    payload.insert("spell2Id", build.summonerSpellIds.at(1));
    const QString path = "/lol-champ-select/v1/session/my-selection";
    const QPointer<AssistController> controller(this);
    client_.postJson(path, payload,
                     [controller, path, payload, reply = std::move(reply)](QJsonDocument, QString postError) mutable {
        if (!controller) return;
        if (postError.isEmpty()) return reply(true, QStringLiteral("已应用 OP.GG 召唤师技能。"));
        controller->client_.patchJson(path, payload,
                          [controller, path, payload, postError, reply = std::move(reply)](QJsonDocument, QString patchError) mutable {
            if (!controller) return;
            if (patchError.isEmpty()) return reply(true, QStringLiteral("已应用 OP.GG 召唤师技能。"));
            controller->client_.putJson(path, payload,
                            [reply = std::move(reply), postError, patchError](QJsonDocument, QString putError) mutable {
                if (putError.isEmpty()) return reply(true, QStringLiteral("已应用 OP.GG 召唤师技能。"));
                reply(false, QStringLiteral("召唤师技能写入失败：") + postError + " / " + patchError + " / " + putError);
            });
        });
    });
}

} // namespace Janna
