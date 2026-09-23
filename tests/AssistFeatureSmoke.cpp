#include "services/OpggCrawler.h"
#include "services/AssistController.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QTimer>

#include <algorithm>
#include <iostream>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    if (argc >= 2 && (QString::fromLocal8Bit(argv[1]) == QStringLiteral("--network")
                      || QString::fromLocal8Bit(argv[1]) == QStringLiteral("--network-normal")
                      || QString::fromLocal8Bit(argv[1]) == QStringLiteral("--network-aram"))) {
        const QString networkArgument = QString::fromLocal8Bit(argv[1]);
        const QString networkMode = networkArgument == QStringLiteral("--network-normal") ? QStringLiteral("NORMAL")
            : networkArgument == QStringLiteral("--network-aram") ? QStringLiteral("ARAM")
            : QStringLiteral("SOLORANKED");
        const QString networkPosition = networkMode == QStringLiteral("ARAM") ? QString{} : QStringLiteral("support");
        Janna::OpggCrawler crawler;
        QTimer::singleShot(60000, &application, [&application] { application.exit(10); });
        crawler.fetchChampionRanking(networkMode, networkPosition,
            [&application, &crawler, networkMode, networkPosition](QList<Janna::OpggChampionRank> ranking, QString error) {
                if (!error.isEmpty() || ranking.isEmpty()) {
                    std::cerr << "network ranking failed: " << error.toStdString() << '\n';
                    application.exit(11);
                    return;
                }
                crawler.fetchChampionBuild(40, networkMode, networkPosition,
                    [&application, ranking](Janna::OpggBuild build, QString buildError) {
                        if (!buildError.isEmpty() || build.primaryStyleId <= 0 || build.runeIds.isEmpty()) {
                            std::cerr << "network build failed: " << buildError.toStdString() << '\n';
                            application.exit(12);
                            return;
                        }
                        std::cout << "network opgg passed: ranking=" << ranking.size()
                                  << " runes=" << build.runeIds.size() << '\n';
                        application.exit(0);
                    }, "janna");
            });
        return application.exec();
    }
    if (argc >= 3) {
        QFile rankingFile(QString::fromLocal8Bit(argv[1]));
        QFile buildFile(QString::fromLocal8Bit(argv[2]));
        if (!rankingFile.open(QIODevice::ReadOnly) || !buildFile.open(QIODevice::ReadOnly)) return 2;
        QString liveError;
        const QByteArray rankingPayload = rankingFile.readAll();
        const QByteArray buildPayload = buildFile.readAll();
        const auto liveRanking = Janna::OpggCrawler::parseChampionRanking(rankingPayload, &liveError);
        if (liveRanking.isEmpty()) {
            std::cerr << "live ranking parser failed: " << liveError.toStdString() << '\n';
            return 3;
        }
        const auto liveBuild = Janna::OpggCrawler::parseBuild(buildPayload, &liveError);
        const auto supportLane = std::find_if(liveBuild.laneStats.cbegin(), liveBuild.laneStats.cend(),
                                              [](const Janna::OpggLaneStat &lane) {
            return lane.position == QStringLiteral("support");
        });
        const auto midLane = std::find_if(liveBuild.laneStats.cbegin(), liveBuild.laneStats.cend(),
                                          [](const Janna::OpggLaneStat &lane) {
            return lane.position == QStringLiteral("mid");
        });
        if (!liveBuild.isValid() || liveBuild.primaryStyleId <= 0 || liveBuild.runeIds.isEmpty()
            || liveBuild.summonerSpellIds.size() < 2
            || liveBuild.championId != 99
            || liveBuild.championKey.compare(QStringLiteral("lux"), Qt::CaseInsensitive) != 0
            || liveBuild.position != QStringLiteral("support")
            || liveBuild.roles != QStringList{QStringLiteral("AP输出"), QStringLiteral("辅助")}
            || supportLane == liveBuild.laneStats.cend() || midLane == liveBuild.laneStats.cend()
            || qAbs(supportLane->pickRate - 75.0) > 0.01
            || qAbs(midLane->pickRate - 25.0) > 0.01
            || (liveBuild.championId == 238
                && (!liveBuild.roles.contains(QStringLiteral("AD刺客"))
                    || !liveBuild.roles.contains(QStringLiteral("AD战士"))))) {
            std::cerr << "live build parser failed: " << liveError.toStdString() << '\n';
            return 4;
        }
        QString counterError;
        const auto counters = Janna::OpggCrawler::parseCounterPage(buildPayload, &counterError);
        if (!counterError.isEmpty() || counters.size() != 5
            || counters.constFirst().championKey != QStringLiteral("galio")
            || qAbs(counters.constFirst().championWinRate - 39.33) > 0.01
            || liveBuild.version != QStringLiteral("16.18")
            || liveBuild.laneStats.size() < 2
            || liveBuild.skillOrder.size() != 15
            || liveBuild.runeBuilds.size() < 2
            || liveBuild.runeBuilds.constFirst().primaryRows.size() != 4
            || liveBuild.runeBuilds.constFirst().primaryChoices.size() != 4
            || liveBuild.runeBuilds.constFirst().statChoices.size() != 3
            || liveBuild.runeBuilds.constFirst().primaryChoices.constFirst().constFirst().name.isEmpty()
            || liveBuild.itemBuilds.isEmpty()
            || liveBuild.itemBuilds.constFirst().starterGroups.isEmpty()
            || liveBuild.itemBuilds.constFirst().starterGroups.constFirst().choices.isEmpty()
            || liveBuild.itemBuilds.constFirst().starterGroups.constFirst().choices.constFirst().pickRate <= 0.0
            || liveBuild.spellBuilds.isEmpty()
            || liveBuild.spellBuilds.constFirst().pickRate <= 0.0
            || liveBuild.spellBuilds.constFirst().spellNames.constFirst().isEmpty()) {
            std::cerr << "live detail parser failed: " << counterError.toStdString() << '\n';
            return 5;
        }
        std::cout << "live opgg parser passed: ranking=" << liveRanking.size()
                  << " runes=" << liveBuild.runeIds.size()
                  << " spells=" << liveBuild.summonerSpellIds.at(0) << ',' << liveBuild.summonerSpellIds.at(1) << '\n';
        return 0;
    }
    QString error;
    const QList<Janna::OpggChampionRank> ranking = Janna::OpggCrawler::parseChampionRanking(
        R"({"data":[{"championId":40,"rank":1,"winRate":"55.0%","pickRate":0.12,"banRate":0.03,"games":1000}]})", &error);
    if (!error.isEmpty() || ranking.size() != 1 || ranking.constFirst().championId != 40
        || qAbs(ranking.constFirst().winRate - 55.0) > 0.01) {
        std::cerr << "ranking parser failed: " << error.toStdString() << '\n';
        return 1;
    }

    const auto localizedRolesRanking = Janna::OpggCrawler::parseChampionRanking(
        R"({"data":[{"championId":103,"roles":["MAGE"],"rank":1,"winRate":55.0}]})", &error);
    if (!error.isEmpty() || localizedRolesRanking.size() != 1
        || localizedRolesRanking.constFirst().roles != QStringList{QStringLiteral("AP输出")}) {
        std::cerr << "OP.GG ranking class role parser failed: " << error.toStdString() << '\n';
        return 1;
    }

    // The localized page is authoritative when its label differs from the
    // fallback code translation. This mirrors OP.GG's <select> Class filter.
    const auto markupLocalizedRanking = Janna::OpggCrawler::parseChampionRanking(
        R"(<select><option value="MAGE">页面法术输出</option></select><script id="__NEXT_DATA__" type="application/json">{"props":{"pageProps":{"data":[{"championId":103,"roles":["MAGE"],"rank":1,"winRate":55.0}]}}}</script>)",
        &error);
    if (!error.isEmpty() || markupLocalizedRanking.size() != 1
        || markupLocalizedRanking.constFirst().roles != QStringList{QStringLiteral("页面法术输出")}) {
        std::cerr << "OP.GG markup class label parser failed: " << error.toStdString() << '\n';
        return 1;
    }

    const Janna::OpggBuild build = Janna::OpggCrawler::parseBuild(
        R"({"data":{"championId":40,"primaryStyleId":8000,"subStyleId":8300,"runeIds":[8214,8226,8237],"statShardIds":[5005,5008,5001],"summonerSpellIds":[4,14]}})", &error);
    if (!error.isEmpty() || !build.isValid() || build.championId != 40 || build.primaryStyleId != 8000
        || build.runeIds.size() != 3 || build.summonerSpellIds.size() != 2) {
        std::cerr << "build parser failed: " << error.toStdString() << '\n';
        return 1;
    }

    const Janna::OpggBuild localizedRolesBuild = Janna::OpggCrawler::parseBuild(
        R"({"data":{"championId":238,"roles":["SLAYER","FIGHTER"],"primaryStyleId":8100,"subStyleId":8200,"runeIds":[8112,8143],"statShardIds":[5008,5001,5003]}})", &error);
    if (!error.isEmpty() || localizedRolesBuild.roles != QStringList{QStringLiteral("AD刺客"), QStringLiteral("AD战士")}) {
        std::cerr << "OP.GG class role parser failed: " << error.toStdString() << '\n';
        return 1;
    }

    const Janna::OpggBuild separatedRoleAndLane = Janna::OpggCrawler::parseBuild(
        R"({"data":{"championId":99,"roles":["MAGE","CONTROLLER"],"position":"support",
            "primaryStyleId":8200,"subStyleId":8300,"runeIds":[8229,8226]}})", &error);
    if (!error.isEmpty() || separatedRoleAndLane.position != QStringLiteral("support")
        || separatedRoleAndLane.roles != QStringList{QStringLiteral("AP输出"), QStringLiteral("辅助")}) {
        std::cerr << "role/lane fields were mixed by the build parser: " << error.toStdString() << '\n';
        return 1;
    }

    const Janna::OpggBuild nestedLaneBuild = Janna::OpggCrawler::parseBuild(
        R"({"data":{"championId":81,"roles":["AP刺客","AD战士"],
            "position":{"name":"ADC"},"primaryStyleId":8000,"runeIds":[8005,9111]}})", &error);
    if (!error.isEmpty() || nestedLaneBuild.position != QStringLiteral("adc")
        || nestedLaneBuild.roles != QStringList{QStringLiteral("AP刺客"), QStringLiteral("AD战士")}) {
        std::cerr << "nested lane or explicit role label parser failed: " << error.toStdString() << '\n';
        return 1;
    }

    const Janna::OpggBuild laneAliasBuild = Janna::OpggCrawler::parseBuild(
        R"({"data":{"championId":99,"position":"bottom-lane","roles":["MAGE"],"primaryStyleId":8200,"runeIds":[8229]}})", &error);
    if (!error.isEmpty() || laneAliasBuild.position != QStringLiteral("adc")
        || laneAliasBuild.roles != QStringList{QStringLiteral("AP输出")}) {
        std::cerr << "lane alias normalization failed: " << error.toStdString() << '\n';
        return 1;
    }

    const QList<Janna::OpggChampionRank> htmlRanking = Janna::OpggCrawler::parseChampionRanking(
        R"(<html><script id="__NEXT_DATA__" type="application/json">{"props":{"pageProps":{"data":[{"championId":1,"rank":2,"winRate":51.2}]}}}</script></html>)", &error);
    if (!error.isEmpty() || htmlRanking.size() != 1 || htmlRanking.constFirst().championId != 1) {
        std::cerr << "html parser failed: " << error.toStdString() << '\n';
        return 1;
    }

    const QList<Janna::OpggChampionRank> flightRanking = Janna::OpggCrawler::parseChampionRanking(
        R"(<script>self.__next_f.push([1,"21:[\"$\",\"$L27\",null,{\"data\":[{\"key\":\"malphite\",\"name\":\"Malphite\",\"positionName\":\"TOP\",\"positionWinRate\":51.5307,\"positionPickRate\":6.86157,\"positionBanRate\":15.2577,\"positionTierData\":{\"tier\":1,\"rank\":2},\"positionTier\":1,\"positionRank\":2,\"play\":2012}]}]"])</script>)", &error);
    if (!error.isEmpty() || flightRanking.size() != 1 || flightRanking.constFirst().championName != "Malphite"
        || flightRanking.constFirst().position != "top" || flightRanking.constFirst().rank != 2
        || flightRanking.constFirst().tier != "T1"
        || qAbs(flightRanking.constFirst().winRate - 51.5307) > 0.01
        || qAbs(flightRanking.constFirst().pickRate - 6.86157) > 0.01
        || flightRanking.constFirst().games != 2012) {
        std::cerr << "flight ranking parser failed: " << error.toStdString() << '\n';
        return 1;
    }

    const QList<Janna::OpggChampionRank> tierRanking = Janna::OpggCrawler::parseChampionRanking(
        R"({"data":[{"championId":1,"tier":"OP","winRate":55.0},{"championId":2,"positionTier":2,"rank":4,"winRate":52.0}]})", &error);
    if (!error.isEmpty() || tierRanking.size() != 2 || tierRanking.at(0).tier != "OP"
        || tierRanking.at(1).tier != "T2") {
        std::cerr << "tier normalization failed: " << error.toStdString() << '\n';
        return 1;
    }

    const QList<Janna::OpggChampionRank> metadataRanking = Janna::OpggCrawler::parseChampionRanking(
        R"({"data":[{"id":8229,"name":"Rune","rank":1},{"championKey":"janna","name":"Janna","positionTier":0,"rank":1}]})",
        &error);
    if (!error.isEmpty() || metadataRanking.size() != 1
        || metadataRanking.constFirst().championKey.compare("janna", Qt::CaseInsensitive) != 0
        || metadataRanking.constFirst().championId != 0
        || metadataRanking.constFirst().tier != "OP") {
        std::cerr << "metadata/rune filtering failed: " << error.toStdString() << '\n';
        return 1;
    }

    const Janna::OpggBuild flightBuild = Janna::OpggCrawler::parseBuild(
        R"(<script>self.__next_f.push([1,"42:{\"importClientData\":{\"type\":\"CHAMPION_DETAIL_BUILD\",\"championKey\":\"janna\",\"primaryStyleId\":8200,\"subStyleId\":8400,\"selectedPerkIds\":[8214,8226,8234,8236,8453,8463,5008,5010,5001]},\"summoner_spells\":[{\"metaId\":73,\"metaType\":\"spell\",\"name\":\"Exhaust\"},{\"metaId\":74,\"metaType\":\"spell\",\"name\":\"Flash\"}]}"])</script>)", &error);
    if (!error.isEmpty() || !flightBuild.isValid() || flightBuild.primaryStyleId != 8200
        || flightBuild.subStyleId != 8400 || flightBuild.runeIds.size() != 9
        || flightBuild.summonerSpellIds.size() < 2 || flightBuild.summonerSpellIds.at(0) != 3
        || flightBuild.summonerSpellIds.at(1) != 4) {
        std::cerr << "flight build parser failed: " << error.toStdString() << '\n';
        return 1;
    }

    const Janna::OpggBuild variantBuild = Janna::OpggCrawler::parseBuild(
        R"({"data":{"championId":40,"rune_pages":[{"play":100,"pick_rate":0.6,"builds":[
            {"primary_perk_style":{"id":8400},"perk_sub_style":{"id":8300},
             "main_runes":[[{"id":8439,"isActive":true}]],"sub_runes":[[{"id":8345,"isActive":true}]],
             "shards":[[{"id":5008,"isActive":true}],[{"id":5008,"isActive":true}],[{"id":5001,"isActive":true}]]},
            {"primary_perk_style":{"id":8400},"perk_sub_style":{"id":8100},
             "main_runes":[[{"id":8439,"isActive":true}]],"sub_runes":[[{"id":8105,"isActive":true}]],
             "shards":[[{"id":5007,"isActive":true}],[{"id":5010,"isActive":true}],[{"id":5011,"isActive":true}]]}
        ]}]}})", &error);
    if (!error.isEmpty() || variantBuild.runeBuilds.size() != 2
        || variantBuild.runeBuilds.at(0).statShardIds.size() != 3
        || variantBuild.runeBuilds.at(1).statShardIds.size() != 3
        || variantBuild.runeBuilds.at(0).subStyleId == variantBuild.runeBuilds.at(1).subStyleId) {
        std::cerr << "multi-rune build parser failed: " << error.toStdString() << '\n';
        return 1;
    }

    const Janna::OpggBuild itemFlightBuild = Janna::OpggCrawler::parseBuild(
        R"(<script>self.__next_f.push([1,"1:[\"$\",\"tr\",\"starter_items_0\",{\"children\":[{\"metaType\":\"item\",\"metaId\":2003}]}]"])</script>)",
        &error);
    if (!error.isEmpty() || itemFlightBuild.itemBuilds.isEmpty()
        || itemFlightBuild.itemBuilds.constFirst().starterItemIds != QList<int>{2003}) {
        std::cerr << "item order parser failed: " << error.toStdString() << '\n';
        return 1;
    }

    Janna::GameFlowSnapshot aram;
    aram.queueId = 450;
    aram.gameMode = "ARAM";
    if (Janna::AssistController::effectiveLane(aram) != "default"
        || !Janna::AssistController::usesDefaultSettings(aram)
        || Janna::AssistController::modeKey(aram) != "ARAM") {
        std::cerr << "default lane resolution failed\n";
        return 1;
    }

    Janna::GameFlowSnapshot classic;
    classic.gameMode = "CLASSIC";
    classic.myTeam.append({.assignedPosition = "UTILITY", .isLocalPlayer = true});
    if (Janna::AssistController::effectiveLane(classic) != "support"
        || Janna::AssistController::modeKey(classic) != "NORMAL") {
        std::cerr << "classic lane resolution failed\n";
        return 1;
    }

    const QList<Janna::ChampSelectAction> actions = Janna::AssistController::parseActions(
        QJsonDocument::fromJson(R"({"myTeam":[{"cellId":2}],"actions":[[
            {"id":"ban-1","type":"ban","actorCellId":2,"isAllyAction":true,"isInProgress":true},
            {"id":"pick-1","type":"pick","actorCellId":2,"isAllyAction":true,"isInProgress":false,"completed":false}
        ]]})"));
    if (actions.size() != 2 || actions.at(0).type != "ban" || actions.at(0).actorCellId != 2
        || !actions.at(0).inProgress || actions.at(1).type != "pick" || actions.at(1).inProgress) {
        std::cerr << "champ select action parser failed\n";
        return 1;
    }

    std::cout << "assist feature smoke passed\n";
    return 0;
}
