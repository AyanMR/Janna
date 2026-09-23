#include "ui/MatchHistoryWidget.h"

#include "services/ChampionRepository.h"
#include "services/GameDataRepository.h"
#include "services/MatchRepository.h"
#include "ui/LineIconButton.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDateTime>
#include <QCursor>
#include <QEasingCurve>
#include <QEnterEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPointer>
#include <QPainter>
#include <QPoint>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

namespace Janna {
namespace {

class MatchRow final : public QFrame {
public:
    explicit MatchRow(QWidget *parent = nullptr) : QFrame(parent) { setCursor(Qt::PointingHandCursor); }

    void setActivatedHandler(std::function<void()> handler) { handler_ = std::move(handler); }
    void watchSummaryChildren()
    {
        // Only the widgets directly managed by the summary layout need the
        // fallback click filter. Installing filters on every icon/label below
        // those widgets makes every mouse event walk a large object tree.
        for (QWidget *child : findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (child != this && !qobject_cast<QAbstractButton *>(child)) child->installEventFilter(this);
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != this && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick)) {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                beginActivationClick();
                event->accept();
                return true;
            }
        }
        if (event->type() == QEvent::MouseButtonRelease && watched != this) {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton && completeActivationClick()) {
                event->accept();
                return true;
            }
        }
        return QFrame::eventFilter(watched, event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            beginActivationClick();
            event->accept();
            return;
        }
        QFrame::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            beginActivationClick();
            event->accept();
            return;
        }
        QFrame::mouseDoubleClickEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && completeActivationClick()) {
            event->accept();
            return;
        }
        QFrame::mouseReleaseEvent(event);
    }

private:
    void beginActivationClick()
    {
        ++activationClickSerial_;
        activationClickArmed_ = true;
    }

    bool completeActivationClick()
    {
        // Child widgets can receive the same mouse sequence before the row.
        // Arm on press and consume exactly its matching release so bubbling
        // never turns one physical click into a second toggle.
        if (!activationClickArmed_ || handledReleaseSerial_ == activationClickSerial_ || !handler_) return false;
        activationClickArmed_ = false;
        handledReleaseSerial_ = activationClickSerial_;
        const std::function<void()> handler = handler_;
        handler();
        return true;
    }

    std::function<void()> handler_;
    quint64 activationClickSerial_{};
    quint64 handledReleaseSerial_{};
    bool activationClickArmed_{};
};

QString championName(const ChampionRepository &repository, const int championId)
{
    for (const Champion &champion : repository.champions()) {
        if (champion.id == championId) return champion.name;
    }
    return "英雄 " + QString::number(championId);
}

QString durationText(const int seconds)
{
    return QString::number(qMax(0, seconds) / 60) + ':' + QString::number(qMax(0, seconds) % 60).rightJustified(2, '0');
}

QString kdaText(const MatchSummary &match)
{
    const double ratio = static_cast<double>(match.kills + match.assists) / qMax(1, match.deaths);
    return QString::number(match.kills) + " / " + QString::number(match.deaths) + " / " + QString::number(match.assists)
        + "  (" + QString::number(ratio, 'f', 1) + ')';
}

QString kdaText(const MatchParticipant &participant)
{
    return QString::number(participant.kills) + " / " + QString::number(participant.deaths) + " / "
        + QString::number(participant.assists);
}

bool sameMatch(const MatchSummary &left, const MatchSummary &right)
{
    return left.gameId == right.gameId && left.won == right.won && left.gameMode == right.gameMode
        && left.queueId == right.queueId && left.mapId == right.mapId && left.durationSeconds == right.durationSeconds
        && left.createdAt == right.createdAt && left.championId == right.championId
        && left.championLevel == right.championLevel && left.lane == right.lane && left.role == right.role
        && left.primaryRuneId == right.primaryRuneId && left.runeIds == right.runeIds
        && left.spell1Id == right.spell1Id && left.spell2Id == right.spell2Id && left.kills == right.kills
        && left.deaths == right.deaths && left.assists == right.assists && left.minionKills == right.minionKills
        && left.goldEarned == right.goldEarned && left.itemIds == right.itemIds;
}

bool sameMatches(const QList<MatchSummary> &left, const QList<MatchSummary> &right)
{
    if (left.size() != right.size()) return false;
    for (qsizetype index = 0; index < left.size(); ++index) {
        if (!sameMatch(left.at(index), right.at(index))) return false;
    }
    return true;
}

QString numberText(const int value)
{
    return QLocale().toString(value);
}

QLabel *assetIcon(QWidget *parent, const QPixmap &icon, const QString &fallback, const QString &tooltip, const QSize &size)
{
    auto *label = new QLabel(parent);
    label->setObjectName("matchAssetIcon");
    label->setFixedSize(size);
    label->setAlignment(Qt::AlignCenter);
    label->setToolTip(tooltip);
    if (icon.isNull()) {
        label->setText(fallback);
    } else {
        label->setPixmap(icon.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    return label;
}

class RuneIconLabel final : public QLabel {
public:
    RuneIconLabel(QWidget *parent, ChampionRepository &champions, QList<int> runeIds, const int primaryRuneId,
                  const QSize &size, const bool enableHoverPopup = true)
        : QLabel(parent), champions_(champions), runeIds_(std::move(runeIds)), enableHoverPopup_(enableHoverPopup)
    {
        if (primaryRuneId > 0 && !runeIds_.contains(primaryRuneId)) runeIds_.prepend(primaryRuneId);
        setObjectName("matchAssetIcon");
        setFixedSize(size);
        setAlignment(Qt::AlignCenter);
        setCursor(runeIds_.isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
        if (primaryRuneId <= 0) {
            setToolTip("该对局未返回主符文");
            return;
        }
        setToolTip(champions_.runeNameFor(primaryRuneId));
        const QPixmap icon = champions_.runeIconFor(primaryRuneId);
        if (icon.isNull()) {
            setText("?");
        } else {
            setPixmap(icon.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    }

    ~RuneIconLabel() override { hidePopup(); }

protected:
    void enterEvent(QEnterEvent *event) override
    {
        QLabel::enterEvent(event);
        if (enableHoverPopup_) showPopup();
    }

    void leaveEvent(QEvent *event) override
    {
        QLabel::leaveEvent(event);
        if (!enableHoverPopup_) return;
        QTimer::singleShot(80, this, [this] {
            if (!underMouse()) hidePopup();
        });
    }

private:
    void showPopup()
    {
        if (runeIds_.isEmpty() || popup_) return;

        auto *popup = new QFrame(nullptr, Qt::ToolTip | Qt::FramelessWindowHint);
        popup->setObjectName("runeHoverCard");
        popup->setAttribute(Qt::WA_ShowWithoutActivating);
        auto *layout = new QHBoxLayout(popup);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(8);
        for (const int runeId : runeIds_) {
            auto *entry = new QWidget(popup);
            auto *entryLayout = new QVBoxLayout(entry);
            entryLayout->setContentsMargins(0, 0, 0, 0);
            entryLayout->setSpacing(3);
            auto *icon = new QLabel(entry);
            icon->setObjectName("runeHoverIcon");
            icon->setFixedSize(30, 30);
            icon->setAlignment(Qt::AlignCenter);
            const QPixmap pixmap = champions_.runeIconFor(runeId);
            if (pixmap.isNull()) icon->setText("?");
            else icon->setPixmap(pixmap.scaled(icon->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            auto *name = new QLabel(champions_.runeNameFor(runeId), entry);
            name->setObjectName("runeHoverName");
            name->setAlignment(Qt::AlignCenter);
            name->setMaximumWidth(110);
            name->setWordWrap(true);
            entryLayout->addWidget(icon, 0, Qt::AlignHCenter);
            entryLayout->addWidget(name);
            layout->addWidget(entry);
        }
        popup->adjustSize();
        popup->move(QCursor::pos() + QPoint(14, 14));
        popup->show();
        popup_ = popup;
    }

    void hidePopup()
    {
        if (!popup_) return;
        popup_->hide();
        popup_->deleteLater();
        popup_ = nullptr;
    }

    ChampionRepository &champions_;
    QList<int> runeIds_;
    QPointer<QFrame> popup_;
    bool enableHoverPopup_{true};
};

QWidget *spellIcons(QWidget *parent, ChampionRepository &champions, const int spell1Id, const int spell2Id, const QSize &size)
{
    auto *spells = new QWidget(parent);
    spells->setObjectName("matchSpellIcons");
    auto *layout = new QHBoxLayout(spells);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    layout->addWidget(assetIcon(spells, champions.summonerSpellIconFor(spell1Id), "1", champions.summonerSpellNameFor(spell1Id), size));
    layout->addWidget(assetIcon(spells, champions.summonerSpellIconFor(spell2Id), "2", champions.summonerSpellNameFor(spell2Id), size));
    spells->setFixedWidth(size.width() * 2 + 3);
    return spells;
}

QWidget *itemIcons(QWidget *parent, ChampionRepository &champions, const QList<int> &itemIds, const QSize &size, const bool reserveSlots)
{
    auto *items = new QWidget(parent);
    items->setObjectName("matchItemIcons");
    auto *layout = new QHBoxLayout(items);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    if (itemIds.isEmpty() && !reserveSlots) {
        auto *none = new QLabel("无装备", items);
        none->setObjectName("matchSmall");
        none->setAlignment(Qt::AlignCenter);
        layout->addWidget(none);
    } else {
        const int slotCount = reserveSlots ? 7 : itemIds.size();
        for (int index = 0; index < slotCount; ++index) {
            if (index < itemIds.size()) {
                const int itemId = itemIds.at(index);
                layout->addWidget(assetIcon(items, champions.itemIconFor(itemId), "?", champions.itemNameFor(itemId), size));
            } else {
                layout->addWidget(assetIcon(items, {}, {}, {}, size));
            }
        }
    }
    if (reserveSlots) items->setFixedWidth(7 * size.width() + 6 * layout->spacing());
    return items;
}

QWidget *championCell(QWidget *parent, ChampionRepository &champions, const int championId, const int level)
{
    auto *cell = new QFrame(parent);
    cell->setObjectName("matchChampionCell");
    cell->setFixedSize(38, 38);
    auto *portrait = new QLabel(cell);
    portrait->setGeometry(0, 0, 38, 38);
    portrait->setAlignment(Qt::AlignCenter);
    portrait->setToolTip(championName(champions, championId));
    const QPixmap icon = champions.portraitFor(championId);
    if (icon.isNull()) {
        portrait->setText(QString::number(championId));
    } else {
        portrait->setPixmap(icon.scaled(portrait->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    }
    auto *levelBadge = new QLabel(QString::number(level), cell);
    levelBadge->setObjectName("championLevelBadge");
    levelBadge->setAlignment(Qt::AlignCenter);
    levelBadge->setGeometry(21, 21, 17, 17);
    return cell;
}

QString sideName(const int teamId)
{
    return teamId == 100 ? "蓝色方" : "红色方";
}

QList<MatchParticipant> teamParticipants(const MatchDetail &detail, const int teamId)
{
    QList<MatchParticipant> participants;
    for (const MatchParticipant &participant : detail.participants) {
        if (participant.teamId == teamId) participants.append(participant);
    }
    return participants;
}

QString teamKdaText(const QList<MatchParticipant> &participants)
{
    int kills = 0;
    int deaths = 0;
    int assists = 0;
    for (const MatchParticipant &participant : participants) {
        kills += participant.kills;
        deaths += participant.deaths;
        assists += participant.assists;
    }
    return QString::number(kills) + " / " + QString::number(deaths) + " / " + QString::number(assists);
}

using PlayerSelectionHandler = std::function<void(const QString &, const QString &, const QString &)>;

QString playerNameText(const MatchParticipant &participant)
{
    return participant.riotId().isEmpty() ? "未知玩家" : participant.riotId();
}

QString rankText(const QString &tier, const QString &division)
{
    const QString normalizedTier = tier.trimmed().toUpper();
    QString localizedTier;
    if (normalizedTier == "IRON") localizedTier = "黑铁";
    else if (normalizedTier == "BRONZE") localizedTier = "黄铜";
    else if (normalizedTier == "SILVER") localizedTier = "白银";
    else if (normalizedTier == "GOLD") localizedTier = "黄金";
    else if (normalizedTier == "PLATINUM") localizedTier = "铂金";
    else if (normalizedTier == "EMERALD") localizedTier = "翡翠";
    else if (normalizedTier == "DIAMOND") localizedTier = "钻石";
    else if (normalizedTier == "MASTER") localizedTier = "大师";
    else if (normalizedTier == "GRANDMASTER") localizedTier = "宗师";
    else if (normalizedTier == "CHALLENGER") localizedTier = "王者";
    if (localizedTier.isEmpty()) return {};

    const QString normalizedDivision = division.trimmed().toUpper();
    if (normalizedDivision.isEmpty() || normalizedDivision == "NONE" || normalizedDivision == "NA") return localizedTier;
    return localizedTier + " " + normalizedDivision;
}

QWidget *rankEmblemCell(QWidget *parent, ChampionRepository &champions, const QString &currentTier,
                        const QString &currentDivision, const int currentLeaguePoints, const QString &fallbackTier)
{
    auto *emblem = new QLabel(parent);
    emblem->setObjectName("rankEmblem");
    emblem->setFixedSize(42, 34);
    emblem->setAlignment(Qt::AlignCenter);
    // Newer match-history responses leave highestAchievedSeasonTier empty.
    // Prefer the asynchronously resolved current rank, retaining the legacy
    // value only as a fallback for older clients.
    const QString resolvedTier = currentTier.trimmed().isEmpty() ? fallbackTier.trimmed() : currentTier.trimmed();
    const QString emblemTier = resolvedTier.isEmpty() ? "unranked" : resolvedTier;
    const QPixmap icon = champions.rankEmblemFor(emblemTier);
    if (!icon.isNull()) emblem->setPixmap(icon.scaled({32, 32}, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    if (!currentTier.trimmed().isEmpty()) {
        const QString rank = rankText(currentTier, currentDivision);
        emblem->setToolTip(rank.isEmpty() ? "未定级" : rank + " "
            + QString::number(qMax(0, currentLeaguePoints)) + " 胜点");
    } else {
        const QString fallbackRank = rankText(fallbackTier, {});
        emblem->setToolTip(fallbackRank.isEmpty() ? "未定级" : "赛季最高段位：" + fallbackRank + " 0 胜点");
    }
    return emblem;
}

QWidget *detailPlayerRow(QWidget *parent, ChampionRepository &champions, const MatchParticipant &participant,
                         const PlayerSelectionHandler &playerSelectionHandler)
{
    auto *row = new QFrame(parent);
    row->setObjectName("matchPlayerRow");
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 5, 8, 5);
    layout->setSpacing(7);

    layout->addWidget(new RuneIconLabel(row, champions, participant.runeIds, participant.primaryRuneId, {22, 22}));
    auto *spells = spellIcons(row, champions, participant.spell1Id, participant.spell2Id, {20, 20});
    layout->addWidget(spells);
    layout->addWidget(championCell(row, champions, participant.championId, participant.championLevel));

    auto *name = new QToolButton(row);
    name->setText(playerNameText(participant));
    name->setObjectName("matchPlayerName");
    name->setAutoRaise(true);
    name->setToolButtonStyle(Qt::ToolButtonTextOnly);
    name->setMinimumWidth(112);
    name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    const bool canOpenProfile = !participant.puuid.isEmpty() || !participant.summonerId.isEmpty();
    name->setCursor(canOpenProfile ? Qt::PointingHandCursor : Qt::ArrowCursor);
    name->setToolTip(canOpenProfile
        ? name->text() + "\n点击查看召唤师数据"
        : name->text());
    if (canOpenProfile) {
        QObject::connect(name, &QToolButton::clicked, name,
                         [playerSelectionHandler, participant] { playerSelectionHandler(participant.puuid, participant.summonerId, participant.riotId()); });
    }
    layout->addWidget(name, 1);
    if (participant.hiddenMatchHistory) {
        auto *lock = new QLabel(QString::fromUtf8("\xF0\x9F\x94\x92"), row);
        lock->setObjectName("matchHistoryPrivateLock");
        lock->setAlignment(Qt::AlignCenter);
        lock->setFixedSize(18, 18);
        lock->setToolTip("该玩家已隐藏战绩");
        layout->addWidget(lock);
    }

    layout->addWidget(rankEmblemCell(row, champions, participant.currentRankTier,
                                    participant.currentRankDivision, participant.currentRankLeaguePoints,
                                    participant.highestAchievedSeasonTier));

    layout->addWidget(itemIcons(row, champions, participant.itemIds, {21, 21}, true));
    auto *kda = new QLabel(kdaText(participant), row);
    kda->setObjectName("matchPlayerKda");
    kda->setAlignment(Qt::AlignCenter);
    kda->setFixedWidth(84);
    layout->addWidget(kda);
    auto *cs = new QLabel(QString::number(participant.minionKills()), row);
    cs->setObjectName("matchPlayerCs");
    cs->setAlignment(Qt::AlignCenter);
    cs->setToolTip("小兵击杀：" + QString::number(participant.laneMinionKills)
        + "\n野怪击杀：" + QString::number(participant.neutralMinionKills));
    cs->setFixedWidth(56);
    layout->addWidget(cs);
    auto *statistics = new QLabel("输出 " + numberText(participant.damageDealtToChampions)
        + "  承伤 " + numberText(participant.damageTaken)
        + "  守卫 " + QString::number(participant.visionScore), row);
    statistics->setObjectName("matchPlayerStats");
    statistics->setToolTip("插眼：" + QString::number(participant.wardsPlaced) + "\n排眼：" + QString::number(participant.wardsKilled));
    statistics->setFixedWidth(210);
    layout->addWidget(statistics);
    return row;
}

QWidget *teamDetail(QWidget *parent, ChampionRepository &champions, const MatchDetail &detail, const MatchTeam &team,
                    const PlayerSelectionHandler &playerSelectionHandler)
{
    const QList<MatchParticipant> participants = teamParticipants(detail, team.teamId);
    auto *section = new QFrame(parent);
    section->setObjectName(team.won ? "matchTeamWin" : "matchTeamLoss");
    auto *layout = new QVBoxLayout(section);
    layout->setContentsMargins(9, 8, 9, 9);
    layout->setSpacing(4);

    auto *header = new QHBoxLayout;
    header->setSpacing(8);
    auto *side = new QLabel(sideName(team.teamId), section);
    side->setObjectName("matchTeamName");
    auto *outcome = new QLabel(team.won ? "胜利" : "失败", section);
    outcome->setObjectName("matchTeamOutcome");
    auto *objectives = new QLabel("防御塔 " + QString::number(team.towerKills)
        + "  水晶 " + QString::number(team.inhibitorKills)
        + "  小龙 " + QString::number(team.dragonKills)
        + "  大龙 " + QString::number(team.baronKills)
        + "  峡谷先锋 " + QString::number(team.riftHeraldKills), section);
    objectives->setObjectName("matchTeamObjectives");
    auto *kda = new QLabel(teamKdaText(participants), section);
    kda->setObjectName("matchTeamKda");
    header->addWidget(side);
    header->addWidget(outcome);
    header->addWidget(objectives);
    header->addStretch();
    header->addWidget(kda);
    layout->addLayout(header);

    auto *columnHeader = new QHBoxLayout;
    columnHeader->setContentsMargins(8, 1, 8, 1);
    columnHeader->setSpacing(7);
    const auto addHeader = [&columnHeader, section](const QString &text, const int width, const int stretch = 0) {
        auto *label = new QLabel(text, section);
        label->setObjectName("matchDetailColumnHeader");
        label->setAlignment(Qt::AlignCenter);
        if (width > 0) label->setFixedWidth(width);
        columnHeader->addWidget(label, stretch);
    };
    addHeader("符文", 22);
    addHeader("技能", 43);
    addHeader("英雄", 38);
    addHeader("玩家", 0, 1);
    addHeader("段位", 42);
    addHeader("装备", 165);
    addHeader("KDA", 84);
    addHeader("补刀", 56);
    addHeader("数据", 210);
    layout->addLayout(columnHeader);

    for (const MatchParticipant &participant : participants) {
        layout->addWidget(detailPlayerRow(section, champions, participant, playerSelectionHandler));
    }
    return section;
}

void clearLayout(QLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

bool populateDetails(QFrame *details, const MatchSummary &match, ChampionRepository &champions,
                     MatchRepository &matchRepository, GameDataRepository &gameData,
                     const PlayerSelectionHandler &playerSelectionHandler)
{
    const bool wereUpdatesEnabled = details->updatesEnabled();
    details->setUpdatesEnabled(false);
    if (QLayout *previous = details->layout()) {
        clearLayout(previous);
        delete previous;
    }

    auto *detailLayout = new QVBoxLayout(details);
    detailLayout->setContentsMargins(10, 9, 10, 10);
    detailLayout->setSpacing(8);
    const MatchDetail *detail = matchRepository.detail(match.gameId);
    if (detail != nullptr) {
        const int detailQueueId = detail->queueId == 0 ? match.queueId : detail->queueId;
        const int detailMapId = detail->mapId == 0 ? match.mapId : detail->mapId;
        auto *detailHeader = new QLabel(gameData.modeName(detailQueueId, detailMapId, detail->gameMode)
            + "  ·  " + gameData.mapName(detailMapId)
            + "  ·  " + durationText(detail->durationSeconds == 0 ? match.durationSeconds : detail->durationSeconds)
            + (detail->createdAt.isValid() ? "  ·  " + detail->createdAt.toLocalTime().toString("yyyy-MM-dd hh:mm") : QString()), details);
        detailHeader->setObjectName("matchDetailHeader");
        detailLayout->addWidget(detailHeader);
        for (const MatchTeam &team : detail->teams) {
            detailLayout->addWidget(teamDetail(details, champions, *detail, team, playerSelectionHandler));
        }
    } else {
        const QString error = matchRepository.detailError(match.gameId);
        auto *status = new QWidget(details);
        status->setMinimumHeight(58);
        auto *statusLayout = new QHBoxLayout(status);
        statusLayout->setContentsMargins(8, 8, 8, 8);
        statusLayout->setSpacing(8);
        statusLayout->addStretch();
        auto *statusText = new QLabel("完整对局记录加载失败：" + error, status);
        statusText->setObjectName(error.isEmpty() ? "matchDetailPending" : "matchDetailError");
        statusText->setAlignment(Qt::AlignCenter);
        statusLayout->addWidget(statusText);
        statusLayout->addStretch();
        detailLayout->addWidget(status);
    }
    details->setUpdatesEnabled(wereUpdatesEnabled);
    if (wereUpdatesEnabled) details->update();
    return detail != nullptr;
}

constexpr int kLoadingDetailsHeight = 78;

int detailsContentHeight(QFrame *details)
{
    if (!details || !details->layout()) return 0;
    QLayout *layout = details->layout();
    // The frame is deliberately capped while it is animating. Qt propagates
    // that cap into a layout's size hint, which previously made a full detail
    // layout report the loading-card height (78px) and left the rest clipped.
    // Measure once without the transient animation constraint, then restore
    // the exact bounds the caller is currently using.
    const int previousMinimum = details->minimumHeight();
    const int previousMaximum = details->maximumHeight();
    const int previousMinimumWidth = details->minimumWidth();
    const int previousMaximumWidth = details->maximumWidth();
    int measurementWidth = details->width();
    if (measurementWidth <= 0) {
        if (const QWidget *parent = details->parentWidget()) measurementWidth = parent->contentsRect().width();
        if (measurementWidth <= 0) measurementWidth = qMax(details->sizeHint().width(), 900);
    }
    if (measurementWidth > 0 && details->width() < measurementWidth) details->setMinimumWidth(measurementWidth);
    details->setMinimumHeight(0);
    details->setMaximumHeight(QWIDGETSIZE_MAX);
    layout->invalidate();
    if (measurementWidth > 0) layout->setGeometry(QRect(0, 0, measurementWidth, QWIDGETSIZE_MAX));
    layout->activate();
    const int contentHeight = qMax(layout->totalSizeHint().height(),
                                   qMax(layout->sizeHint().height(), details->sizeHint().height()));
    details->setMinimumHeight(previousMinimum);
    details->setMaximumHeight(previousMaximum);
    details->setMinimumWidth(previousMinimumWidth);
    details->setMaximumWidth(previousMaximumWidth);
    return qMax(contentHeight, details->minimumSizeHint().height());
}

int detailsTargetHeight(QFrame *details, const bool hasFullDetail)
{
    const int intrinsicHeight = detailsContentHeight(details);
    return hasFullDetail ? qMax(1, intrinsicHeight) : qMax(kLoadingDetailsHeight, intrinsicHeight);
}

int animatedDetailsHeight(QFrame *details)
{
    if (!details) return 0;
    if (auto *animation = details->findChild<QPropertyAnimation *>("detailsHeightAnimation")) {
        if (animation->state() == QAbstractAnimation::Running) {
            return qMax(0, animation->currentValue().toInt());
        }
    }
    const int maximumHeight = details->maximumHeight();
    return maximumHeight == QWIDGETSIZE_MAX ? qMax(0, details->height()) : qMax(0, maximumHeight);
}

struct MatchScrollAnchor {
    QPointer<QScrollArea> scrollArea;
    QPointer<QWidget> row;
    int rowViewportY{};
};

MatchScrollAnchor scrollAnchorFor(QWidget *row)
{
    QScrollArea *scrollArea = nullptr;
    for (QWidget *candidate = row; candidate != nullptr; candidate = candidate->parentWidget()) {
        if (auto *found = qobject_cast<QScrollArea *>(candidate)) {
            scrollArea = found;
            break;
        }
    }
    if (!scrollArea || !row || !scrollArea->viewport()) return {};
    return {scrollArea, row, row->mapTo(scrollArea->viewport(), QPoint{}).y()};
}

void restoreScrollAnchor(const MatchScrollAnchor &anchor)
{
    if (!anchor.scrollArea || !anchor.row || !anchor.scrollArea->viewport()) return;
    QScrollBar *scrollBar = anchor.scrollArea->verticalScrollBar();
    if (!scrollBar) return;
    const int currentY = anchor.row->mapTo(anchor.scrollArea->viewport(), QPoint{}).y();
    const int offset = currentY - anchor.rowViewportY;
    if (offset == 0) return;
    scrollBar->setValue(qBound(scrollBar->minimum(), scrollBar->value() + offset, scrollBar->maximum()));
}

void restoreScrollAnchorLater(const MatchScrollAnchor &anchor)
{
    if (!anchor.row) return;
    QTimer::singleShot(0, anchor.row, [anchor] { restoreScrollAnchor(anchor); });
}

void cancelDetailsHeightAnimation(QFrame *details)
{
    if (!details) return;
    if (auto *animation = details->findChild<QPropertyAnimation *>("detailsHeightAnimation")) {
        // Keep one animation object attached to the details frame for its
        // entire lifetime. Deleting an animation from inside its own signal
        // chain can trip a Debug Qt/CRT assertion when a new animation starts
        // during the same event turn.
        QObject::disconnect(animation, &QPropertyAnimation::finished, nullptr, nullptr);
        animation->stop();
    }
}

QPropertyAnimation *detailsHeightAnimation(QFrame *details)
{
    if (!details) return nullptr;
    if (auto *existing = details->findChild<QPropertyAnimation *>("detailsHeightAnimation")) return existing;
    auto *animation = new QPropertyAnimation(details, "maximumHeight", details);
    animation->setObjectName("detailsHeightAnimation");
    return animation;
}

void animateDetailsHeight(QFrame *details, const int startHeight, const int targetHeight, const MatchScrollAnchor &anchor)
{
    cancelDetailsHeightAnimation(details);
    const int start = qMax(0, startHeight);
    const int target = qMax(0, targetHeight);
    details->setMinimumHeight(0);
    details->setMaximumHeight(start);
    if (start == target) {
        details->setMaximumHeight(target);
        restoreScrollAnchorLater(anchor);
        return;
    }

    auto *animation = detailsHeightAnimation(details);
    if (!animation) return;
    animation->setStartValue(start);
    animation->setEndValue(target);
    animation->setDuration(220);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    const QPointer<QFrame> safeDetails = details;
    QObject::connect(animation, &QPropertyAnimation::finished, details, [safeDetails, target, anchor] {
        if (!safeDetails) return;
        QFrame *details = safeDetails.data();
        details->setMaximumHeight(target);
        restoreScrollAnchor(anchor);
        restoreScrollAnchorLater(anchor);
    });
    animation->start();
}

void queueDetailsHeightSettle(QFrame *details, const int startHeight, const bool animated,
                              const bool hasFullDetail, const MatchScrollAnchor &anchor)
{
    if (!details) return;
    const quint64 serial = details->property("jannaHeightSettleSerial").toULongLong() + 1;
    details->setProperty("jannaHeightSettleSerial", QVariant::fromValue(serial));
    const QPointer<QFrame> safeDetails = details;
    QTimer::singleShot(0, details, [safeDetails, serial, startHeight, animated, hasFullDetail, anchor] {
        if (!safeDetails || safeDetails->property("jannaHeightSettleSerial").toULongLong() != serial) return;
        const int targetHeight = detailsTargetHeight(safeDetails, hasFullDetail);
        if (animated) {
            animateDetailsHeight(safeDetails, startHeight, targetHeight, anchor);
        } else {
            cancelDetailsHeightAnimation(safeDetails);
            safeDetails->setMaximumHeight(targetHeight);
            restoreScrollAnchorLater(anchor);
        }
    });
}

} // namespace

MatchHistoryWidget::MatchHistoryWidget(ChampionRepository &champions, MatchRepository &matchRepository,
                                       GameDataRepository &gameData, QWidget *parent)
    : QWidget(parent), champions_(champions), matchRepository_(matchRepository), gameData_(gameData), layout_(new QVBoxLayout(this))
{
    setObjectName("matchHistory");
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_StyledBackground);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(7);

    assetRebuildTimer_ = new QTimer(this);
    assetRebuildTimer_->setSingleShot(true);
    assetRebuildTimer_->setInterval(120);
    connect(assetRebuildTimer_, &QTimer::timeout, this, [this] {
        if (hasRunningDetailAnimation()) {
            assetRebuildTimer_->start();
            return;
        }
        rebuild();
    });

    QSettings settings;
    for (const QString &id : settings.value("matches/favorites").toStringList()) {
        bool valid = false;
        const qint64 gameId = id.toLongLong(&valid);
        if (valid) favoriteGameIds_.insert(gameId);
    }
    connect(&champions_, &ChampionRepository::championPortraitsChanged, this, &MatchHistoryWidget::queueAssetRefresh);
    connect(&champions_, &ChampionRepository::staticIconsChanged, this, &MatchHistoryWidget::queueAssetRefresh);
    connect(&gameData_, &GameDataRepository::catalogChanged, this, &MatchHistoryWidget::queueAssetRefresh);
    connect(&matchRepository_, &MatchRepository::matchDetailChanged, this, [this](const qint64 gameId) {
        if (!expandedGameIds_.contains(gameId)) return;
        const QPointer<QFrame> details = detailFrames_.value(gameId);
        if (!details) {
            addDetails(gameId, false);
            return;
        }
        const bool needsContentTransition = details && !details->property("jannaFullDetail").toBool();
        refreshDetails(gameId, needsContentTransition);
    });
    connect(&matchRepository_, &MatchRepository::matchDetailLoadingChanged, this, [this](const qint64 gameId, const bool loading, const QString &) {
        // A successful result emits matchDetailChanged immediately before this
        // signal. Refreshing twice restarts the height animation at its first
        // frame; this signal only needs to update the visible error state.
        if (!loading && expandedGameIds_.contains(gameId)) {
            if (!detailFrames_.value(gameId)) addDetails(gameId, false);
            else if (!matchRepository_.detail(gameId)) refreshDetails(gameId, true);
        }
    });
}

void MatchHistoryWidget::collapseDetails()
{
    QList<qint64> ids = detailFrames_.keys();
    for (const qint64 gameId : expandedGameIds_) {
        if (!ids.contains(gameId)) ids.append(gameId);
    }
    expandedGameIds_.clear();
    for (const qint64 gameId : ids) removeDetails(gameId, false);
}

void MatchHistoryWidget::setMatches(QList<MatchSummary> matches)
{
    if (sameMatches(matches_, matches) && layout_->count() > 0) return;
    // A changed filter can retain the same game id. Keeping its old details
    // frame would reuse an invalid geometry in the newly filtered layout.
    expandedGameIds_.clear();
    matches_ = std::move(matches);
    rebuild();
}

void MatchHistoryWidget::queueAssetRefresh()
{
    if (!assetRebuildTimer_->isActive()) assetRebuildTimer_->start();
}

bool MatchHistoryWidget::hasRunningDetailAnimation() const
{
    return std::any_of(detailFrames_.cbegin(), detailFrames_.cend(), [](const QPointer<QFrame> &details) {
        const QPropertyAnimation *animation = details ? details->findChild<QPropertyAnimation *>("detailsHeightAnimation") : nullptr;
        return animation != nullptr && animation->state() == QAbstractAnimation::Running;
    });
}

void MatchHistoryWidget::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.fillRect(event->rect(), palette().color(QPalette::Window));
}

void MatchHistoryWidget::rebuild()
{
    const bool wereUpdatesEnabled = updatesEnabled();
    setUpdatesEnabled(false);
    rowFrames_.clear();
    detailFrames_.clear();
    while (QLayoutItem *item = layout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    if (matches_.isEmpty()) {
        auto *empty = new QLabel("暂无近期对局记录");
        empty->setObjectName("empty");
        empty->setAlignment(Qt::AlignCenter);
        empty->setMinimumHeight(88);
        layout_->addWidget(empty);
    } else for (const MatchSummary &match : matches_) {
        auto *row = new MatchRow(this);
        row->setObjectName(match.won ? "matchRowWin" : "matchRowLoss");
        auto *rowLayout = new QVBoxLayout(row);
        rowLayout->setContentsMargins(12, 9, 12, 9);
        // The details panel animates its own vertical gap. Keeping the parent
        // layout gap at zero prevents a one-frame jump before the expansion.
        rowLayout->setSpacing(0);

        auto *summary = new QHBoxLayout;
        summary->setSpacing(12);
        auto *portrait = new QLabel;
        portrait->setObjectName("matchChampionPortrait");
        portrait->setFixedSize(42, 42);
        portrait->setAlignment(Qt::AlignCenter);
        const QPixmap championPortrait = champions_.portraitFor(match.championId);
        if (championPortrait.isNull()) {
            portrait->setText(QString::number(match.championId));
        } else {
            portrait->setPixmap(championPortrait.scaled(portrait->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        }
        summary->addWidget(portrait);

        auto *game = new QVBoxLayout;
        game->setSpacing(2);
        auto *title = new QLabel((match.won ? "胜利  " : "失败  ") + championName(champions_, match.championId));
        title->setObjectName("matchTitle");
        auto *metadata = new QLabel(gameData_.modeName(match.queueId, match.mapId, match.gameMode) + "  ·  " + durationText(match.durationSeconds)
            + (match.createdAt.isValid() ? "  ·  " + match.createdAt.toLocalTime().toString("MM-dd hh:mm") : QString()));
        metadata->setObjectName("muted");
        game->addWidget(title);
        game->addWidget(metadata);
        summary->addLayout(game, 1);

        auto *rune = new RuneIconLabel(row, champions_, match.runeIds, match.primaryRuneId, {28, 28}, false);
        summary->addWidget(rune);
        auto *spells = spellIcons(row, champions_, match.spell1Id, match.spell2Id, {26, 26});
        summary->addWidget(spells);
        auto *kda = new QLabel(kdaText(match));
        kda->setObjectName("matchKda");
        kda->setAlignment(Qt::AlignCenter);
        kda->setFixedWidth(124);
        summary->addWidget(kda);
        auto *cs = new QLabel("补刀 " + QString::number(match.minionKills));
        cs->setObjectName("matchSmall");
        cs->setAlignment(Qt::AlignCenter);
        cs->setFixedWidth(58);
        summary->addWidget(cs);
        auto *items = itemIcons(row, champions_, match.itemIds, {24, 24}, true);
        items->setObjectName("matchSummaryItems");
        summary->addWidget(items);
        auto *gold = new QLabel(QString::number(match.goldEarned) + " 金币");
        gold->setObjectName("matchSmall");
        gold->setAlignment(Qt::AlignCenter);
        gold->setFixedWidth(76);
        summary->addWidget(gold);
        auto *favorite = new LineIconButton(LineIconButton::Icon::Favorite, "收藏对局", row);
        favorite->setCheckable(true);
        favorite->setChecked(favoriteGameIds_.contains(match.gameId));
        favorite->setToolTip(favorite->isChecked() ? "已收藏对局" : "收藏对局");
        summary->addWidget(favorite);
        rowLayout->addLayout(summary);

        row->setActivatedHandler([this, gameId = match.gameId] { toggleDetails(gameId); });
        connect(favorite, &QToolButton::clicked, row, [this, gameId = match.gameId, favorite](const bool checked) {
            if (checked) {
                favoriteGameIds_.insert(gameId);
                favorite->setToolTip("已收藏对局");
            } else {
                favoriteGameIds_.remove(gameId);
                favorite->setToolTip("收藏对局");
            }
            saveFavorites();
        });
        row->watchSummaryChildren();
        rowFrames_.insert(match.gameId, row);
        layout_->addWidget(row);
        if (expandedGameIds_.contains(match.gameId)) addDetails(match.gameId, false);
    }
    layout_->addStretch(1);
    setUpdatesEnabled(wereUpdatesEnabled);
    if (wereUpdatesEnabled) {
        updateGeometry();
        update();
        if (QWidget *parent = parentWidget()) parent->update();
    }
}

void MatchHistoryWidget::addDetails(const qint64 gameId, const bool animated)
{
    if (detailFrames_.value(gameId)) {
        refreshDetails(gameId, animated);
        return;
    }
    detailFrames_.remove(gameId);
    const QPointer<QFrame> row = rowFrames_.value(gameId);
    if (!row) return;
    const auto matchIt = std::find_if(matches_.cbegin(), matches_.cend(), [gameId](const MatchSummary &match) {
        return match.gameId == gameId;
    });
    if (matchIt == matches_.cend()) return;
    // Keep the list free of a spinner card. The completed detail is mounted
    // from matchDetailChanged; an error is still rendered when loading ends.
    if (!matchRepository_.detail(gameId) && matchRepository_.detailError(gameId).isEmpty()) return;

    auto *details = new QFrame(row);
    details->setObjectName("matchDetails");
    // Prevent the details frame from participating in one full-height layout pass.
    details->setMinimumHeight(0);
    details->setMaximumHeight(0);
    const PlayerSelectionHandler playerSelectionHandler = [this](const QString &puuid, const QString &summonerId, const QString &riotId) {
        emit playerSelected(puuid, summonerId, riotId);
    };
    const bool hasFullDetail = populateDetails(details, *matchIt, champions_, matchRepository_, gameData_, playerSelectionHandler);
    details->setProperty("jannaFullDetail", hasFullDetail);
    auto *rowLayout = qobject_cast<QVBoxLayout *>(row->layout());
    if (!rowLayout) {
        details->deleteLater();
        return;
    }
    const MatchScrollAnchor anchor = scrollAnchorFor(row);
    rowLayout->addWidget(details);
    detailFrames_.insert(gameId, details);

    if (hasFullDetail) {
        // A cached response can arrive before the parent layout has assigned
        // the details width. Defer the full-height measurement until that
        // layout pass has completed.
        queueDetailsHeightSettle(details, 0, animated, true, anchor);
    } else {
        const int targetHeight = detailsTargetHeight(details, false);
        if (animated) animateDetailsHeight(details, 0, targetHeight, anchor);
        else {
            details->setMaximumHeight(targetHeight);
            restoreScrollAnchorLater(anchor);
        }
    }
}

void MatchHistoryWidget::removeDetails(const qint64 gameId, const bool animated)
{
    const QPointer<QFrame> details = detailFrames_.value(gameId);
    if (!details) {
        detailFrames_.remove(gameId);
        return;
    }
    details->setProperty("jannaHeightSettleSerial", QVariant::fromValue(details->property("jannaHeightSettleSerial").toULongLong() + 1));
    if (!animated) {
        detailFrames_.remove(gameId);
        cancelDetailsHeightAnimation(details);
        details->setMaximumHeight(0);
        details->hide();
        details->deleteLater();
        return;
    }

    const MatchScrollAnchor anchor = scrollAnchorFor(details->parentWidget());
    const int startHeight = animatedDetailsHeight(details);
    cancelDetailsHeightAnimation(details);
    details->setMinimumHeight(0);
    details->setMaximumHeight(startHeight);
    auto *animation = detailsHeightAnimation(details);
    if (!animation) return;
    animation->setStartValue(startHeight);
    animation->setEndValue(0);
    animation->setDuration(150);
    animation->setEasingCurve(QEasingCurve::InCubic);
    const QPointer<QFrame> safeDetails = details;
    connect(animation, &QPropertyAnimation::finished, this, [this, gameId, safeDetails, anchor] {
        if (detailFrames_.value(gameId).data() == safeDetails.data()) detailFrames_.remove(gameId);
        if (safeDetails) safeDetails->deleteLater();
        restoreScrollAnchor(anchor);
        restoreScrollAnchorLater(anchor);
    });
    animation->start();
}

void MatchHistoryWidget::refreshDetails(const qint64 gameId, const bool animated)
{
    const QPointer<QFrame> details = detailFrames_.value(gameId);
    if (!details) return;
    const auto matchIt = std::find_if(matches_.cbegin(), matches_.cend(), [gameId](const MatchSummary &match) {
        return match.gameId == gameId;
    });
    if (matchIt == matches_.cend()) return;

    const MatchScrollAnchor anchor = scrollAnchorFor(details->parentWidget());
    const int visibleHeight = animatedDetailsHeight(details);
    const bool hadFullDetail = details->property("jannaFullDetail").toBool();
    details->setMinimumHeight(0);
    details->setMaximumHeight(visibleHeight);
    const PlayerSelectionHandler playerSelectionHandler = [this](const QString &puuid, const QString &summonerId, const QString &riotId) {
        emit playerSelected(puuid, summonerId, riotId);
    };
    const bool hasFullDetail = populateDetails(details, *matchIt, champions_, matchRepository_, gameData_, playerSelectionHandler);
    details->setProperty("jannaFullDetail", hasFullDetail);
    const bool transitionToFullDetail = !hadFullDetail && hasFullDetail;
    if (hasFullDetail) {
        // LCU can answer before the loading card's first animation tick. Keep
        // its visible footprint while replacing it so the new layout is never
        // constrained to a zero-height frame.
        details->setMaximumHeight(qMax(visibleHeight, kLoadingDetailsHeight));
        queueDetailsHeightSettle(details, qMax(visibleHeight, kLoadingDetailsHeight),
                                 animated && transitionToFullDetail, true, anchor);
    } else {
        cancelDetailsHeightAnimation(details);
        const int targetHeight = detailsTargetHeight(details, false);
        details->setMaximumHeight(targetHeight);
        restoreScrollAnchorLater(anchor);
    }
}

void MatchHistoryWidget::toggleDetails(const qint64 gameId)
{
    const int expandedIndex = expandedGameIds_.indexOf(gameId);
    if (expandedIndex >= 0) {
        expandedGameIds_.removeAt(expandedIndex);
        removeDetails(gameId, true);
        return;
    }

    expandedGameIds_.append(gameId);
    while (expandedGameIds_.size() > 3) {
        const qint64 oldestGameId = expandedGameIds_.takeFirst();
        removeDetails(oldestGameId, true);
    }
    matchRepository_.loadDetail(gameId);
    addDetails(gameId, false);
}

void MatchHistoryWidget::saveFavorites() const
{
    QStringList ids;
    ids.reserve(favoriteGameIds_.size());
    for (const qint64 gameId : favoriteGameIds_) ids.append(QString::number(gameId));
    QSettings settings;
    settings.setValue("matches/favorites", ids);
    settings.sync();
}

} // namespace Janna
