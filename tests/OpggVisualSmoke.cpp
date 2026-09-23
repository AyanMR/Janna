#include <QApplication>
#include <QDir>
#include <QImage>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QTableWidget>
#include <QToolButton>

#include "ui/OpggWindow.h"

#include "services/AssistController.h"
#include "services/ChampionRepository.h"
#include "services/GameFlowRepository.h"
#include "services/OpggCrawler.h"
#include "ui/ThemeController.h"

#include <iostream>

namespace Janna {

class OpggVisualSmokeAccess {
public:
    static QToolButton *backButton(OpggWindow &window) { return window.backButton_; }
    static QTableWidget *rankingTable(OpggWindow &window) { return window.rankingTable_; }
    static QLabel *laneStats(OpggWindow &window) { return window.detailLaneStats_; }
    static QLabel *role(OpggWindow &window) { return window.detailRole_; }
    static QLabel *version(OpggWindow &window) { return window.detailVersion_; }
    static QWidget *runeContent(OpggWindow &window) { return window.detailRunes_; }
    static QScrollArea *detailScroll(OpggWindow &window) { return window.detailScroll_; }
    static bool followsOwner(const OpggWindow &window) { return window.followOwner_; }
    static void renderRanking(OpggWindow &window, const QList<OpggChampionRank> &ranking)
    {
        window.renderRanking(ranking);
    }
    static void renderBuild(OpggWindow &window, const OpggBuild &build)
    {
        window.renderBuild(build);
    }
    static void showDetailPage(OpggWindow &window) { window.showDetailPage(); }
    static void showRankingPage(OpggWindow &window) { window.showRankingPage(); }
};

} // namespace Janna

namespace {

bool iconHasVisiblePixels(const QIcon &icon)
{
    const QImage image = icon.pixmap(22, 22).toImage().convertToFormat(QImage::Format_ARGB32);
    int visible = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 0) ++visible;
        }
    }
    return visible >= 20;
}

Janna::OpggBuild luxBuild()
{
    Janna::OpggBuild build;
    build.championId = 99;
    build.championName = QStringLiteral("拉克丝");
    build.championKey = QStringLiteral("lux");
    build.position = QStringLiteral("support");
    build.primaryStyleId = 8200;
    build.subStyleId = 8300;
    build.runeIds = {8214, 8226, 8210, 8237, 8304, 8347};
    build.statShardIds = {5008, 5008, 5001};
    build.summonerSpellIds = {4, 3};
    build.itemIds = {3865, 3871, 3010, 3114, 6617};
    build.roles = {QStringLiteral("AP输出"), QStringLiteral("辅助")};
    build.version = QStringLiteral("16.18");
    build.rankTier = QStringLiteral("emerald_plus");
    build.laneStats = {{QStringLiteral("support"), 75.0, 2600}, {QStringLiteral("mid"), 25.0, 830}};
    build.skillOrder = {QStringLiteral("E"), QStringLiteral("Q"), QStringLiteral("W"), QStringLiteral("E"), QStringLiteral("E"),
                        QStringLiteral("R"), QStringLiteral("E"), QStringLiteral("Q"), QStringLiteral("E"), QStringLiteral("Q"),
                        QStringLiteral("R"), QStringLiteral("Q"), QStringLiteral("Q"), QStringLiteral("W"), QStringLiteral("W")};

    Janna::OpggRuneBuild primary;
    primary.label = QStringLiteral("艾黎 · 推荐");
    primary.primaryStyleId = 8200;
    primary.subStyleId = 8300;
    primary.runeIds = build.runeIds;
    primary.statShardIds = build.statShardIds;
    primary.summonerSpellIds = build.summonerSpellIds;
    primary.primaryRows = {{8214, 8229, 8230}, {8224, 8226, 8275}, {8210, 8234, 8233}, {8237, 8236, 8232}};
    primary.subRows = {{8351, 8360, 8369}, {8304, 8306, 8313}, {8345, 8347, 8316}, {8321, 8339, 8352}};
    primary.statRows = {{5005, 5008, 5007}, {5008, 5002, 5003}, {5001, 5013, 5011}};
    build.runeBuilds.append(primary);

    Janna::OpggRuneBuild alternate = primary;
    alternate.label = QStringLiteral("彗星 · 备选");
    alternate.runeIds = {8229, 8226, 8210, 8237, 8304, 8347};
    build.runeBuilds.append(alternate);

    Janna::OpggItemBuild items;
    items.label = QStringLiteral("标准出装");
    items.starterItemIds = {3865};
    items.bootsItemIds = {3010};
    items.supportItemIds = {3871};
    items.coreItemIds = {3114, 6617};
    items.finalItemIds = {3107, 3157};
    items.itemIds = {3865, 3010, 3871, 3114, 6617, 3107, 3157};
    build.itemBuilds.append(items);

    build.spellBuilds.append({{4, 3}, 52.0, 81.0, 1200});
    build.counters = {
        {3, QStringLiteral("加里奥"), QStringLiteral("galio"), 60.67, 39.33, 500, QStringLiteral("counter")},
        {131, QStringLiteral("戴安娜"), QStringLiteral("diana"), 57.0, 43.0, 400, QStringLiteral("counter")}
    };
    build.weakCounters = build.counters;
    build.favorableCounters = {
        {40, QStringLiteral("迦娜"), QStringLiteral("janna"), 42.1, 57.9, 300, QStringLiteral("favorable")}
    };
    return build;
}

bool onlyOneSelectedPerRow(const Janna::OpggWindow &window)
{
    QHash<const QWidget *, int> selectedByRow;
    const auto options = window.findChildren<QWidget *>(QStringLiteral("opggRuneOption"));
    for (const QWidget *option : options) {
        if (!option->property("runeSelected").toBool()) continue;
        ++selectedByRow[option->parentWidget()];
    }
    for (auto it = selectedByRow.cbegin(); it != selectedByRow.cend(); ++it) {
        if (it.value() > 1) return false;
    }
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("JannaOpggVisualSmoke"));
    Janna::ThemeController theme;
    Janna::ChampionRepository champions;
    Janna::GameFlowRepository gameFlow;
    Janna::AssistController controller(champions.lcuClient(), gameFlow);
    Janna::OpggCrawler crawler;
    Janna::OpggWindow window(champions, controller, gameFlow, crawler);
    window.resize(820, 900);

    window.setFollowOwner(false);
    if (Janna::OpggVisualSmokeAccess::followsOwner(window)) {
        std::cerr << "OP.GG window did not detach from its owner\n";
        return 1;
    }
    window.setFollowOwner(true);

    if (!Janna::OpggVisualSmokeAccess::backButton(window)
        || !iconHasVisiblePixels(Janna::OpggVisualSmokeAccess::backButton(window)->icon())) {
        std::cerr << "back icon did not render\n";
        return 1;
    }

    const QStringList expectedHeaders = {QStringLiteral("排名"), QStringLiteral("英雄头像"), QStringLiteral("强度"),
                                         QStringLiteral("位置"), QStringLiteral("胜率"), QStringLiteral("选取率"),
                                         QStringLiteral("禁用率"), QStringLiteral("劣势对抗")};
    QStringList headers;
    QTableWidget *rankingTable = Janna::OpggVisualSmokeAccess::rankingTable(window);
    for (int column = 0; column < rankingTable->columnCount(); ++column) {
        headers.append(rankingTable->horizontalHeaderItem(column)->text());
    }
    if (headers != expectedHeaders) {
        std::cerr << "ranking headers do not match the compact layout\n";
        return 2;
    }

    Janna::OpggChampionRank rank;
    rank.championId = 99;
    rank.championName = QStringLiteral("拉克丝");
    rank.championKey = QStringLiteral("lux");
    rank.rank = 3;
    rank.tier = QStringLiteral("T1");
    rank.position = QStringLiteral("support");
    rank.winRate = 52.4;
    rank.pickRate = 7.5;
    rank.banRate = 1.1;
    rank.weakAgainst = {
        {3, QStringLiteral("加里奥"), QStringLiteral("galio"), 60.7, 39.3, 500, QStringLiteral("counter")},
        {131, QStringLiteral("戴安娜"), QStringLiteral("diana"), 57.0, 43.0, 400, QStringLiteral("counter")},
        {25, QStringLiteral("莫甘娜"), QStringLiteral("morgana"), 55.0, 45.0, 350, QStringLiteral("counter")}
    };
    Janna::OpggVisualSmokeAccess::renderRanking(window, {rank});
    if (rankingTable->rowCount() != 1 || !rankingTable->cellWidget(0, 7)
        || rankingTable->cellWidget(0, 7)->findChildren<QLabel *>().size() < 3) {
        std::cerr << "ranking row did not render its counter portrait cell\n";
        return 3;
    }

    Janna::OpggVisualSmokeAccess::renderBuild(window, luxBuild());
    Janna::OpggVisualSmokeAccess::showDetailPage(window);
    window.show();
    application.processEvents();

    if (!Janna::OpggVisualSmokeAccess::laneStats(window)->text().contains(QStringLiteral("辅助 75.0%"))
        || !Janna::OpggVisualSmokeAccess::laneStats(window)->text().contains(QStringLiteral("中路 25.0%"))
        || !Janna::OpggVisualSmokeAccess::role(window)->text().contains(QStringLiteral("AP输出"))
        || !Janna::OpggVisualSmokeAccess::version(window)->text().contains(QStringLiteral("16.18"))) {
        std::cerr << "detail metadata did not render\n";
        return 4;
    }
    if (!onlyOneSelectedPerRow(window)) {
        std::cerr << "more than one rune is highlighted in a row\n";
        return 5;
    }
    const auto runeOptions = window.findChildren<QWidget *>(QStringLiteral("opggRuneOption"));
    if (runeOptions.isEmpty()) {
        std::cerr << "rune rows were not rendered\n";
        return 6;
    }
    const QWidget *runeContent = Janna::OpggVisualSmokeAccess::runeContent(window);
    if (!runeContent || !runeContent->isVisible() || runeContent->sizeHint().height() <= 0) {
        std::cerr << "rune content layout is not visible\n";
        return 6;
    }
    for (const QWidget *option : runeOptions) {
        const QLabel *icon = option->findChild<QLabel *>(QStringLiteral("opggAssetIcon"));
        if (!option->isVisible() || option->geometry().width() <= 0 || option->geometry().height() <= 0
            || !icon || icon->mask().isEmpty()) {
            std::cerr << "rune icon is not clipped to a circle\n";
            return 7;
        }
    }
    QScrollArea *detailScroll = Janna::OpggVisualSmokeAccess::detailScroll(window);
    if (!detailScroll || !detailScroll->horizontalScrollBar()
        || detailScroll->horizontalScrollBar()->maximum() != 0) {
        std::cerr << "detail view has horizontal overflow\n";
        return 8;
    }

    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--capture")) {
        const QString output = QDir::current().absoluteFilePath(QStringLiteral("opgg-detail-smoke.png"));
        if (!window.grab().save(output)) {
            std::cerr << "could not save visual capture\n";
            return 9;
        }
        Janna::OpggVisualSmokeAccess::showRankingPage(window);
        application.processEvents();
        const QString rankingOutput = QDir::current().absoluteFilePath(QStringLiteral("opgg-ranking-smoke.png"));
        if (!window.grab().save(rankingOutput)) {
            std::cerr << "could not save ranking capture\n";
            return 10;
        }
        std::cout << "capture=" << output.toStdString() << '\n';
    }
    std::cout << "opgg visual smoke passed\n";
    return 0;
}
