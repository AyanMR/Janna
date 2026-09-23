#include "ui/MainWindow.h"

#include "services/ChampionRepository.h"
#include "services/AssistController.h"
#include "services/GameDataRepository.h"
#include "services/GameFlowRepository.h"
#include "services/GamePathResolver.h"
#include "services/LeagueClientMonitor.h"
#include "services/MatchAnalytics.h"
#include "services/MatchRepository.h"
#include "services/OpggCrawler.h"
#include "services/RankedRepository.h"
#include "services/SearchHistoryStore.h"
#include "services/SummonerRepository.h"
#include "ui/AdaptiveComboBox.h"
#include "ui/AssistPage.h"
#include "ui/CenteredBar.h"
#include "ui/ChampionPicker.h"
#include "ui/LineIconButton.h"
#include "ui/MatchHistoryWidget.h"
#include "ui/MatchStatisticsWidget.h"
#include "ui/OpggWindow.h"
#include "ui/ProfileAvatar.h"
#include "ui/RecentChampionUsageWidget.h"
#include "ui/RoleBarChart.h"
#include "ui/ThemeController.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QHBoxLayout>
#include <QComboBox>
#include <QDir>
#include <QEasingCurve>
#include <QFileDialog>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPaintEvent>
#include <QParallelAnimationGroup>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QRegion>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QToolButton>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <functional>

namespace Janna {
namespace {
LineIconButton *tool(LineIconButton::Icon icon, const QString &tip)
{
    return new LineIconButton(icon, tip);
}

void addSoftShadow(QWidget *widget)
{
    auto *shadow = new QGraphicsDropShadowEffect(widget);
    shadow->setBlurRadius(18);
    shadow->setOffset(0, 4);
    shadow->setColor(QColor(0, 0, 0, 34));
    widget->setGraphicsEffect(shadow);
}

class ClickableRolePanel final : public QFrame {
public:
    void setActivatedHandler(std::function<void()> handler)
    {
        handler_ = std::move(handler);
        setCursor(Qt::PointingHandCursor);
    }

    void watch(QWidget *widget)
    {
        widget->installEventFilter(this);
        widget->setCursor(Qt::PointingHandCursor);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != this && event->type() == QEvent::MouseButtonRelease) {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton && handler_) {
                event->accept();
                handler_();
                return true;
            }
        }
        return QFrame::eventFilter(watched, event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QFrame::mouseReleaseEvent(event);
        if (event->button() == Qt::LeftButton && handler_) handler_();
    }

private:
    std::function<void()> handler_;
};

QWidget *championSearchResult(QWidget *parent, ChampionRepository &repository, const Champion &champion)
{
    auto *row = new QWidget(parent);
    row->setObjectName("championSearchResult");
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(10, 6, 10, 6);
    layout->setSpacing(8);

    auto *portrait = new QLabel(row);
    portrait->setObjectName("searchChampionPortrait");
    portrait->setFixedSize(28, 28);
    portrait->setAlignment(Qt::AlignCenter);
    const QPixmap icon = repository.portraitFor(champion.id);
    if (icon.isNull()) portrait->setText("?");
    else portrait->setPixmap(icon.scaled(portrait->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    layout->addWidget(portrait);

    auto *title = new QLabel(champion.title, row);
    title->setObjectName("searchChampionTitle");
    title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *name = new QLabel(champion.name, row);
    name->setObjectName("searchChampionName");
    name->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(title, 1);
    layout->addWidget(name);
    row->setToolTip(champion.name + "\n" + champion.title);
    return row;
}

void reveal(QWidget *widget)
{
    if (widget->isVisible()) return;
    widget->show();
    auto *effect = new QGraphicsOpacityEffect(widget);
    effect->setOpacity(0.0);
    widget->setGraphicsEffect(effect);
    auto *animation = new QPropertyAnimation(effect, "opacity", widget);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);
    animation->setDuration(140);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(animation, &QPropertyAnimation::finished, widget, [widget, effect] {
        if (widget->graphicsEffect() == effect) widget->setGraphicsEffect(nullptr);
    });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void transitionTo(QStackedWidget *stack, int nextIndex, int direction)
{
    if (nextIndex < 0 || nextIndex >= stack->count() || nextIndex == stack->currentIndex()) return;
    if (stack->property("jannaTransitionRunning").toBool()) return;

    const QPointer<QWidget> outgoing = stack->currentWidget();
    const QPointer<QWidget> incoming = stack->widget(nextIndex);
    if (!outgoing || !incoming) {
        stack->setCurrentIndex(nextIndex);
        return;
    }

    stack->setProperty("jannaTransitionRunning", true);
    stack->setCurrentIndex(nextIndex);
    const QPoint origin = incoming->pos();
    const QPoint offset(direction * 24, 0);
    outgoing->setGeometry(QRect(origin, incoming->size()));
    outgoing->show();
    incoming->show();
    outgoing->move(origin);
    incoming->move(origin + offset);
    outgoing->raise();
    incoming->raise();

    auto *outgoingOpacity = new QGraphicsOpacityEffect(outgoing);
    auto *incomingOpacity = new QGraphicsOpacityEffect(incoming);
    outgoingOpacity->setOpacity(1.0);
    incomingOpacity->setOpacity(0.0);
    outgoing->setGraphicsEffect(outgoingOpacity);
    incoming->setGraphicsEffect(incomingOpacity);

    auto *group = new QParallelAnimationGroup(stack);
    auto *fadeOutgoing = new QPropertyAnimation(outgoingOpacity, "opacity", group);
    fadeOutgoing->setStartValue(1.0);
    fadeOutgoing->setEndValue(0.0);
    auto *moveOutgoing = new QPropertyAnimation(outgoing, "pos", group);
    moveOutgoing->setStartValue(origin);
    moveOutgoing->setEndValue(origin - offset / 3);
    auto *fadeIncoming = new QPropertyAnimation(incomingOpacity, "opacity", group);
    fadeIncoming->setStartValue(0.0);
    fadeIncoming->setEndValue(1.0);
    auto *moveIncoming = new QPropertyAnimation(incoming, "pos", group);
    moveIncoming->setStartValue(origin + offset);
    moveIncoming->setEndValue(origin);
    for (QPropertyAnimation *animation : {fadeOutgoing, moveOutgoing, fadeIncoming, moveIncoming}) {
        animation->setDuration(190);
        animation->setEasingCurve(QEasingCurve::OutCubic);
        group->addAnimation(animation);
    }
    QObject::connect(group, &QParallelAnimationGroup::finished, stack, [stack, outgoing, incoming, origin] {
        if (outgoing) {
            outgoing->hide();
            outgoing->move(origin);
            outgoing->setGraphicsEffect(nullptr);
        }
        if (incoming) {
            incoming->move(origin);
            incoming->setGraphicsEffect(nullptr);
        }
        stack->setProperty("jannaTransitionRunning", false);
    });
    group->start(QAbstractAnimation::DeleteWhenStopped);
}

QWidget *clientPlaceholder(QLabel *&pathLabel)
{
    auto *page = new QFrame;
    page->setObjectName("clientPlaceholder");
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(52, 32, 52, 92);
    root->setSpacing(0);
    root->addStretch();

    auto *panel = new QFrame;
    panel->setObjectName("placeholderPanel");
    addSoftShadow(panel);
    panel->setMaximumWidth(440);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(32, 30, 32, 28);
    layout->setSpacing(10);
    auto *beacon = new QFrame;
    beacon->setObjectName("placeholderBeacon");
    auto *signal = new QLabel("LCU", beacon);
    signal->setObjectName("placeholderSignal");
    signal->setAlignment(Qt::AlignCenter);
    auto *beaconLayout = new QVBoxLayout(beacon);
    beaconLayout->setContentsMargins(0, 0, 0, 0);
    beaconLayout->addWidget(signal);
    layout->addWidget(beacon, 0, Qt::AlignHCenter);
    auto *title = new QLabel("未检测到英雄联盟客户端");
    title->setObjectName("placeholderTitle");
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);
    auto *detail = new QLabel("启动 LeagueClient.exe 后将自动连接");
    detail->setObjectName("placeholderDetail");
    detail->setAlignment(Qt::AlignCenter);
    layout->addWidget(detail);
    pathLabel = new QLabel;
    pathLabel->setObjectName("placeholderPath");
    pathLabel->setAlignment(Qt::AlignCenter);
    pathLabel->setWordWrap(true);
    layout->addSpacing(8);
    layout->addWidget(pathLabel);
    root->addWidget(panel, 0, Qt::AlignHCenter);
    root->addStretch();
    return page;
}

QWidget *dashboard(ChampionRepository &repository, SummonerRepository &summonerRepository, MatchRepository &matchRepository,
                   GameDataRepository &gameData, RankedRepository &rankedRepository,
                   std::function<void(const QString &, const QString &, const QString &)> playerSelectionHandler)
{
    auto *page = new QWidget;
    page->setObjectName("dashboard");
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(52, 32, 52, 92);
    root->setSpacing(28);

    auto *header = new QHBoxLayout;
    header->setSpacing(16);
    auto *avatar = new ProfileAvatar;
    header->addWidget(avatar);
    auto *name = new QVBoxLayout;
    name->setSpacing(5);
    auto *eyebrow = new QLabel("召唤师数据");
    eyebrow->setObjectName("eyebrow");
    auto *identity = new QHBoxLayout;
    identity->setContentsMargins(0, 0, 0, 0);
    identity->setSpacing(4);
    auto *summoner = new QLabel("正在读取账号数据");
    summoner->setObjectName("summonerName");
    auto *copy = tool(LineIconButton::Icon::Copy, "复制 Riot ID");
    identity->addWidget(summoner);
    identity->addWidget(copy);
    identity->addStretch();
    auto *subtitle = new QLabel("正在连接 LCU");
    subtitle->setObjectName("muted");
    name->addWidget(eyebrow);
    name->addLayout(identity);
    name->addWidget(subtitle);
    header->addLayout(name);
    header->addStretch();
    auto *roles = new ClickableRolePanel;
    roles->setObjectName("rolePanel");
    addSoftShadow(roles);
    auto *roleLayout = new QVBoxLayout(roles);
    roleLayout->setContentsMargins(15, 10, 15, 10);
    roleLayout->setSpacing(3);
    auto *roleTitle = new QLabel(roles);
    roleTitle->setObjectName("roleChartToggle");
    roleTitle->setText("常用位置");
    roleTitle->setToolTip("点击切换为常用英雄类型");
    roleTitle->setCursor(Qt::PointingHandCursor);
    auto *roleChart = new RoleBarChart;
    roles->watch(roleTitle);
    roles->watch(roleChart);
    roleLayout->addWidget(roleTitle, 0, Qt::AlignLeft);
    roleLayout->addWidget(roleChart);
    header->addWidget(roles);
    auto *refresh = tool(LineIconButton::Icon::Refresh, "刷新");
    header->addWidget(refresh, 0, Qt::AlignVCenter);
    root->addLayout(header);

    auto *stats = new MatchStatisticsWidget;
    addSoftShadow(stats);
    root->addWidget(stats);

    auto *recentSection = new QWidget;
    auto *recentSectionLayout = new QVBoxLayout(recentSection);
    recentSectionLayout->setContentsMargins(0, 0, 0, 0);
    recentSectionLayout->setSpacing(10);
    auto *recent = new QHBoxLayout;
    recent->setSpacing(10);
    auto *recentTitle = new QLabel("近期对局");
    recentTitle->setObjectName("sectionTitle");
    recent->addWidget(recentTitle);
    auto *range = new QLabel("最近");
    range->setObjectName("muted");
    recent->addWidget(range);
    auto *count = new AdaptiveComboBox;
    count->setObjectName("matchCountCombo");
    count->setMinimumContentsLength(5);
    count->setPopupMaximumVisibleItems(3);
    count->addItems({"20", "10", "5"});
    recent->addWidget(count);
    auto *matches = new QLabel("场");
    matches->setObjectName("muted");
    recent->addWidget(matches);
    recent->addSpacing(14);
    auto *summary = new QLabel("正在读取近期对局");
    summary->setObjectName("matchSummary");
    recent->addWidget(summary);
    recent->addStretch();
    auto *mode = new AdaptiveComboBox;
    mode->setObjectName("matchModeCombo");
    mode->setMinimumContentsLength(16);
    mode->setPopupMaximumVisibleItems(10);
    mode->setCollapsedPopupItemCount(1);
    mode->setPopupAbove(true);
    mode->setPopupAlignRight(true);
    mode->addItem("全部模式", 0);
    recent->addWidget(mode);
    recentSectionLayout->addLayout(recent);

    auto *recentChampionPanel = new QWidget;
    recentChampionPanel->setObjectName("recentChampionPanel");
    addSoftShadow(recentChampionPanel);
    auto *recentChampionLayout = new QHBoxLayout(recentChampionPanel);
    recentChampionLayout->setContentsMargins(10, 6, 10, 6);
    recentChampionLayout->setSpacing(8);
    auto *recentChampionTitle = new QLabel("近期英雄");
    recentChampionTitle->setObjectName("panelLabel");
    recentChampionLayout->addWidget(recentChampionTitle);
    auto *recentChampionUsage = new RecentChampionUsageWidget(repository);
    recentChampionLayout->addWidget(recentChampionUsage);
    recentChampionLayout->addStretch();
    auto *select = new QPushButton("选择英雄");
    select->setObjectName("secondary");
    recentChampionLayout->addWidget(select);
    recentSectionLayout->addWidget(recentChampionPanel);
    root->addWidget(recentSection);

    auto *history = new MatchHistoryWidget(repository, matchRepository, gameData);
    root->addWidget(history, 1);
    QObject::connect(history, &MatchHistoryWidget::playerSelected, page,
                     [playerSelectionHandler = std::move(playerSelectionHandler)](const QString &puuid, const QString &summonerId, const QString &riotId) {
                         if (playerSelectionHandler) playerSelectionHandler(puuid, summonerId, riotId);
                     });
    QObject::connect(refresh, &QToolButton::clicked, page, [&repository, &summonerRepository, &gameData] {
        gameData.refresh();
        repository.refresh();
        summonerRepository.refresh();
    });
    QObject::connect(copy, &QToolButton::clicked, page, [&summonerRepository] {
        const QString riotId = summonerRepository.profile().riotId();
        if (!riotId.isEmpty()) QApplication::clipboard()->setText(riotId);
    });

    const auto updateProfile = [avatar, summoner, subtitle, &repository, &summonerRepository] {
        const SummonerProfile &profile = summonerRepository.profile();
        if (!profile.isValid()) return;
        avatar->setProfile(repository.profileIconFor(profile.profileIconId), profile.level,
                           profile.experienceSinceLevel, profile.experienceLevelCap);
        const QString escapedName = profile.gameName.toHtmlEscaped();
        const QString escapedTagLine = profile.tagLine.toHtmlEscaped();
        summoner->setText(escapedTagLine.isEmpty() ? escapedName : escapedName + " <span>#" + escapedTagLine + "</span>");
        subtitle->setText("已连接 LCU");
    };
    QObject::connect(&repository, &ChampionRepository::profileIconAvailable, page, [&summonerRepository, updateProfile](const int profileIconId) {
        if (summonerRepository.profile().profileIconId == profileIconId) updateProfile();
    });
    QObject::connect(&summonerRepository, &SummonerRepository::loadingChanged, page, [subtitle](const bool loading, const QString &message) {
        if (loading) subtitle->setText("正在读取 LCU 账号数据");
        else if (!message.isEmpty()) subtitle->setText("账号数据读取失败");
    });

    const auto selectedChampionIds = std::make_shared<QSet<int>>();
    const auto showingChampionTypes = std::make_shared<bool>(false);
    const auto refreshModeOptions = [mode, &gameData, &matchRepository] {
        const int selectedQueueId = mode->currentData().toInt();
        QSignalBlocker blocker(mode);
        mode->clear();
        mode->addItem("全部模式", 0);

        // Keep the first-level entries data-driven: they are the distinct
        // queue IDs that actually occur in the latest match snapshot, in the
        // same order as those matches.  The catalog is only used to populate
        // the complete list under "更多" below.
        QSet<int> listedQueueIds;
        listedQueueIds.insert(0);
        for (const MatchSummary &match : matchRepository.matches()) {
            if (match.queueId <= 0 || listedQueueIds.contains(match.queueId)) continue;
            const QString label = gameData.modeName(match.queueId, match.mapId, match.gameMode).trimmed();
            if (label.isEmpty()) continue;
            mode->addItem(label, match.queueId);
            listedQueueIds.insert(match.queueId);
        }
        const int recentModeItemCount = mode->count();
        for (const GameQueue &queue : gameData.queues()) {
            if (queue.id <= 0 || listedQueueIds.contains(queue.id)) continue;
            const QString label = queue.name.trimmed();
            if (label.isEmpty()) continue;
            mode->addItem(label, queue.id);
            listedQueueIds.insert(queue.id);
        }
        mode->setCollapsedPopupItemCount(recentModeItemCount);
        const int restoredIndex = mode->findData(selectedQueueId);
        mode->setCurrentIndex(restoredIndex >= 0 ? restoredIndex : 0);
        mode->updateGeometry();
    };
    const auto applyMatchFilters = [history, summary, count, mode, recentChampionPanel, recentChampionUsage, roles, roleChart, roleTitle,
                                     stats, selectedChampionIds, showingChampionTypes, &repository, &gameData, &matchRepository,
                                     &rankedRepository] {
        const QList<MatchSummary> &allMatches = matchRepository.matches();
        const QList<ChampionUsage> championUsage = MatchAnalytics::recentChampionUsage(allMatches);
        recentChampionUsage->setUsage(championUsage);
        if (*showingChampionTypes) {
            roleTitle->setText("常用英雄类型");
            roleTitle->setToolTip("点击切换为常用位置");
            roles->setToolTip("点击切换为常用位置");
            roleChart->setToolTip("点击切换为常用位置");
            roleChart->setRoleUsage(MatchAnalytics::championTypeUsage(allMatches, repository));
        } else {
            roleTitle->setText("常用位置");
            roleTitle->setToolTip("点击切换为常用英雄类型");
            roles->setToolTip("点击切换为常用英雄类型");
            roleChart->setToolTip("点击切换为常用英雄类型");
            roleChart->setRoleUsage(MatchAnalytics::roleUsage(allMatches));
        }
        stats->setStatistics(MatchAnalytics::modeStatistics(allMatches, gameData, rankedRepository));

        const int requestedCount = count->currentText().toInt();
        const int selectedQueueId = mode->currentData().toInt();
        QList<MatchSummary> filtered;
        for (const MatchSummary &match : allMatches) {
            if (selectedQueueId != 0 && match.queueId != selectedQueueId) continue;
            if (!selectedChampionIds->isEmpty() && !selectedChampionIds->contains(match.championId)) continue;
            filtered.append(match);
            if (filtered.size() >= requestedCount) break;
        }
        int wins = 0;
        int losses = 0;
        int kills = 0;
        int deaths = 0;
        int assists = 0;
        for (const MatchSummary &match : filtered) {
            match.won ? ++wins : ++losses;
            kills += match.kills;
            deaths += match.deaths;
            assists += match.assists;
        }
        history->setMatches(std::move(filtered));
        if (wins + losses == 0) {
            summary->setText("暂无匹配的近期对局");
        } else {
            const double kda = static_cast<double>(kills + assists) / qMax(1, deaths);
            summary->setText("胜 " + QString::number(wins) + "     负 " + QString::number(losses)
                + "     KDA " + QString::number(kda, 'f', 1));
        }
    };
    QObject::connect(&summonerRepository, &SummonerRepository::profileChanged, page,
                     [&summonerRepository, &matchRepository, &rankedRepository, selectedChampionIds, recentChampionUsage, updateProfile] {
                         selectedChampionIds->clear();
                         recentChampionUsage->setSelectedChampionIds({});
                         updateProfile();
                         rankedRepository.refresh(summonerRepository.profile());
                         matchRepository.refresh(summonerRepository.profile());
                     });
    QObject::connect(select, &QPushButton::clicked, page, [&repository, page, selectedChampionIds, recentChampionUsage, applyMatchFilters] {
        ChampionPicker picker(repository, page);
        if (picker.exec() != QDialog::Accepted) return;
        QSet<int> selected;
        for (const int championId : picker.selectedChampionIds()) selected.insert(championId);
        *selectedChampionIds = std::move(selected);
        recentChampionUsage->setSelectedChampionIds(*selectedChampionIds);
        applyMatchFilters();
    });
    QObject::connect(recentChampionUsage, &RecentChampionUsageWidget::selectedChampionIdsChanged, page,
                      [selectedChampionIds, applyMatchFilters](const QSet<int> &selected) {
                          *selectedChampionIds = selected;
                          applyMatchFilters();
                      });
    roles->setActivatedHandler([showingChampionTypes, applyMatchFilters] {
        *showingChampionTypes = !*showingChampionTypes;
        applyMatchFilters();
    });
    QObject::connect(&matchRepository, &MatchRepository::matchesChanged, page, [refreshModeOptions, applyMatchFilters] {
        refreshModeOptions();
        applyMatchFilters();
    });
    QObject::connect(&gameData, &GameDataRepository::catalogChanged, page, [refreshModeOptions, applyMatchFilters] {
        refreshModeOptions();
        applyMatchFilters();
    });
    QObject::connect(&rankedRepository, &RankedRepository::statsChanged, page, applyMatchFilters);
    QObject::connect(&repository, &ChampionRepository::championsChanged, page, applyMatchFilters);
    QObject::connect(count, qOverload<int>(&QComboBox::currentIndexChanged), page, [applyMatchFilters](int) { applyMatchFilters(); });
    QObject::connect(mode, qOverload<int>(&QComboBox::currentIndexChanged), page,
                     [history, applyMatchFilters](int) {
                         // A mode change can retain the same game ids. Clear
                         // the old detail frame before filtering so its height
                         // cannot leak into the new result list.
                         history->collapseDetails();
                         applyMatchFilters();
                     });
    QObject::connect(&matchRepository, &MatchRepository::loadingChanged, page, [summary](const bool loading, const QString &message) {
        if (loading) summary->setText("正在读取近期对局");
        else if (!message.isEmpty()) summary->setText("近期对局读取失败：" + message);
    });
    updateProfile();
    refreshModeOptions();
    applyMatchFilters();
    return page;
}

void clearLayout(QLayout *layout)
{
    if (!layout) return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (item->layout()) clearLayout(item->layout());
        if (QWidget *widget = item->widget()) widget->deleteLater();
        delete item;
    }
}

QString gameFlowPhaseText(const QString &phase)
{
    if (phase.compare("ReadyCheck", Qt::CaseInsensitive) == 0) return "准备确认";
    if (phase.compare("ChampSelect", Qt::CaseInsensitive) == 0) return "英雄选择";
    if (phase.compare("GameStart", Qt::CaseInsensitive) == 0) return "进入游戏";
    if (phase.compare("InProgress", Qt::CaseInsensitive) == 0) return "对局进行中";
    if (phase.compare("Reconnect", Qt::CaseInsensitive) == 0) return "重新连接";
    if (phase.compare("WaitingForStats", Qt::CaseInsensitive) == 0) return "等待结算";
    if (phase.compare("PreEndOfGame", Qt::CaseInsensitive) == 0) return "对局即将结束";
    if (phase.compare("EndOfGame", Qt::CaseInsensitive) == 0) return "对局结束";
    if (phase.compare("Lobby", Qt::CaseInsensitive) == 0) return "大厅";
    return phase.trimmed().isEmpty() ? "等待游戏流程" : phase;
}

QString championNameFor(const ChampionRepository &repository, const int championId)
{
    if (championId <= 0) return "未选择英雄";
    for (const Champion &champion : repository.champions()) {
        if (champion.id == championId) return champion.name;
    }
    return "英雄 " + QString::number(championId);
}

QString positionText(const QString &position)
{
    const QString normalized = position.trimmed().toUpper();
    if (normalized == "TOP") return "上路";
    if (normalized == "JUNGLE") return "打野";
    if (normalized == "MIDDLE" || normalized == "MID") return "中路";
    if (normalized == "BOTTOM" || normalized == "BOT") return "下路";
    if (normalized == "UTILITY" || normalized == "SUPPORT") return "辅助";
    return position.trimmed();
}

QString localizedTierText(const QString &tier)
{
    const QString normalized = tier.trimmed().toUpper();
    if (normalized == "IRON") return "黑铁";
    if (normalized == "BRONZE") return "黄铜";
    if (normalized == "SILVER") return "白银";
    if (normalized == "GOLD") return "黄金";
    if (normalized == "PLATINUM") return "铂金";
    if (normalized == "EMERALD") return "翡翠";
    if (normalized == "DIAMOND") return "钻石";
    if (normalized == "MASTER") return "大师";
    if (normalized == "GRANDMASTER") return "宗师";
    if (normalized == "CHALLENGER") return "王者";
    return tier.trimmed();
}

QLabel *flowPortrait(QWidget *parent, ChampionRepository &repository, const int championId, const QSize size = {42, 42})
{
    auto *portrait = new QLabel(parent);
    portrait->setObjectName("gameFlowPortrait");
    portrait->setFixedSize(size);
    portrait->setAlignment(Qt::AlignCenter);
    const QPixmap pixmap = repository.portraitFor(championId);
    if (!pixmap.isNull()) portrait->setPixmap(pixmap.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    else portrait->setText(championId > 0 ? QString::number(championId) : "?");
    portrait->setToolTip(championNameFor(repository, championId));
    return portrait;
}

QWidget *gameFlowSpellIcons(QWidget *parent, ChampionRepository &repository, const int spell1Id, const int spell2Id)
{
    auto *spells = new QWidget(parent);
    spells->setObjectName("gameFlowSpellIcons");
    auto *layout = new QHBoxLayout(spells);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    for (const int spellId : {spell1Id, spell2Id}) {
        auto *icon = new QLabel(spells);
        icon->setObjectName("gameFlowSpellIcon");
        icon->setFixedSize(22, 22);
        icon->setAlignment(Qt::AlignCenter);
        icon->setToolTip(repository.summonerSpellNameFor(spellId));
        const QPixmap pixmap = repository.summonerSpellIconFor(spellId);
        if (!pixmap.isNull()) icon->setPixmap(pixmap.scaled(icon->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        else icon->setText(spellId > 0 ? QString::number(spellId) : "-");
        layout->addWidget(icon);
    }
    return spells;
}

using GameFlowPlayerHandler = std::function<void(const QString &, const QString &, const QString &)>;

QWidget *gameFlowPlayerRow(QWidget *parent, ChampionRepository &repository, const GameFlowPlayer &player,
                           const bool ownTeam, const GameFlowPlayerHandler &playerHandler)
{
    auto *row = new QFrame(parent);
    row->setObjectName(ownTeam ? "gameFlowPlayerRowOwn" : "gameFlowPlayerRowEnemy");
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(9);
    layout->addWidget(flowPortrait(row, repository, player.championId));

    auto *details = new QVBoxLayout;
    details->setContentsMargins(0, 0, 0, 0);
    details->setSpacing(2);
    QString playerName = player.riotId().trimmed();
    if (playerName.isEmpty()) playerName = player.isBot ? "人机" : "等待玩家信息";
    auto *name = new QToolButton(row);
    name->setText(playerName);
    name->setObjectName("gameFlowPlayerName");
    name->setToolButtonStyle(Qt::ToolButtonTextOnly);
    name->setAutoRaise(true);
    name->setEnabled(!player.summonerId.isEmpty() || !player.puuid.isEmpty());
    name->setCursor(name->isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    name->setToolTip(playerName);
    name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    if (name->isEnabled() && playerHandler) {
        QObject::connect(name, &QToolButton::clicked, name, [playerHandler, player] {
            playerHandler(player.puuid, player.summonerId, player.riotId());
        });
    }
    details->addWidget(name);

    const QString position = positionText(player.assignedPosition);
    QString state;
    if (player.championId > 0) state = championNameFor(repository, player.championId);
    else if (player.championPickIntent > 0) state = "意向：" + championNameFor(repository, player.championPickIntent);
    else state = "尚未选择英雄";
    if (!position.isEmpty()) state = position + " · " + state;
    if (player.pickInProgress) state += " · 选择中";
    else if (player.pickCompleted) state += " · 已锁定";
    auto *status = new QLabel(state, row);
    status->setObjectName("gameFlowPlayerStatus");
    status->setToolTip(state);
    status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    details->addWidget(status);
    layout->addLayout(details, 1);

    auto *rankColumn = new QWidget(row);
    rankColumn->setObjectName("gameFlowRankColumn");
    rankColumn->setFixedWidth(54);
    auto *rankColumnLayout = new QVBoxLayout(rankColumn);
    rankColumnLayout->setContentsMargins(0, 0, 0, 0);
    rankColumnLayout->setSpacing(0);
    auto *rank = new QLabel(rankColumn);
    rank->setObjectName("gameFlowRank");
    rank->setFixedHeight(30);
    rank->setAlignment(Qt::AlignCenter);
    auto *kda = new QLabel(rankColumn);
    kda->setObjectName("gameFlowPlayerKda");
    kda->setAlignment(Qt::AlignCenter);
    kda->setText(player.currentKdaAvailable
        ? QString::number(player.kills) + "/" + QString::number(player.deaths) + "/" + QString::number(player.assists)
        : "-/-/-");
    kda->setToolTip("当前对局 K/D/A");
    rankColumnLayout->addWidget(rank);
    rankColumnLayout->addWidget(kda);
    if (player.rank.isValid()) {
        const QPixmap emblem = repository.rankEmblemFor(player.rank.tier);
        if (!emblem.isNull()) rank->setPixmap(emblem.scaled({30, 30}, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        const QString rankTooltip = "段位：" + localizedTierText(player.rank.tier)
            + (player.rank.division.trimmed().isEmpty() ? QString{} : " " + player.rank.division.trimmed())
            + "\n胜点：" + QString::number(qMax(0, player.rank.leaguePoints));
        rank->setToolTip(rankTooltip);
        rankColumn->setToolTip(rankTooltip);
    } else {
        rank->setText("未定级");
        rank->setToolTip("未定级");
        rankColumn->setToolTip("未定级");
    }
    layout->addWidget(rankColumn);

    if (player.spell1Id > 0 || player.spell2Id > 0) {
        layout->addWidget(gameFlowSpellIcons(row, repository, player.spell1Id, player.spell2Id));
    }
    return row;
}

QFrame *gameFlowTeamPanel(QWidget *parent, ChampionRepository &repository, const QString &title,
                          const QList<GameFlowPlayer> &players, const QList<int> &bans, const bool ownTeam,
                          const GameFlowPlayerHandler &playerHandler)
{
    auto *panel = new QFrame(parent);
    panel->setObjectName(ownTeam ? "gameFlowTeamOwn" : "gameFlowTeamEnemy");
    addSoftShadow(panel);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(14, 12, 14, 14);
    layout->setSpacing(8);
    auto *header = new QHBoxLayout;
    auto *label = new QLabel(title, panel);
    label->setObjectName("gameFlowTeamTitle");
    header->addWidget(label);
    header->addStretch();
    auto *count = new QLabel(QString::number(players.size()) + " 人", panel);
    count->setObjectName("muted");
    header->addWidget(count);
    layout->addLayout(header);

    if (players.isEmpty()) {
        auto *empty = new QLabel("等待 LCU 返回阵容", panel);
        empty->setObjectName("empty");
        empty->setAlignment(Qt::AlignCenter);
        empty->setMinimumHeight(180);
        layout->addWidget(empty);
    } else {
        for (const GameFlowPlayer &player : players) layout->addWidget(gameFlowPlayerRow(panel, repository, player, ownTeam, playerHandler));
    }

    if (!bans.isEmpty()) {
        auto *banTitle = new QLabel("禁用", panel);
        banTitle->setObjectName("gameFlowBanTitle");
        layout->addWidget(banTitle);
        auto *banLayout = new QHBoxLayout;
        banLayout->setSpacing(5);
        for (const int championId : bans) banLayout->addWidget(flowPortrait(panel, repository, championId, {28, 28}));
        banLayout->addStretch();
        layout->addLayout(banLayout);
    }
    return panel;
}

QFrame *gameFlowRecentPanel(QWidget *parent, ChampionRepository &repository, GameDataRepository &gameData,
                            const QList<GameFlowPlayer> &players, const GameFlowPlayerHandler &playerHandler)
{
    auto *panel = new QFrame(parent);
    panel->setObjectName("gameFlowRecentPanel");
    addSoftShadow(panel);
    auto *root = new QVBoxLayout(panel);
    root->setContentsMargins(14, 12, 14, 14);
    root->setSpacing(8);
    auto *header = new QHBoxLayout;
    auto *title = new QLabel("最近 10 场对局", panel);
    title->setObjectName("gameFlowTeamTitle");
    header->addWidget(title);
    header->addStretch();
    auto *hint = new QLabel("点击玩家可查看召唤师数据", panel);
    hint->setObjectName("muted");
    header->addWidget(hint);
    root->addLayout(header);

    auto *columns = new QHBoxLayout;
    columns->setSpacing(7);
    if (players.isEmpty()) {
        auto *empty = new QLabel("等待当前队伍数据", panel);
        empty->setObjectName("empty");
        empty->setAlignment(Qt::AlignCenter);
        columns->addWidget(empty, 1);
    } else {
        for (const GameFlowPlayer &player : players) {
            auto *column = new QFrame(panel);
            column->setObjectName("gameFlowRecentColumn");
            auto *columnLayout = new QVBoxLayout(column);
            columnLayout->setContentsMargins(5, 7, 5, 7);
            columnLayout->setSpacing(5);
            auto *playerIcon = flowPortrait(column, repository, player.championId, {30, 30});
            columnLayout->addWidget(playerIcon, 0, Qt::AlignHCenter);
            auto *playerName = new QToolButton(column);
            playerName->setText(player.riotId().isEmpty() ? (player.isBot ? "人机" : "未知玩家") : player.riotId());
            playerName->setObjectName("gameFlowRecentPlayer");
            playerName->setToolButtonStyle(Qt::ToolButtonTextOnly);
            playerName->setAutoRaise(true);
            playerName->setEnabled(!player.summonerId.isEmpty() || !player.puuid.isEmpty());
            playerName->setCursor(playerName->isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
            playerName->setToolTip(playerName->text());
            if (playerName->isEnabled() && playerHandler) {
                QObject::connect(playerName, &QToolButton::clicked, playerName, [playerHandler, player] {
                    playerHandler(player.puuid, player.summonerId, player.riotId());
                });
            }
            columnLayout->addWidget(playerName);
            if (player.isBot) {
                auto *botHistory = new QFrame(column);
                botHistory->setObjectName("gameFlowRecentMatchPending");
                botHistory->setFixedHeight(46);
                auto *botLayout = new QVBoxLayout(botHistory);
                botLayout->setContentsMargins(3, 3, 3, 3);
                auto *botLabel = new QLabel("人机\n无近期战绩", botHistory);
                botLabel->setObjectName("gameFlowRecentMatchText");
                botLabel->setAlignment(Qt::AlignCenter);
                botLayout->addWidget(botLabel);
                columnLayout->addWidget(botHistory);
                columnLayout->addStretch();
                columns->addWidget(column, 1);
                continue;
            }
            for (int index = 0; index < 10; ++index) {
                auto *match = new QFrame(column);
                const bool hasMatch = index < player.recentMatches.size();
                match->setObjectName(hasMatch
                    ? (player.recentMatches.at(index).won ? "gameFlowRecentMatchWin" : "gameFlowRecentMatchLoss")
                    : "gameFlowRecentMatchPending");
                match->setFixedHeight(46);
                auto *matchLayout = new QHBoxLayout(match);
                matchLayout->setContentsMargins(4, 3, 4, 3);
                matchLayout->setSpacing(4);
                QLabel *matchLabel = nullptr;
                if (hasMatch) {
                    const GameFlowMatch &recent = player.recentMatches.at(index);
                    matchLabel = new QLabel(match);
                    const QPixmap icon = repository.portraitFor(recent.championId);
                    if (!icon.isNull()) matchLabel->setPixmap(icon.scaled({18, 18}, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
                    else matchLabel->setText("?");
                    matchLabel->setToolTip(championNameFor(repository, recent.championId));
                    matchLabel->setFixedSize(18, 18);
                } else {
                    matchLabel = new QLabel(player.recentMatchesLoading ? "读取中" : "—", match);
                }
                matchLabel->setObjectName("gameFlowRecentMatchText");
                matchLabel->setAlignment(Qt::AlignCenter);
                if (hasMatch) {
                    const GameFlowMatch &recent = player.recentMatches.at(index);
                    matchLayout->addWidget(matchLabel, 0, Qt::AlignVCenter);
                    auto *meta = new QVBoxLayout;
                    meta->setContentsMargins(0, 0, 0, 0);
                    meta->setSpacing(0);
                    auto *kda = new QLabel(QString::number(recent.kills) + "/" + QString::number(recent.deaths)
                        + "/" + QString::number(recent.assists), match);
                    kda->setObjectName("gameFlowRecentMatchText");
                    kda->setAlignment(Qt::AlignCenter);
                    meta->addWidget(kda);
                    QString mode = recent.queueName.trimmed();
                    if (mode.isEmpty() && recent.queueId > 0) mode = gameData.modeName(recent.queueId, 0, recent.gameMode).trimmed();
                    if (mode.isEmpty()) mode = recent.gameMode.trimmed();
                    if (mode.isEmpty()) mode = "未知模式";
                    const QString when = recent.createdAt.isValid()
                        ? recent.createdAt.toLocalTime().toString("MM-dd HH:mm") : "时间未知";
                    QString duration;
                    if (recent.durationSeconds > 0) {
                        duration = QString(" · %1:%2").arg(recent.durationSeconds / 60, 2, 10, QLatin1Char('0'))
                            .arg(recent.durationSeconds % 60, 2, 10, QLatin1Char('0'));
                    }
                    auto *modeLabel = new QLabel(mode + "\n" + when, match);
                    modeLabel->setObjectName("gameFlowRecentMatchMeta");
                    modeLabel->setAlignment(Qt::AlignCenter);
                    modeLabel->setWordWrap(true);
                    modeLabel->setToolTip(mode + "\n" + when + duration);
                    meta->addWidget(modeLabel);
                    matchLayout->addLayout(meta, 1);
                } else {
                    matchLayout->addWidget(matchLabel, 1, Qt::AlignVCenter);
                }
                columnLayout->addWidget(match);
            }
            columnLayout->addStretch();
            columns->addWidget(column, 1);
        }
    }
    root->addLayout(columns, 1);
    return panel;
}

class GameFlowPage final : public QWidget {
public:
    GameFlowPage(ChampionRepository &repository, GameDataRepository &gameData,
                 GameFlowPlayerHandler playerHandler, QWidget *parent = nullptr)
        : QWidget(parent), repository_(repository), gameData_(gameData), playerHandler_(std::move(playerHandler))
    {
        setObjectName("dashboard");
        root_ = new QVBoxLayout(this);
        root_->setContentsMargins(52, 32, 52, 92);
        root_->setSpacing(18);

        auto *header = new QHBoxLayout;
        header->setSpacing(12);
        heading_ = new QLabel("对局信息", this);
        heading_->setObjectName("heading");
        header->addWidget(heading_);
        status_ = new QLabel("等待游戏流程", this);
        status_->setObjectName("statusPill");
        header->addWidget(status_);
        header->addStretch();
        queue_ = new QLabel(this);
        queue_->setObjectName("gameFlowMeta");
        header->addWidget(queue_);
        timer_ = new QLabel(this);
        timer_->setObjectName("gameFlowTimer");
        header->addWidget(timer_);
        root_->addLayout(header);

        auto *tabs = new QWidget(this);
        auto *tabsLayout = new QHBoxLayout(tabs);
        tabsLayout->setContentsMargins(0, 0, 0, 0);
        tabsLayout->setSpacing(2);
        allyTab_ = new QPushButton("友方", tabs);
        enemyTab_ = new QPushButton("敌方", tabs);
        allyTab_->setObjectName("tab");
        enemyTab_->setObjectName("tab");
        allyTab_->setCheckable(true);
        enemyTab_->setCheckable(true);
        allyTab_->setChecked(true);
        auto *tabGroup = new QButtonGroup(tabs);
        tabGroup->setExclusive(true);
        tabGroup->addButton(allyTab_);
        tabGroup->addButton(enemyTab_);
        tabsLayout->addWidget(allyTab_);
        tabsLayout->addWidget(enemyTab_);
        tabsLayout->addStretch();
        root_->addWidget(tabs);
        connect(allyTab_, &QPushButton::clicked, this, [this] {
            showEnemy_ = false;
            rebuildTeams();
        });
        connect(enemyTab_, &QPushButton::clicked, this, [this] {
            if (!enemyTab_->isEnabled()) return;
            showEnemy_ = true;
            rebuildTeams();
        });

        subtitle_ = new QLabel("进入英雄选择或游戏后，这里会自动显示当前对局阵容", this);
        subtitle_->setObjectName("muted");
        root_->addWidget(subtitle_);

        teams_ = new QHBoxLayout;
        teams_->setSpacing(14);
        root_->addLayout(teams_, 1);
        connect(&repository_, &ChampionRepository::championPortraitsChanged, this, [this] {
            setSnapshot(snapshot_);
        });
        connect(&repository_, &ChampionRepository::championsChanged, this, [this] {
            setSnapshot(snapshot_);
        });
        connect(&repository_, &ChampionRepository::staticIconsChanged, this, [this] {
            setSnapshot(snapshot_);
        });
        connect(&gameData_, &GameDataRepository::catalogChanged, this, [this] {
            setSnapshot(snapshot_);
        });
        setSnapshot(GameFlowSnapshot{});
    }

    void setSnapshot(const GameFlowSnapshot &snapshot)
    {
        snapshot_ = snapshot;
        status_->setText(gameFlowPhaseText(snapshot.phase));
        QString queueText = snapshot.queueName.trimmed();
        if (queueText.isEmpty() && snapshot.queueId > 0) queueText = gameData_.queueName(snapshot.queueId);
        if (queueText.isEmpty()) queueText = "尚未进入对局";
        queue_->setText(queueText);
        updateTimerText();
        subtitle_->setText(snapshot.isActive()
            ? (snapshot.myTeam.isEmpty() ? "正在读取当前对局阵容" : "当前对局阵容与选择状态")
            : "进入英雄选择或游戏后，这里会自动显示当前对局阵容");
        const bool enemyAvailable = snapshot.phase.compare("GameStart", Qt::CaseInsensitive) == 0
            || snapshot.phase.compare("InProgress", Qt::CaseInsensitive) == 0
            || snapshot.phase.compare("Reconnect", Qt::CaseInsensitive) == 0
            || snapshot.phase.compare("WaitingForStats", Qt::CaseInsensitive) == 0
            || snapshot.phase.compare("PreEndOfGame", Qt::CaseInsensitive) == 0
            || snapshot.phase.compare("EndOfGame", Qt::CaseInsensitive) == 0;
        enemyTab_->setEnabled(enemyAvailable);
        if (!enemyAvailable) {
            showEnemy_ = false;
            allyTab_->setChecked(true);
        }
        rebuildTeams();
    }

    void updateTimer(const int seconds)
    {
        snapshot_.secondsRemaining = seconds;
        updateTimerText();
    }

private:
    void rebuildTeams()
    {
        clearLayout(teams_);
        const QList<GameFlowPlayer> &players = showEnemy_ ? snapshot_.theirTeam : snapshot_.myTeam;
        const QList<int> &bans = showEnemy_ ? snapshot_.theirBans : snapshot_.myBans;
        teams_->addWidget(gameFlowTeamPanel(this, repository_, showEnemy_ ? "敌方阵容" : "友方阵容",
                                            players, bans, !showEnemy_, playerHandler_), 0);
        teams_->addWidget(gameFlowRecentPanel(this, repository_, gameData_, players, playerHandler_), 1);
        teams_->invalidate();
        teams_->activate();
    }

    void updateTimerText()
    {
        if (snapshot_.secondsRemaining >= 0) {
            timer_->setText(QString("%1:%2").arg(snapshot_.secondsRemaining / 60, 2, 10, QLatin1Char('0'))
                                           .arg(snapshot_.secondsRemaining % 60, 2, 10, QLatin1Char('0')));
        } else {
            timer_->clear();
        }
    }

    ChampionRepository &repository_;
    GameDataRepository &gameData_;
    GameFlowPlayerHandler playerHandler_;
    GameFlowSnapshot snapshot_;
    QVBoxLayout *root_{};
    QHBoxLayout *teams_{};
    QLabel *heading_{};
    QLabel *status_{};
    QLabel *queue_{};
    QLabel *timer_{};
    QLabel *subtitle_{};
    QPushButton *allyTab_{};
    QPushButton *enemyTab_{};
    bool showEnemy_{};
};

GameFlowPage *bpPage(ChampionRepository &repository, GameDataRepository &gameData,
                     GameFlowPlayerHandler playerHandler)
{
    return new GameFlowPage(repository, gameData, std::move(playerHandler));
}
}

MainWindow::MainWindow()
    : champions_(std::make_unique<ChampionRepository>()),
      summoner_(std::make_unique<SummonerRepository>(champions_->lcuClient())),
      matches_(std::make_unique<MatchRepository>(champions_->lcuClient())),
      gameData_(std::make_unique<GameDataRepository>(champions_->lcuClient())),
      ranked_(std::make_unique<RankedRepository>(champions_->lcuClient())),
      searchHistory_(std::make_unique<SearchHistoryStore>()),
      monitor_(std::make_unique<LeagueClientMonitor>()),
      gameFlow_(std::make_unique<GameFlowRepository>()),
      assistController_(std::make_unique<AssistController>(champions_->lcuClient(), *gameFlow_)),
      opgg_(std::make_unique<OpggCrawler>()),
      theme_(std::make_unique<ThemeController>())
{
    setWindowTitle("Janna"); setObjectName("window"); setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    // A stylesheet radius only affects painting; it does not clip child
    // widgets.  Use a translucent top-level plus a mask so all four corners,
    // including the lower corners behind the bottom bar, are truly rounded.
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_StyledBackground);
    setFocusPolicy(Qt::StrongFocus); setMinimumSize(1060, 720); resize(1260, 850);
    auto *root = new QVBoxLayout(this); root->setContentsMargins(0, 0, 0, 0); root->setSpacing(0);
    auto *topBar = new CenteredBar; topBar->setObjectName("topBar"); topBar->setFixedHeight(62); topBar->setContentsMargins(28, 0, 16, 0);
    auto *brandGroup = new QWidget(topBar); auto *top = new QHBoxLayout(brandGroup); top->setContentsMargins(0, 0, 0, 0); top->setSpacing(10);
    auto *brandMark = new QLabel("J"); brandMark->setObjectName("brandMark"); brandMark->setAlignment(Qt::AlignCenter); top->addWidget(brandMark);
    auto *brand = new QLabel("JANNA"); brand->setObjectName("brand"); top->addWidget(brand); auto *divider = new QFrame; divider->setObjectName("headerDivider"); divider->setFrameShape(QFrame::VLine); top->addWidget(divider); auto *context = new QLabel("LEAGUE COMPANION"); context->setObjectName("context"); top->addWidget(context);
    searchShell_ = new QFrame(topBar); searchShell_->setObjectName("searchShell"); searchShell_->setFixedWidth(386); searchShell_->installEventFilter(this); auto *searchLayout = new QHBoxLayout(searchShell_); searchLayout->setContentsMargins(7, 0, 12, 0); searchLayout->setSpacing(2); auto *searchIcon = tool(LineIconButton::Icon::Search, "搜索"); searchIcon->setEnabled(false); searchLayout->addWidget(searchIcon);
    search_ = new QLineEdit; search_->setObjectName("globalSearch"); search_->setFocusPolicy(Qt::StrongFocus); search_->setPlaceholderText("搜索召唤师、对局或英雄"); searchLayout->addWidget(search_);
    auto *windowControls = new QWidget(topBar); auto *controls = new QHBoxLayout(windowControls); controls->setContentsMargins(0, 0, 0, 0); controls->setSpacing(4);
    auto *minimize = tool(LineIconButton::Icon::Minimize, "最小化"); auto *close = tool(LineIconButton::Icon::Close, "关闭"); close->setObjectName("closeButton"); controls->addWidget(minimize); controls->addWidget(close);
    topBar->setLeadingWidget(brandGroup); topBar->setCenterWidget(searchShell_); topBar->setTrailingWidget(windowControls); root->addWidget(topBar);
    suggestions_ = new QFrame(this); suggestions_->setObjectName("suggestions");
    suggestions_->setAttribute(Qt::WA_StyledBackground);
    suggestions_->setAutoFillBackground(true);
    suggestions_->setMinimumHeight(56);
    suggestionLayout_ = new QVBoxLayout(suggestions_); suggestionLayout_->setContentsMargins(14, 8, 14, 8); suggestions_->hide();
    applicationPages_ = new QStackedWidget;
    applicationPages_->setObjectName("applicationPages");
    applicationPages_->addWidget(clientPlaceholder(gamePathLabel_));
    auto *workspace = new QWidget;
    workspace->setObjectName("featureWorkspace");
    auto *workspaceLayout = new QVBoxLayout(workspace);
    workspaceLayout->setContentsMargins(0, 0, 0, 0);
    workspaceLayout->setSpacing(0);
    pages_ = new QStackedWidget;
    auto *scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    // Reserve the scrollbar gutter so expanding a match cannot change the
    // dashboard width when its content first exceeds the viewport.
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(dashboard(*champions_, *summoner_, *matches_, *gameData_, *ranked_, [this](const QString &puuid, const QString &summonerId, const QString &riotId) {
        openPlayerProfile(puuid, summonerId, riotId);
    }));
    pages_->addWidget(scroll);
    auto *gameFlowPage = bpPage(*champions_, *gameData_, [this](const QString &puuid, const QString &summonerId, const QString &riotId) {
        openPlayerProfile(puuid, summonerId, riotId);
    });
    auto *gameFlowScroll = new QScrollArea;
    gameFlowScroll->setObjectName("gameFlowScroll");
    gameFlowScroll->setFrameShape(QFrame::NoFrame);
    gameFlowScroll->setWidgetResizable(true);
    gameFlowScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    gameFlowScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    gameFlowScroll->setWidget(gameFlowPage);
    pages_->addWidget(gameFlowScroll);
    auto *assistPage = new AssistPage(*champions_, *gameData_, *gameFlow_, *assistController_, *opgg_);
    auto *assistScroll = new QScrollArea;
    assistScroll->setObjectName("assistScroll");
    assistScroll->setFrameShape(QFrame::NoFrame);
    assistScroll->setWidgetResizable(true);
    assistScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    assistScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    assistScroll->setWidget(assistPage);
    pages_->addWidget(assistScroll);
    workspaceLayout->addWidget(pages_);
    applicationPages_->addWidget(workspace);
    root->addWidget(applicationPages_, 1);

    opggWindow_ = new OpggWindow(*champions_, *assistController_, *gameFlow_, *opgg_, this);
    opggWindow_->setSnapshot(gameFlow_->snapshot());
    connect(opggWindow_, &OpggWindow::buildReady, assistPage, &AssistPage::setRecommendedBuild);
    connect(opggWindow_, &OpggWindow::buildFailed, assistPage, &AssistPage::setRecommendedBuildError);
    auto *bottomBar = new CenteredBar; bottomBar->setObjectName("bottomBar"); bottomBar->setFixedHeight(60); bottomBar->setContentsMargins(28, 8, 24, 10);
    auto *navigation = new QWidget(bottomBar); auto *bottom = new QHBoxLayout(navigation); bottom->setContentsMargins(0, 0, 0, 0); bottom->setSpacing(7);
    auto *summoner = tool(LineIconButton::Icon::Profile, "召唤师数据");
    auto *match = tool(LineIconButton::Icon::Match, "对局信息");
    auto *assist = tool(LineIconButton::Icon::Assist, "辅助功能");
    auto *favorite = tool(LineIconButton::Icon::Favorite, "收藏的对局");
    summoner->setCheckable(true);
    match->setCheckable(true);
    assist->setCheckable(true);
    summoner->setChecked(true);
    bottom->addWidget(summoner);
    bottom->addWidget(match);
    bottom->addWidget(assist);
    bottom->addWidget(favorite);
    auto *trailingControls = new QWidget(bottomBar); auto *trailing = new QHBoxLayout(trailingControls); trailing->setContentsMargins(0, 0, 0, 0); trailing->setSpacing(7);
    auto *opgg = tool(LineIconButton::Icon::Opgg, "OP.GG");
    trailing->addWidget(opgg);
    trailing->addWidget(tool(LineIconButton::Icon::Notice, "公告"));
    settingsButton_ = tool(LineIconButton::Icon::Settings, "设置");
    trailing->addWidget(settingsButton_);
    bottomBar->setCenterWidget(navigation); bottomBar->setTrailingWidget(trailingControls);
    root->addWidget(bottomBar);
    searchAnimation_ = new QVariantAnimation(this);
    searchAnimation_->setDuration(300);
    searchAnimation_->setEasingCurve(QEasingCurve::OutCubic);
    connect(searchAnimation_, &QVariantAnimation::valueChanged, this, [this, topBar](const QVariant &value) {
        searchShell_->setFixedWidth(value.toInt());
        topBar->updateLayout();
        if (suggestions_->isVisible()) updateSuggestionGeometry();
    });
    summonerNavigationButton_ = summoner;
    matchNavigationButton_ = match;
    assistNavigationButton_ = assist;
    // OPGG is a public read-only view and remains available without an LCU
    // connection; an empty snapshot naturally falls back to NORMAL/default.
    clientControls_ = {search_, summoner, match, assist, favorite};
    qApp->installEventFilter(this);
    connect(minimize, &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(close, &QToolButton::clicked, this, &QWidget::close);
    connect(summoner, &QToolButton::clicked, this, [this, summoner, match] {
        transitionTo(pages_, 0, -1);
        summoner->setChecked(true);
        match->setChecked(false);
        if (assistNavigationButton_) assistNavigationButton_->setChecked(false);
    });
    connect(match, &QToolButton::clicked, this, [this, summoner, match] {
        transitionTo(pages_, 1, 1);
        match->setChecked(true);
        summoner->setChecked(false);
        if (assistNavigationButton_) assistNavigationButton_->setChecked(false);
    });
    connect(assist, &QToolButton::clicked, this, [this, summoner, match, assist] {
        transitionTo(pages_, 2, 1);
        assist->setChecked(true);
        summoner->setChecked(false);
        match->setChecked(false);
    });
    connect(opgg, &QToolButton::clicked, this, [this] {
        if (!opggWindow_) return;
        opggWindow_->setSnapshot(gameFlow_->snapshot());
        opggWindow_->showFor(this);
        opggWindow_->refreshRanking();
    });
    connect(search_, &QLineEdit::textChanged, this, &MainWindow::updateSearch);
    connect(settingsButton_, &QToolButton::clicked, this, &MainWindow::showSettingsMenu);
    const auto refreshSearchSuggestions = [this] {
        if (!search_->text().trimmed().isEmpty()) updateSearch(search_->text());
    };
    connect(champions_.get(), &ChampionRepository::championsChanged, this, refreshSearchSuggestions);
    connect(champions_.get(), &ChampionRepository::championPortraitsChanged, this, refreshSearchSuggestions);
    connect(monitor_.get(), &LeagueClientMonitor::availabilityChanged, this, &MainWindow::setClientAvailable);
    connect(gameFlow_.get(), &GameFlowRepository::snapshotChanged, this, [this, gameFlowPage] {
        gameFlowPage->setSnapshot(gameFlow_->snapshot());
    });
    connect(gameFlow_.get(), &GameFlowRepository::snapshotChanged, this, [assistPage, this] {
        assistPage->setSnapshot(gameFlow_->snapshot());
    });
    connect(gameFlow_.get(), &GameFlowRepository::snapshotChanged, this, [this] {
        if (opggWindow_) opggWindow_->setSnapshot(gameFlow_->snapshot());
    });
    connect(gameFlow_.get(), &GameFlowRepository::activityObserved, this, [this](const bool active) {
        const bool wasActive = gameFlowActive_;
        gameFlowActive_ = active;
        if (active) {
            // Navigation into the match page is driven by the ChampSelect
            // edge below.  Reconnect/InProgress updates must not steal focus
            // from whatever page the user is viewing.
        } else if (wasActive) {
            transitionTo(pages_, 0, -1);
            if (summonerNavigationButton_) summonerNavigationButton_->setChecked(true);
            if (matchNavigationButton_) matchNavigationButton_->setChecked(false);
            if (assistNavigationButton_) assistNavigationButton_->setChecked(false);
        }
    });
    connect(gameFlow_.get(), &GameFlowRepository::champSelectEntered, this, [this] {
        if (!gameFlowActive_) return;
        ensureGameFlowPage();
        if (summonerNavigationButton_) summonerNavigationButton_->setChecked(false);
        if (matchNavigationButton_) matchNavigationButton_->setChecked(true);
        if (assistNavigationButton_) assistNavigationButton_->setChecked(false);
    });
    connect(gameFlow_.get(), &GameFlowRepository::remainingTimeChanged, this, [gameFlowPage](const int seconds) {
        gameFlowPage->updateTimer(seconds);
    });
    connect(monitor_.get(), &LeagueClientMonitor::availabilityChanged, assistController_.get(), &AssistController::setClientAvailable);
    updateGamePathPresentation();
    setClientAvailable(LeagueClientMonitor::isLeagueClientRunning());
    gameFlow_->setClientAvailable(clientAvailable_);
    monitor_->start();

    // Prime the static catalogs and the default Korean OP.GG ranking on the
    // first event-loop turn.  This keeps the detail view usable immediately
    // after launch, including when League Client is still starting.
    QTimer::singleShot(0, this, [this] {
        if (champions_) champions_->warmStaticAssets();
        if (opggWindow_) opggWindow_->refreshRanking();
    });
}

MainWindow::~MainWindow() = default;

void MainWindow::ensureGameFlowPage()
{
    if (!gameFlowActive_ || !gameFlow_ || !pages_ || pages_->count() < 2
        || gameFlow_->snapshot().phase.compare("ChampSelect", Qt::CaseInsensitive) != 0) return;
    if (!isVisible()) {
        // The first LCU response can arrive while MainWindow is still being
        // constructed.  Defer the transition until the stacked pages have
        // their real layout geometry.
        QTimer::singleShot(0, this, [this] {
            if (gameFlowActive_ && gameFlow_ && gameFlow_->snapshot().phase.compare("ChampSelect", Qt::CaseInsensitive) == 0) {
                ensureGameFlowPage();
            }
        });
        return;
    }
    if (pages_->currentIndex() == 1) return;

    // A game-flow update can arrive while the overview animation is still
    // running.  Keep the animated transition, but retry after it has had a
    // chance to release its guard instead of silently dropping navigation.
    transitionTo(pages_, 1, 1);
    QTimer::singleShot(260, this, [this] {
        if (gameFlowActive_ && gameFlow_ && gameFlow_->snapshot().phase.compare("ChampSelect", Qt::CaseInsensitive) == 0
            && pages_ && pages_->currentIndex() != 1) {
            transitionTo(pages_, 1, 1);
        }
    });
    QTimer::singleShot(720, this, [this] {
        if (!gameFlowActive_ || !gameFlow_ || gameFlow_->snapshot().phase.compare("ChampSelect", Qt::CaseInsensitive) != 0
            || !pages_ || pages_->currentIndex() == 1) return;
        // A stale animation guard should never strand the user on the
        // overview.  Clean up the two page widgets before the direct fallback.
        pages_->setProperty("jannaTransitionRunning", false);
        if (QWidget *overview = pages_->widget(0)) {
            overview->hide();
            overview->setGraphicsEffect(nullptr);
        }
        if (QWidget *gamePage = pages_->widget(1)) {
            gamePage->show();
            gamePage->setGraphicsEffect(nullptr);
        }
        pages_->setCurrentIndex(1);
    });
}

void MainWindow::openPlayerProfile(const QString &puuid, const QString &summonerId, const QString &riotId)
{
    if (!clientAvailable_ || (puuid.isEmpty() && summonerId.isEmpty())) return;
    viewingExternalProfile_ = true;
    {
        QSignalBlocker blocker(search_);
        search_->setText(riotId.trimmed());
    }
    searchHistory_->record({riotId, puuid, summonerId});
    suggestions_->hide();
    setSearchExpanded(false);
    transitionTo(pages_, 0, -1);
    summoner_->loadPlayer(puuid, summonerId);
}

void MainWindow::setClientAvailable(const bool available)
{
    for (QWidget *control : clientControls_) control->setEnabled(available);
    if (!available) {
        viewingExternalProfile_ = false;
        setSearchExpanded(false);
        search_->clear();
        suggestions_->hide();
        // OPGG is a public read-only view and may be used while the League
        // client is starting, reconnecting, or temporarily unavailable.
        // Keep its independent window open; the next snapshot will fall back
        // to the mode/default lane when the client state is cleared.
    }
    updateGamePathPresentation();
    if (clientAvailable_ == available && applicationPages_->currentIndex() == (available ? 1 : 0)) return;

    const bool wasAvailable = clientAvailable_;
    clientAvailable_ = available;
    if (gameFlow_) gameFlow_->setClientAvailable(available);
    if (assistController_) assistController_->setClientAvailable(available);
    transitionTo(applicationPages_, available ? 1 : 0, available || !wasAvailable ? 1 : -1);
    if (available) {
        gameData_->refresh();
        summoner_->refresh();
        champions_->refresh();
    }
}

void MainWindow::setSearchExpanded(const bool expanded)
{
    if (searchExpanded_ == expanded) return;
    searchExpanded_ = expanded;
    searchAnimation_->stop();
    searchAnimation_->setDuration(expanded ? 300 : 170);
    searchAnimation_->setStartValue(searchShell_->width());
    searchAnimation_->setEndValue(expanded ? 456 : 386);
    searchAnimation_->start();
}

void MainWindow::showSettingsMenu()
{
    QMenu menu(this);
    menu.setObjectName("settingsMenu");
    QActionGroup appearance(&menu);
    appearance.setExclusive(true);
    auto *light = menu.addAction("浅色");
    auto *dark = menu.addAction("深色");
    auto *system = menu.addAction("跟随系统");
    for (QAction *action : {light, dark, system}) {
        action->setCheckable(true);
        appearance.addAction(action);
    }
    switch (theme_->mode()) {
    case ThemeController::Mode::Light: light->setChecked(true); break;
    case ThemeController::Mode::Dark: dark->setChecked(true); break;
    case ThemeController::Mode::System: system->setChecked(true); break;
    }

    menu.addSeparator();
    const QString directory = GamePathResolver::gameDirectory();
    auto *pathStatus = menu.addAction(directory.isEmpty()
        ? "游戏目录：未检测到"
        : GamePathResolver::usesManualPath() ? "游戏目录：手动设置" : "游戏目录：自动检测");
    pathStatus->setEnabled(false);
    pathStatus->setToolTip(directory);
    auto *autoDetect = menu.addAction("自动检测游戏目录");
    auto *choosePath = menu.addAction("手动选择 LeagueClient.exe...");
    const QAction *choice = menu.exec(settingsButton_->mapToGlobal(QPoint(0, settingsButton_->height())));

    if (choice == light) theme_->setMode(ThemeController::Mode::Light);
    if (choice == dark) theme_->setMode(ThemeController::Mode::Dark);
    if (choice == system) theme_->setMode(ThemeController::Mode::System);
    if (choice == autoDetect) {
        GamePathResolver::useAutomaticPath();
        updateGamePathPresentation();
        const QString detected = GamePathResolver::gameDirectory();
        if (detected.isEmpty()) {
            QMessageBox::warning(this, "未检测到游戏目录", "未能自动找到 LeagueClient.exe。请手动选择该文件。");
        } else {
            QMessageBox::information(this, "已检测到游戏目录", QDir::toNativeSeparators(detected));
            if (clientAvailable_) champions_->refresh();
        }
    }
    if (choice == choosePath) {
        const QString initialDirectory = directory.isEmpty() ? QDir::homePath() : directory;
        const QString selected = QFileDialog::getOpenFileName(this, "选择 LeagueClient.exe", initialDirectory,
                                                               "LeagueClient.exe (LeagueClient.exe)");
        if (selected.isEmpty()) return;
        if (!GamePathResolver::setManualLeagueClientPath(selected)) {
            QMessageBox::warning(this, "游戏目录无效", "请选择名为 LeagueClient.exe 的有效游戏客户端文件。");
            return;
        }
        updateGamePathPresentation();
        QMessageBox::information(this, "已设置游戏目录", QDir::toNativeSeparators(GamePathResolver::gameDirectory()));
        if (clientAvailable_) champions_->refresh();
    }
}

void MainWindow::updateGamePathPresentation()
{
    const QString directory = GamePathResolver::gameDirectory();
    if (directory.isEmpty()) {
        gamePathLabel_->setText("正在自动检测游戏目录");
        gamePathLabel_->setToolTip({});
        settingsButton_->setToolTip("设置");
        return;
    }
    const QString nativePath = QDir::toNativeSeparators(directory);
    gamePathLabel_->setText(nativePath);
    gamePathLabel_->setToolTip(nativePath);
    settingsButton_->setToolTip("设置\n" + nativePath);
}

void MainWindow::updateSearch(const QString &query)
{
    const QString normalizedQuery = query.trimmed();
    // Programmatic changes (for example when opening a profile) must not
    // resurrect the popup.  Suggestions are strictly a focused-search UI.
    if (!search_->hasFocus()) {
        suggestions_->hide();
        return;
    }
    if (normalizedQuery.isEmpty()) {
        if (viewingExternalProfile_) {
            viewingExternalProfile_ = false;
            if (clientAvailable_) summoner_->refresh();
        }
        showSearchHistory();
        return;
    }
    // Suggestions follow the content, not a prior mouse click. This also
    // covers the second query after the popup has already been dismissed.
    setSearchExpanded(true);
    clearSearchSuggestions();
    for (const QString &label : {"查询召唤师 " + normalizedQuery, "查询 " + normalizedQuery + " 对局记录"}) { auto *button = new QPushButton(label); button->setObjectName("option"); suggestionLayout_->addWidget(button); }
    for (const Champion &champion : champions_->champions()) {
        if (champion.name.contains(normalizedQuery, Qt::CaseInsensitive) || champion.title.contains(normalizedQuery, Qt::CaseInsensitive) || champions_->aliasFor(champion.id).contains(normalizedQuery, Qt::CaseInsensitive)) {
            suggestionLayout_->addWidget(championSearchResult(suggestions_, *champions_, champion));
        }
    }
    auto *mine = new QPushButton("查看我的数据"); mine->setObjectName("option"); suggestionLayout_->addWidget(mine);
    connect(mine, &QPushButton::clicked, this, [this] { search_->clear(); });
    if (!suggestions_->isVisible()) reveal(suggestions_);
    else suggestions_->show();
    updateSuggestionGeometry();
    // QLayout activation is deferred on Windows while the line edit is being
    // edited.  Re-measure once the new child widgets have been polished so a
    // second or third query cannot leave a one-line white strip behind.
    QTimer::singleShot(0, this, [this] {
        if (!search_->hasFocus() || search_->text().trimmed().isEmpty()) return;
        suggestionLayout_->invalidate();
        suggestionLayout_->activate();
        suggestions_->show();
        updateSuggestionGeometry();
    });
}

void MainWindow::clearSearchSuggestions()
{
    while (auto *item = suggestionLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    suggestionLayout_->invalidate();
    suggestionLayout_->activate();
}

void MainWindow::showSearchHistory()
{
    clearSearchSuggestions();
    if (!clientAvailable_ || !search_->hasFocus()) {
        suggestions_->hide();
        return;
    }

    const QList<SearchHistoryEntry> entries = searchHistory_->entries();
    if (entries.isEmpty()) {
        suggestions_->hide();
        return;
    }

    auto *header = new QWidget(suggestions_);
    header->setObjectName("searchHistoryHeader");
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 7, 8, 4);
    auto *title = new QLabel("最近搜索", header);
    title->setObjectName("searchHistoryTitle");
    auto *clear = new QPushButton("清空记录", header);
    clear->setObjectName("clearSearchHistory");
    clear->setToolTip("删除全部搜索记录");
    clear->setFocusPolicy(Qt::NoFocus);
    headerLayout->addWidget(title);
    headerLayout->addStretch();
    headerLayout->addWidget(clear);
    suggestionLayout_->addWidget(header);
    connect(clear, &QPushButton::clicked, this, [this] {
        searchHistory_->clear();
        QMetaObject::invokeMethod(this, [this] { showSearchHistory(); }, Qt::QueuedConnection);
    });

    for (const SearchHistoryEntry &entry : entries) {
        auto *row = new QWidget(suggestions_);
        row->setObjectName("searchHistoryEntryRow");
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(2, 0, 3, 0);
        rowLayout->setSpacing(2);
        auto *open = new QPushButton(entry.riotId, row);
        open->setObjectName("searchHistoryEntry");
        open->setToolTip("查看 " + entry.riotId + " 的召唤师数据");
        auto *remove = new QToolButton(row);
        remove->setObjectName("searchHistoryRemove");
        remove->setText("×");
        remove->setToolTip("删除这条搜索记录");
        remove->setAccessibleName(remove->toolTip());
        remove->setCursor(Qt::PointingHandCursor);
        remove->setFocusPolicy(Qt::NoFocus);
        remove->setAutoRaise(true);
        remove->setFixedSize(30, 30);
        rowLayout->addWidget(open, 1);
        rowLayout->addWidget(remove);
        suggestionLayout_->addWidget(row);
        connect(open, &QPushButton::clicked, this, [this, entry] {
            openPlayerProfile(entry.puuid, entry.summonerId, entry.riotId);
        });
        connect(remove, &QToolButton::clicked, this, [this, entry] {
            searchHistory_->remove(entry);
            // Keep the field focused so the refreshed history remains open;
            // this also makes the per-row delete work when the button briefly
            // receives focus during a Windows mouse click.
            search_->setFocus(Qt::OtherFocusReason);
            QMetaObject::invokeMethod(this, [this] {
                showSearchHistory();
            }, Qt::QueuedConnection);
        });
    }
    if (!suggestions_->isVisible()) reveal(suggestions_);
    else suggestions_->show();
    updateSuggestionGeometry();
    QTimer::singleShot(0, this, [this] {
        if (!search_->hasFocus() || !search_->text().trimmed().isEmpty()) return;
        suggestionLayout_->invalidate();
        suggestionLayout_->activate();
        suggestions_->show();
        updateSuggestionGeometry();
    });
}

void MainWindow::updateSuggestionGeometry()
{
    if (!suggestions_ || !searchShell_) return;
    suggestions_->setFixedWidth(searchShell_->width());
    const QPoint requested = searchShell_->mapTo(this, QPoint(0, searchShell_->height() + 8));
    const int maximumHeight = qMax(72, height() - requested.y() - 10);
    suggestions_->setMaximumHeight(maximumHeight);
    suggestions_->setMinimumHeight(qMin(56, maximumHeight));
    suggestionLayout_->invalidate();
    suggestionLayout_->activate();
    suggestions_->adjustSize();
    const int x = qBound(8, requested.x(), qMax(8, width() - suggestions_->width() - 8));
    suggestions_->move(x, requested.y());
    suggestions_->raise();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == search_ || watched == searchShell_) && event->type() == QEvent::MouseButtonPress) {
        if (watched == searchShell_) search_->setFocus(Qt::MouseFocusReason);
        setSearchExpanded(true);
        QTimer::singleShot(0, this, [this] {
            if (!search_->hasFocus()) return;
            if (search_->text().trimmed().isEmpty()) showSearchHistory();
            else updateSearch(search_->text());
        });
    }
    if (watched == search_ && event->type() == QEvent::FocusIn) {
        setSearchExpanded(true);
        QTimer::singleShot(0, this, [this] {
            if (!search_->hasFocus()) return;
            if (search_->text().trimmed().isEmpty()) showSearchHistory();
            else updateSearch(search_->text());
        });
    }
    if (watched == search_ && event->type() == QEvent::FocusOut) {
        QTimer::singleShot(0, this, [this] {
            if (search_->hasFocus()) return;
            suggestions_->hide();
            setSearchExpanded(false);
        });
    }
    if (event->type() == QEvent::MouseButtonPress && searchExpanded_) {
        const auto *mouseEvent = static_cast<const QMouseEvent *>(event);
        const QPoint globalPoint = mouseEvent->globalPosition().toPoint();
        const QWidget *watchedWidget = qobject_cast<QWidget *>(watched);
        const auto belongsTo = [](const QWidget *widget, const QWidget *ancestor) {
            for (const QWidget *current = widget; current; current = current->parentWidget()) {
                if (current == ancestor) return true;
            }
            return false;
        };
        const bool inSearch = searchShell_->rect().contains(searchShell_->mapFromGlobal(globalPoint))
            || belongsTo(watchedWidget, searchShell_);
        const bool inSuggestions = suggestions_->isVisible()
            && (suggestions_->rect().contains(suggestions_->mapFromGlobal(globalPoint))
                || belongsTo(watchedWidget, suggestions_));
        if (!inSearch && !inSuggestions) {
            setSearchExpanded(false);
            suggestions_->hide();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MainWindow::mousePressEvent(QMouseEvent *event) { if (event->button() == Qt::LeftButton && event->position().y() < 62) { dragging_ = true; dragOffset_ = event->globalPosition().toPoint() - frameGeometry().topLeft(); } QWidget::mousePressEvent(event); }
void MainWindow::mouseMoveEvent(QMouseEvent *event) { if (dragging_ && (event->buttons() & Qt::LeftButton)) move(event->globalPosition().toPoint() - dragOffset_); QWidget::mouseMoveEvent(event); }
void MainWindow::mouseReleaseEvent(QMouseEvent *event) { dragging_ = false; QWidget::mouseReleaseEvent(event); }
void MainWindow::moveEvent(QMoveEvent *event)
{
    QWidget::moveEvent(event);
    if (opggWindow_ && opggWindow_->isVisible()) opggWindow_->syncToOwner();
}
void MainWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor border = qApp->property("jannaBorder").value<QColor>();
    painter.setBrush(palette().color(QPalette::Window));
    painter.setPen(QPen(border.isValid() ? border : palette().color(QPalette::Mid), 1));
    painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 12, 12);
}
void MainWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    QPainterPath path;
    path.addRoundedRect(rect().adjusted(0, 0, -1, -1), 12, 12);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
    if (suggestions_ && suggestions_->isVisible()) updateSuggestionGeometry();
    if (opggWindow_ && opggWindow_->isVisible()) opggWindow_->syncToOwner();
}
void MainWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    QPainterPath path;
    path.addRoundedRect(rect().adjusted(0, 0, -1, -1), 12, 12);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
    if (initialFocusHandled_) return;
    initialFocusHandled_ = true;
    QTimer::singleShot(0, this, [this] {
        if (isVisible()) setFocus(Qt::OtherFocusReason);
        if (opggWindow_ && opggWindow_->isVisible()) opggWindow_->syncToOwner();
    });
}
}
