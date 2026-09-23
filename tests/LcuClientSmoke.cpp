#include "services/LcuClient.h"
#include "services/LcuCredentialProvider.h"
#include "services/CommunityDragonClient.h"
#include "services/MatchRepository.h"
#include "services/SummonerRepository.h"

#include <QCoreApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>

#include <iostream>

namespace {

bool isIconPath(const QString &path)
{
    return path.startsWith('/') && path.endsWith(".png", Qt::CaseInsensitive);
}

bool isCacheableStaticIconPath(const QString &path)
{
    const QString normalized = path.toLower();
    return isIconPath(path)
        && (normalized.contains("/items/") || normalized.contains("/items2d/")
            || normalized.contains("/perks/") || normalized.contains("/perk-images/")
            || normalized.contains("/spells/"));
}

void collectIconMappings(const QJsonValue &value, QHash<int, QString> &mappings, int &iconPathCount)
{
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) collectIconMappings(entry, mappings, iconPathCount);
        return;
    }
    if (!value.isObject()) return;

    const QJsonObject object = value.toObject();
    const QString path = object.value("iconPath").toString();
    bool validId = false;
    const int id = object.value("id").toVariant().toInt(&validId);
    if (!path.isEmpty()) ++iconPathCount;
    if (validId && id > 0 && isCacheableStaticIconPath(path)) {
        mappings.insert(id, path);
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        collectIconMappings(it.value(), mappings, iconPathCount);
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QString firstCredentialError;
    QString secondCredentialError;
    const Janna::LcuCredentials firstCredentials = Janna::LcuCredentialProvider::discover(firstCredentialError);
    const Janna::LcuCredentials secondCredentials = Janna::LcuCredentialProvider::discover(secondCredentialError);
    std::cerr << "credentials-stable=" << (firstCredentials.port == secondCredentials.port && firstCredentials.token == secondCredentials.token)
              << " valid=" << (firstCredentials.isValid() && secondCredentials.isValid()) << '\n';
    Janna::LcuClient client;
    Janna::SummonerRepository summoner(client);
    Janna::SummonerRepository selectedSummoner(client);
    Janna::MatchRepository matchHistory(client);
    Janna::CommunityDragonClient communityDragon;
    bool rankEnrichmentObserved = false;

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &application, [&application] {
        std::cerr << "LCU request timed out.\n";
        application.exit(2);
    });
    timeout.start(10000);

    QObject::connect(&summoner, &Janna::SummonerRepository::profileChanged, &application, [&application, &client, &summoner, &matchHistory] {
        std::cerr << "summoner-profile-valid=" << summoner.profile().isValid() << '\n';
        if (!summoner.profile().isValid()) {
            application.exit(1);
            return;
        }
        const QString encodedPuuid = QString::fromLatin1(QUrl::toPercentEncoding(summoner.profile().puuid));
        client.get("/lol-ranked/v1/ranked-stats/" + encodedPuuid, [](QJsonDocument ranked, QString rankedError) {
            const QJsonObject queueMap = ranked.object().value("queueMap").toObject();
            QStringList tiers;
            for (auto it = queueMap.constBegin(); it != queueMap.constEnd(); ++it) {
                const QJsonObject queue = it.value().toObject();
                tiers.append(it.key() + ':' + queue.value("queueType").toString()
                    + '/' + queue.value("tier").toString()
                    + '/' + queue.value("highestTier").toString()
                    + '/' + queue.value("ratedTier").toString()
                    + '/' + queue.value("division").toString()
                    + '/' + queue.value("ratedRating").toString());
            }
            std::cerr << "current-player-ranked-stats-error=" << !rankedError.isEmpty()
                      << " queue-count=" << queueMap.size()
                      << " tiers=" << tiers.join(',').toStdString() << '\n';
        });
        matchHistory.refresh(summoner.profile());
    });
    QObject::connect(&summoner, &Janna::SummonerRepository::loadingChanged, &application, [&application](bool loading, const QString &error) {
        if (!loading && !error.isEmpty()) {
            std::cerr << "summoner-profile-error=" << error.toStdString() << '\n';
            application.exit(1);
        }
    });
    QObject::connect(&matchHistory, &Janna::MatchRepository::matchesChanged, &application, [&application, &client, &matchHistory] {
        std::cerr << "match-summaries=" << matchHistory.matches().size() << '\n';
        if (matchHistory.matches().isEmpty()) {
            application.exit(1);
            return;
        }
        int summariesWithRune = 0;
        for (const Janna::MatchSummary &match : matchHistory.matches()) summariesWithRune += match.primaryRuneId > 0;
        client.get("/lol-game-data/assets/v1/perks.json", [&application, &matchHistory, summariesWithRune](QJsonDocument perks, QString perksError) {
            QHash<int, QString> runeMappings;
            int iconPathCount = 0;
            if (perksError.isEmpty()) {
                if (perks.isArray()) collectIconMappings(perks.array(), runeMappings, iconPathCount);
                if (perks.isObject()) collectIconMappings(perks.object(), runeMappings, iconPathCount);
            }
            std::cerr << "perk-asset-mappings=" << runeMappings.size() << " icon-paths=" << iconPathCount
                      << " summary-runes-present=" << summariesWithRune
                      << " root-array=" << perks.isArray() << " error=" << !perksError.isEmpty() << '\n';
            if (!perksError.isEmpty() || runeMappings.isEmpty()) {
                application.exit(1);
                return;
            }
            matchHistory.loadDetail(matchHistory.matches().constFirst().gameId);
        });
    });
    QObject::connect(&matchHistory, &Janna::MatchRepository::loadingChanged, &application, [&application](bool loading, const QString &error) {
        if (!loading && !error.isEmpty()) {
            std::cerr << "match-history-error=" << error.toStdString() << '\n';
            application.exit(1);
        }
    });
    QObject::connect(&selectedSummoner, &Janna::SummonerRepository::profileChanged, &application, [&application, &communityDragon, &selectedSummoner] {
        std::cerr << "selected-player-profile-valid=" << selectedSummoner.profile().isValid() << '\n';
        if (!selectedSummoner.profile().isValid()) {
            application.exit(1);
            return;
        }
        communityDragon.fetchStaticAsset("/latest/plugins/rcp-fe-lol-static-assets/global/default/images/ranked-emblem/emblem-gold.png",
            [&application](QByteArray emblem, QString error) {
                std::cerr << "rank-emblem-bytes=" << emblem.size() << " error=" << !error.isEmpty() << '\n';
                application.exit(error.isEmpty() && !emblem.isEmpty() ? 0 : 1);
            });
    });
    QObject::connect(&selectedSummoner, &Janna::SummonerRepository::loadingChanged, &application, [&application](bool loading, const QString &error) {
        if (!loading && !error.isEmpty()) {
            std::cerr << "selected-player-profile-error=" << error.toStdString() << '\n';
            application.exit(1);
        }
    });
    QObject::connect(&matchHistory, &Janna::MatchRepository::matchDetailChanged, &application, [&application, &client, &matchHistory, &selectedSummoner, &summoner, &rankEnrichmentObserved](const qint64 gameId) {
        const Janna::MatchDetail *detail = matchHistory.detail(gameId);
        const int participants = detail == nullptr ? 0 : detail->participants.size();
        const int teams = detail == nullptr ? 0 : detail->teams.size();
        int hiddenMatchHistories = 0;
        QStringList highestAchievedTiers;
        QStringList resolvedCurrentTiers;
        QStringList resolvedCurrentRankPoints;
        const Janna::MatchParticipant *selectableParticipant = nullptr;
        if (detail != nullptr) {
            for (const Janna::MatchParticipant &participant : detail->participants) {
                hiddenMatchHistories += participant.hiddenMatchHistory;
                highestAchievedTiers.append(participant.highestAchievedSeasonTier.trimmed().isEmpty()
                    ? "<empty>"
                    : participant.highestAchievedSeasonTier.trimmed());
                resolvedCurrentTiers.append(participant.currentRankTier.trimmed().isEmpty()
                    ? "<pending>"
                    : participant.currentRankTier.trimmed());
                resolvedCurrentRankPoints.append(participant.currentRankTier.trimmed().isEmpty()
                    ? "<pending>"
                    : QString::number(participant.currentRankLeaguePoints));
                if (selectableParticipant == nullptr && participant.puuid != summoner.profile().puuid
                    && (!participant.puuid.isEmpty() || !participant.summonerId.isEmpty())) {
                    selectableParticipant = &participant;
                }
            }
        }
        std::cerr << "match-detail-participants=" << participants << " teams=" << teams
                  << " hidden-match-histories=" << hiddenMatchHistories
                  << " highest-achieved-tiers=" << highestAchievedTiers.join(',').toStdString()
                  << " resolved-current-tiers=" << resolvedCurrentTiers.join(',').toStdString()
                  << " resolved-current-rank-points=" << resolvedCurrentRankPoints.join(',').toStdString() << '\n';
        if (participants != 10 || teams != 2 || selectableParticipant == nullptr) {
            application.exit(1);
            return;
        }
        if (!rankEnrichmentObserved) {
            rankEnrichmentObserved = true;
            std::cerr << "rank-enrichment-pending\n";
            return;
        }
        const QString participantPuuid = selectableParticipant->puuid;
        const QString participantSummonerId = selectableParticipant->summonerId;
        if (participantPuuid.isEmpty()) {
            selectedSummoner.loadPlayer(participantPuuid, participantSummonerId);
            return;
        }
        const QString encodedPuuid = QString::fromLatin1(QUrl::toPercentEncoding(participantPuuid));
        client.get("/lol-ranked/v1/ranked-stats/" + encodedPuuid,
                   [&client, &selectedSummoner, participantPuuid, participantSummonerId](QJsonDocument ranked, QString rankedError) {
            const QJsonObject queueMap = ranked.object().value("queueMap").toObject();
            QStringList tiers;
            for (auto it = queueMap.constBegin(); it != queueMap.constEnd(); ++it) {
                const QJsonObject queue = it.value().toObject();
                tiers.append(it.key() + ':' + queue.value("tier").toString() + '/' + queue.value("highestTier").toString()
                    + '/' + queue.value("ratedTier").toString() + '/' + queue.value("division").toString());
            }
            std::cerr << "other-player-ranked-stats-error=" << !rankedError.isEmpty()
                      << " queue-count=" << queueMap.size()
                      << " tiers=" << tiers.join(',').toStdString() << '\n';
            selectedSummoner.loadPlayer(participantPuuid, participantSummonerId);
        });
    });
    QObject::connect(&matchHistory, &Janna::MatchRepository::matchDetailLoadingChanged, &application, [&application](qint64, bool loading, const QString &error) {
        if (!loading && !error.isEmpty()) {
            std::cerr << "match-detail-error=" << error.toStdString() << '\n';
            application.exit(1);
        }
    });

    client.get("/lol-game-data/assets/v1/champion-summary.json", [&application, &client, &summoner](QJsonDocument champions, QString championError) {
        bool hasKhaZix = false;
        bool hasRell = false;
        for (const QJsonValue &value : champions.array()) {
            const int championId = value.toObject().value("id").toInt();
            hasKhaZix = hasKhaZix || championId == 121;
            hasRell = hasRell || championId == 526;
        }
        std::cerr << "champion-summary-count=" << champions.array().size()
                  << " has-121=" << hasKhaZix << " has-526=" << hasRell
                  << " error=" << !championError.isEmpty() << '\n';
        if (!championError.isEmpty() || !hasKhaZix || !hasRell) {
            application.exit(1);
            return;
        }
        QTimer::singleShot(200, &application, [&application, &client, &summoner] {
            client.getBytes("/lol-game-data/assets/v1/champion-icons/121.png", [&application, &client, &summoner](QByteArray khaZixBytes, QString khaZixError) {
                std::cerr << "champion-121-bytes=" << khaZixBytes.size() << " error=" << !khaZixError.isEmpty() << '\n';
                if (!khaZixError.isEmpty() || khaZixBytes.isEmpty()) {
                    application.exit(1);
                    return;
                }
                client.getBytes("/lol-game-data/assets/v1/champion-icons/526.png", [&application, &summoner](QByteArray rellBytes, QString rellError) {
                    std::cerr << "champion-526-bytes=" << rellBytes.size() << " error=" << !rellError.isEmpty() << '\n';
                    if (!rellError.isEmpty() || rellBytes.isEmpty()) {
                        application.exit(1);
                        return;
                    }
                    summoner.refresh();
                });
            });
        });
    });
    return application.exec();
}
