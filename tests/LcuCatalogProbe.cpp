#include "services/LcuClient.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>

#include <iostream>

namespace {

QStringList keysFor(const QJsonObject &object)
{
    QStringList keys = object.keys();
    keys.sort(Qt::CaseInsensitive);
    return keys;
}

void printCatalog(const char *name, const QJsonDocument &document, const QString &error)
{
    const QJsonArray entries = document.isArray() ? document.array() : QJsonArray{};
    const QJsonObject first = entries.isEmpty() ? QJsonObject{} : entries.at(0).toObject();
    std::cerr << name << " error=" << !error.isEmpty() << " count=" << entries.size()
              << " fields=" << keysFor(first).join(',').toStdString() << '\n';
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    Janna::LcuClient client;

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &application, [&application] {
        std::cerr << "catalog probe timed out\n";
        application.exit(2);
    });
    timeout.start(15000);

    client.get("/lol-game-data/assets/v1/queues.json", [&application, &client](QJsonDocument queues, QString queueError) {
        printCatalog("queues", queues, queueError);
        client.get("/lol-game-data/assets/v1/maps.json", [&application, &client](QJsonDocument maps, QString mapError) {
            printCatalog("maps", maps, mapError);
            client.get("/lol-summoner/v1/current-summoner", [&application, &client](QJsonDocument profile, QString profileError) {
                const QString puuid = profile.object().value("puuid").toString();
                if (!profileError.isEmpty() || puuid.isEmpty()) {
                    std::cerr << "current-profile error=1\n";
                    application.exit(1);
                    return;
                }

                const QString encodedPuuid = QString::fromLatin1(QUrl::toPercentEncoding(puuid));
                client.get("/lol-ranked/v1/ranked-stats/" + encodedPuuid,
                    [&application, &client, encodedPuuid](QJsonDocument ranked, QString rankedError) {
                        const QJsonObject rankedRoot = ranked.object();
                        const QJsonObject queueMap = rankedRoot.value("queueMap").toObject();
                        const QJsonObject firstQueue = queueMap.isEmpty() ? QJsonObject{} : queueMap.constBegin().value().toObject();
                        std::cerr << "ranked error=" << !rankedError.isEmpty()
                                  << " root-fields=" << keysFor(rankedRoot).join(',').toStdString()
                                  << " queue-count=" << queueMap.size()
                                  << " queue-fields=" << keysFor(firstQueue).join(',').toStdString() << '\n';

                        client.get("/lol-match-history/v1/products/lol/" + encodedPuuid + "/matches?begIndex=0&endIndex=1",
                            [&application, &client](QJsonDocument history, QString historyError) {
                                const QJsonArray games = history.object().value("games").toObject().value("games").toArray();
                                if (!historyError.isEmpty() || games.isEmpty()) {
                                    std::cerr << "history error=1\n";
                                    application.exit(1);
                                    return;
                                }
                                const QJsonObject summaryParticipant = games.at(0).toObject().value("participants").toArray().isEmpty()
                                    ? QJsonObject{}
                                    : games.at(0).toObject().value("participants").toArray().at(0).toObject();
                                std::cerr << "summary-participant-fields=" << keysFor(summaryParticipant).join(',').toStdString()
                                          << " summary-timeline-fields=" << keysFor(summaryParticipant.value("timeline").toObject()).join(',').toStdString() << '\n';
                                const qint64 gameId = static_cast<qint64>(games.at(0).toObject().value("gameId").toDouble());
                                client.get("/lol-match-history/v1/games/" + QString::number(gameId),
                                    [&application, &client](QJsonDocument detail, QString detailError) {
                                        const QJsonArray identities = detail.object().value("participantIdentities").toArray();
                                        int emptyMatchHistoryUri = 0;
                                        int nonEmptyMatchHistoryUri = 0;
                                        int privacyFields = 0;
                                        QStringList playerFields;
                                        QStringList participantFields;
                                        QStringList participantStatsFields;
                                        QStringList timelineFields;
                                        for (const QJsonValue &value : identities) {
                                            const QJsonObject player = value.toObject().value("player").toObject();
                                            if (playerFields.isEmpty()) playerFields = keysFor(player);
                                            const QString uri = player.value("matchHistoryUri").toString();
                                            uri.isEmpty() ? ++emptyMatchHistoryUri : ++nonEmptyMatchHistoryUri;
                                            privacyFields += player.contains("privacy") || player.contains("private") || player.contains("isPrivate");
                                        }
                                        const QJsonArray participants = detail.object().value("participants").toArray();
                                        if (!participants.isEmpty()) {
                                            const QJsonObject participant = participants.at(0).toObject();
                                            participantFields = keysFor(participant);
                                            participantStatsFields = keysFor(participant.value("stats").toObject());
                                            timelineFields = keysFor(participant.value("timeline").toObject());
                                        }
                                        std::cerr << "identities error=" << !detailError.isEmpty()
                                                  << " count=" << identities.size()
                                                  << " empty-match-history-uri=" << emptyMatchHistoryUri
                                                  << " nonempty-match-history-uri=" << nonEmptyMatchHistoryUri
                                                  << " privacy-field-count=" << privacyFields
                                                  << " player-fields=" << playerFields.join(',').toStdString()
                                                  << " participant-fields=" << participantFields.join(',').toStdString()
                                                  << " stats-fields=" << participantStatsFields.join(',').toStdString()
                                                  << " timeline-fields=" << timelineFields.join(',').toStdString() << '\n';
                                        client.get("/lol-gameflow/v1/session", [&application, &client, detailError](QJsonDocument session, QString sessionError) {
                                            const QJsonObject root = session.object();
                                            const QJsonObject gameData = root.value("gameData").toObject();
                                            const QJsonObject queue = gameData.value("queue").toObject();
                                            std::cerr << "gameflow error=" << !sessionError.isEmpty()
                                                      << " error-text=" << sessionError.toStdString()
                                                      << " phase=" << root.value("phase").toString().toStdString()
                                                      << " game-data-fields=" << keysFor(gameData).join(',').toStdString()
                                                      << " map-id=" << gameData.value("mapId").toInt()
                                                      << " map-name=" << gameData.value("mapName").toString().toStdString()
                                                      << " queue-id=" << queue.value("id").toInt()
                                                      << " queue-name=" << queue.value("name").toString().toStdString()
                                                      << " queue-map=" << queue.value("map").toString().toStdString() << '\n';
                                            client.getBytes("/lol-gameflow/v1/gameflow-phase",
                                                [&application, detailError, sessionError](QByteArray phase, QString phaseError) {
                                                    std::cerr << "gameflow-phase error=" << !phaseError.isEmpty()
                                                              << " error-text=" << phaseError.toStdString()
                                                              << " value=" << phase.trimmed().toStdString() << '\n';
                                                    application.exit(detailError.isEmpty() && phaseError.isEmpty() ? 0 : 1);
                                                });
                                        });
                                    });
                            });
                    });
            });
        });
    });

    return application.exec();
}
