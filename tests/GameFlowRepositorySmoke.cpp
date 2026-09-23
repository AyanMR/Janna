#include "services/GameFlowRepository.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QTimer>

#include <algorithm>
#include <iostream>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == "--live") {
        Janna::GameFlowRepository repository;
        QObject::connect(&repository, &Janna::GameFlowRepository::snapshotChanged, &application, [&repository] {
            const Janna::GameFlowSnapshot &snapshot = repository.snapshot();
            std::cerr << "live phase=" << snapshot.phase.toStdString()
                      << " active=" << snapshot.isActive()
                      << " teams=" << snapshot.myTeam.size() << '/' << snapshot.theirTeam.size() << '\n';
        });
        QTimer::singleShot(5000, &application, &QCoreApplication::quit);
        repository.setClientAvailable(true);
        return application.exec();
    }
    const QJsonDocument gameFlow = QJsonDocument::fromJson(R"({
        "phase": "ChampSelect",
        "gameData": {
            "gameId": 987654321,
            "gameMode": "CLASSIC",
            "mapId": 11,
            "mapName": "召唤师峡谷",
            "queue": { "id": 420, "name": "单双排位" }
        }
    })");
    const QJsonDocument champSelect = QJsonDocument::fromJson(R"({
        "myTeam": [
            { "cellId": 0, "championId": 40, "championPickIntent": 40, "spell1Id": 4, "spell2Id": 14,
              "summonerId": 101, "gameName": "Janna", "tagLine": "CN1", "assignedPosition": "UTILITY" },
            { "cellId": 2, "championId": 0, "summonerId": 103, "gameName": "Ranked", "tagLine": "CN1", "puuid": "puuid-103" },
            { "cellId": 1, "championId": 0, "championPickIntent": 222, "summonerId": 102,
              "gameName": "Carry", "tagLine": "CN1", "assignedPosition": "BOTTOM" }
        ],
        "theirTeam": [
            { "cellId": 5, "championId": 238, "summonerId": 201, "gameName": "Zed", "tagLine": "CN1" }
        ],
        "bans": { "myTeamBans": [157], "theirTeamBans": [64] },
        "actions": [
            [{ "type": "ban", "actorCellId": 0, "championId": 157, "isAllyAction": true }],
            [{ "type": "pick", "actorCellId": 0, "championId": 40, "completed": true, "isInProgress": false }],
            [{ "type": "pick", "actorCellId": 1, "championId": 222, "completed": false, "isInProgress": true }]
        ],
        "timer": { "adjustedTimeLeftInPhase": 27500, "isInfinite": false }
    })");

    const Janna::GameFlowSnapshot snapshot = Janna::GameFlowRepository::parse(gameFlow, champSelect);
    const Janna::GameFlowSnapshot lobby = Janna::GameFlowRepository::parse(
        QJsonDocument::fromJson(R"({"phase":"Lobby","gameData":{}})"));
    const Janna::GameFlowSnapshot inProgress = Janna::GameFlowRepository::parse(
        QJsonDocument::fromJson(R"({
            "phase":"InProgress",
            "gameData":{
                "queueId":440,
                "queueName":"单双排位",
                "teamOne":{"players":[{"summonerId":301,"puuid":"p301","championId":1,"gameName":"Ally","tagLine":"CN1"}]},
                "teamTwo":{"members":[{"summonerId":401,"puuid":"p401","championId":2,"gameName":"Enemy","tagLine":"CN1","isPlayer":true}]}
            }
        })"));
    const Janna::GameFlowSnapshot spectator = Janna::GameFlowRepository::parse(
        QJsonDocument::fromJson(R"({
            "phase":"InProgress",
            "gameData":{"queueId":420}
        })"), QJsonDocument(), QJsonDocument::fromJson(R"({
            "gameData":{
                "gameId":7654321,
                "gameMode":"CLASSIC",
                "mapId":11,
                "queueId":420,
                "participants":[]
            },
            "participants":[
                {"summonerId":501,"puuid":"p501","championId":11,"teamId":100,"gameName":"Blue","tagLine":"CN1"},
                {"summonerId":601,"puuid":"p601","championId":22,"teamId":200,"gameName":"Red","tagLine":"CN1","isPlayer":true}
            ],
            "bannedChampions":[
                {"championId":157,"teamId":100},
                {"championId":64,"teamId":200}
            ]
        })"));
    const Janna::GameFlowSnapshot flattened = Janna::GameFlowRepository::parse(
        QJsonDocument::fromJson(R"({
            "phase":"InProgress",
            "gameId":111,
            "gameQueueConfigId":420,
            "participants":[
                {"summonerId":701,"teamId":100,"championId":3,"gameName":"FlatBlue","tagLine":"CN1"},
                {"summonerId":801,"teamId":200,"championId":4,
                 "riotId":{"gameName":"FlatRed","tagLine":"CN2"}}
            ],
            "bannedChampions":[
                {"championId":5,"teamId":100},
                {"championId":6,"teamId":200}
            ]
        })"));
    const Janna::GameFlowSnapshot nestedTeams = Janna::GameFlowRepository::parse(
        QJsonDocument::fromJson(R"({"phase":"InProgress"})"), QJsonDocument(),
        QJsonDocument::fromJson(R"({
            "gameId":222,
            "teamOne":{"players":[{"summonerId":901,"teamId":100,"championId":7}]},
            "teamTwo":{"players":[{"summonerId":902,"teamId":200,"championId":8}]}
        })"));
    const Janna::GameFlowSnapshot nestedPhase = Janna::GameFlowRepository::parse(
        QJsonDocument::fromJson(R"({
            "gameData":{"phase":"InProgress","participants":[
                {"summonerId":903,"teamId":100,"championId":9},
                {"summonerId":904,"teamId":200,"championId":10}
            ]}
        })"));
    const Janna::GameFlowSnapshot placeholderRoster = Janna::GameFlowRepository::parse(
        QJsonDocument::fromJson(R"({
            "phase":"InProgress",
            "gameData":{
                "teamOne":{"players":[{"summonerId":905}]},
                "teamTwo":{"players":[{"summonerId":906}]}
            }
        })"), QJsonDocument(),
        QJsonDocument::fromJson(R"({
            "participants":[
                {"summonerId":905,"teamId":100,"championId":11,"gameName":"RealBlue"},
                {"summonerId":906,"teamId":200,"championId":12,"gameName":"RealRed"}
            ]
        })"));
    const Janna::GameFlowSnapshot trainingBots = Janna::GameFlowRepository::parse(
        QJsonDocument::fromJson(R"({
            "phase":"InProgress",
            "gameData":{
                "gameId":333,
                "gameLength":42,
                "participants":[
                    {"teamId":100,"championId":1,"gameName":"Player","isPlayer":true,"kills":2,"deaths":1,"assists":5},
                    {"teamId":100,"championId":2,"gameName":"Ally1"},
                    {"teamId":100,"championId":3,"gameName":"Ally2"},
                    {"teamId":100,"championId":4,"gameName":"Ally3"},
                    {"teamId":100,"championId":5,"gameName":"Ally4"},
                    {"teamId":200,"championId":6,"isBot":true},
                    {"teamId":200,"championId":7,"bot":true},
                    {"teamId":200,"championId":8,"playerType":"AI"},
                    {"teamId":200,"championId":9,"isComputer":true},
                    {"teamId":200,"championId":10,"isAI":true}
                ]
            }
        })"));
    const bool valid = snapshot.isActive()
        && !lobby.isActive()
        && inProgress.isActive()
        && snapshot.phase == "ChampSelect"
        && snapshot.gameId == 987654321
        && snapshot.queueId == 420
        && snapshot.myTeam.size() == 3
        && snapshot.theirTeam.size() == 1
        && snapshot.myBans == QList<int>{157}
        && snapshot.theirBans == QList<int>{64}
        && snapshot.myTeam.at(0).riotId() == "Janna#CN1"
        && snapshot.myTeam.at(0).pickCompleted
        && snapshot.myTeam.at(1).championId == 222
        && snapshot.myTeam.at(1).pickInProgress
        && snapshot.secondsRemaining == 28
        && inProgress.queueId == 440
        && inProgress.myTeam.size() == 1
        && inProgress.theirTeam.size() == 1
        && inProgress.myTeam.constFirst().riotId() == "Enemy#CN1"
        && inProgress.theirTeam.constFirst().riotId() == "Ally#CN1"
        && spectator.isActive()
        && spectator.gameId == 7654321
        && spectator.myTeam.size() == 1
        && spectator.theirTeam.size() == 1
        && spectator.myTeam.constFirst().riotId() == "Red#CN1"
        && spectator.theirTeam.constFirst().riotId() == "Blue#CN1"
        && spectator.myBans == QList<int>{64}
        && spectator.theirBans == QList<int>{157};
    const bool extendedValid = flattened.isActive()
        && flattened.gameId == 111
        && flattened.queueId == 420
        && flattened.myTeam.size() == 1
        && flattened.theirTeam.size() == 1
        && flattened.myTeam.constFirst().riotId() == "FlatBlue#CN1"
        && flattened.theirTeam.constFirst().riotId() == "FlatRed#CN2"
        && flattened.myBans == QList<int>{5}
        && flattened.theirBans == QList<int>{6}
        && nestedTeams.myTeam.size() == 1
        && nestedTeams.theirTeam.size() == 1
        && nestedTeams.myTeam.constFirst().championId == 7
        && nestedTeams.theirTeam.constFirst().championId == 8
        && nestedPhase.isActive()
        && nestedPhase.myTeam.size() == 1
        && nestedPhase.theirTeam.size() == 1
        && placeholderRoster.myTeam.size() == 1
        && placeholderRoster.theirTeam.size() == 1
        && placeholderRoster.myTeam.constFirst().championId == 11
        && placeholderRoster.theirTeam.constFirst().championId == 12
        && trainingBots.isActive()
        && trainingBots.secondsRemaining == 42
        && trainingBots.myTeam.size() == 5
        && trainingBots.theirTeam.size() == 5
        && trainingBots.myTeam.constFirst().currentKdaAvailable
        && trainingBots.myTeam.constFirst().kills == 2
        && std::all_of(trainingBots.theirTeam.cbegin(), trainingBots.theirTeam.cend(), [](const Janna::GameFlowPlayer &player) {
            return player.isBot;
        });
    std::cerr << "gameflow active=" << snapshot.isActive()
              << " teams=" << snapshot.myTeam.size() << '/' << snapshot.theirTeam.size()
              << " bans=" << snapshot.myBans.size() << '/' << snapshot.theirBans.size()
              << " timer=" << snapshot.secondsRemaining
              << " spectator=" << spectator.gameId << ':' << spectator.myTeam.size() << '/'
              << spectator.theirTeam.size() << ':' << spectator.myBans.size() << '/'
              << spectator.theirBans.size() << ':'
              << (spectator.myTeam.isEmpty() ? "" : spectator.myTeam.constFirst().riotId().toStdString())
              << ':' << (spectator.theirTeam.isEmpty() ? "" : spectator.theirTeam.constFirst().riotId().toStdString())
              << " flattened=" << flattened.myTeam.size() << '/' << flattened.theirTeam.size()
              << " nested=" << nestedTeams.myTeam.size() << '/' << nestedTeams.theirTeam.size()
              << '\n';
    return valid && extendedValid ? 0 : 1;
}
