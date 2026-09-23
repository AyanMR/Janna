#include "ui/AssistPage.h"

#include "services/ChampionRepository.h"
#include "services/GameDataRepository.h"
#include "services/OpggCrawler.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QAbstractItemView>
#include <QFrame>
#include <QRegularExpression>
#include <QStringList>

namespace Janna {
namespace {

QString pct(const double value)
{
    return value <= 0.0 ? QStringLiteral("-") : QString::number(value, 'f', 1) + "%";
}

QString championDisplayName(const ChampionRepository &repository, const int id)
{
    const QString name = repository.championNameFor(id);
    return !name.isEmpty() ? name : (id > 0 ? QStringLiteral("未知英雄") : QStringLiteral("不指定"));
}

QString championKey(QString value)
{
    value = value.trimmed().toCaseFolded();
    value.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    if (value == QStringLiteral("nunuwillump")) value = QStringLiteral("nunu");
    return value;
}

int championIdForRank(const ChampionRepository &repository, const OpggChampionRank &entry)
{
    return repository.resolveChampionId(entry.championId,
        !entry.championKey.trimmed().isEmpty() ? entry.championKey : entry.championName);
}

QString displayTier(const QString &raw)
{
    const QString value = raw.trimmed();
    if (value.isEmpty()) return QStringLiteral("-");
    const QString compact = value.toUpper().remove(' ').remove('_').remove('-');
    if (compact == QStringLiteral("0") || compact == QStringLiteral("OP") || compact == QStringLiteral("OPGOD")
        || compact == QStringLiteral("OPGG")) return QStringLiteral("OP");
    if (compact == QStringLiteral("TIER0")) return QStringLiteral("OP");
    if (compact.startsWith(QStringLiteral("TIER"))) {
        const QString number = compact.mid(4);
        if (number.toInt() > 0) return QStringLiteral("T") + number;
    }
    if (compact.toInt() > 0) return QStringLiteral("T") + compact;
    return value.toUpper();
}

QString runeSummary(const ChampionRepository &repository, const QList<int> &ids)
{
    QStringList names;
    for (const int id : ids) {
        const QString name = repository.runeNameFor(id).trimmed();
        names.append(name.isEmpty() ? QString::number(id) : name);
    }
    return names.isEmpty() ? QStringLiteral("无") : names.join(QStringLiteral("、"));
}

QString spellSummary(const ChampionRepository &repository, const QList<int> &ids)
{
    QStringList names;
    for (const int id : ids) {
        const QString name = repository.summonerSpellNameFor(id).trimmed();
        names.append(name.isEmpty() ? QString::number(id) : name);
    }
    return names.isEmpty() ? QStringLiteral("未提供") : names.join(QStringLiteral(" / "));
}

QString runeStyleName(const int styleId)
{
    switch (styleId) {
    case 8000: return QStringLiteral("精密");
    case 8100: return QStringLiteral("主宰");
    case 8200: return QStringLiteral("巫术");
    case 8300: return QStringLiteral("启迪");
    case 8400: return QStringLiteral("坚决");
    default: return styleId > 0 ? QString::number(styleId) : QStringLiteral("未提供");
    }
}

QString modeLabel(const QString &mode)
{
    const QString normalized = mode.trimmed().toUpper();
    if (normalized == QStringLiteral("SOLORANKED")) return QStringLiteral("排位 · 单双排");
    if (normalized == QStringLiteral("FLEXRANKED")) return QStringLiteral("排位 · 灵活组排");
    if (normalized == QStringLiteral("NORMAL")) return QStringLiteral("经典匹配");
    if (normalized == QStringLiteral("ARAM")) return QStringLiteral("大乱斗 · ARAM");
    if (normalized == QStringLiteral("ARENA")) return QStringLiteral("斗魂竞技场");
    if (normalized == QStringLiteral("URF")) return QStringLiteral("无限火力 · URF");
    if (normalized == QStringLiteral("ONEFORALL")) return QStringLiteral("无限乱斗");
    if (normalized == QStringLiteral("ULTBOOK")) return QStringLiteral("终极魔典");
    return normalized;
}

} // namespace

AssistPage::AssistPage(ChampionRepository &champions, GameDataRepository &gameData,
                       GameFlowRepository &gameFlow, AssistController &controller,
                       OpggCrawler &opgg, QWidget *parent)
    : QWidget(parent), champions_(champions), gameData_(gameData), gameFlow_(gameFlow),
      controller_(controller), opgg_(opgg)
{
    setObjectName("assistPage");
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(52, 32, 52, 92);
    root->setSpacing(16);

    auto *header = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("辅助功能"), this);
    title->setObjectName("heading");
    header->addWidget(title);
    status_ = new QLabel(QStringLiteral("等待进入英雄选择"), this);
    status_->setObjectName("statusPill");
    header->addWidget(status_);
    header->addStretch();
    mode_ = new QLabel(QStringLiteral("模式：-") , this);
    mode_->setObjectName("gameFlowMeta");
    header->addWidget(mode_);
    lane_ = new QLabel(QStringLiteral("分路：默认"), this);
    lane_->setObjectName("gameFlowMeta");
    header->addWidget(lane_);
    root->addLayout(header);

    auto *settingsPanel = new QFrame(this);
    settingsPanel->setObjectName("assistPanel");
    auto *settingsLayout = new QGridLayout(settingsPanel);
    settingsLayout->setContentsMargins(16, 14, 16, 14);
    settingsLayout->setHorizontalSpacing(12);
    settingsLayout->setVerticalSpacing(10);

    settingsLayout->addWidget(new QLabel(QStringLiteral("配置范围"), settingsPanel), 0, 0);
    scopeCombo_ = new QComboBox(settingsPanel);
    scopeCombo_->addItem(QStringLiteral("当前模式无分路 · 默认设置"), "default");
    scopeCombo_->addItem(QStringLiteral("上路"), "top");
    scopeCombo_->addItem(QStringLiteral("打野"), "jungle");
    scopeCombo_->addItem(QStringLiteral("中路"), "mid");
    scopeCombo_->addItem(QStringLiteral("下路"), "bot");
    scopeCombo_->addItem(QStringLiteral("辅助"), "support");
    settingsLayout->addWidget(scopeCombo_, 0, 1, 1, 3);
    settingsLayout->addWidget(new QLabel(QStringLiteral("自动 Ban"), settingsPanel), 1, 0);
    banCombo_ = new QComboBox(settingsPanel);
    settingsLayout->addWidget(banCombo_, 1, 1);
    autoBan_ = new QCheckBox(QStringLiteral("启用"), settingsPanel);
    settingsLayout->addWidget(autoBan_, 1, 2);
    settingsLayout->addWidget(new QLabel(QStringLiteral("自动选人"), settingsPanel), 2, 0);
    pickCombo_ = new QComboBox(settingsPanel);
    settingsLayout->addWidget(pickCombo_, 2, 1);
    autoPick_ = new QCheckBox(QStringLiteral("启用"), settingsPanel);
    settingsLayout->addWidget(autoPick_, 2, 2);
    autoLock_ = new QCheckBox(QStringLiteral("选人后自动锁定"), settingsPanel);
    settingsLayout->addWidget(autoLock_, 2, 3);
    auto *save = new QPushButton(QStringLiteral("保存设置"), settingsPanel);
    save->setObjectName("primary");
    settingsLayout->addWidget(save, 1, 3);
    root->addWidget(settingsPanel);

    auto *rankingPanel = new QFrame(this);
    rankingPanel->setObjectName("assistPanel");
    auto *rankingLayout = new QVBoxLayout(rankingPanel);
    rankingLayout->setContentsMargins(16, 14, 16, 14);
    auto *rankingHeader = new QHBoxLayout;
    rankingHeader->addWidget(new QLabel(QStringLiteral("OP.GG 英雄强度排行"), rankingPanel));
    rankingHeader->addStretch();
    rankingButton_ = new QPushButton(QStringLiteral("获取当前模式排行"), rankingPanel);
    rankingButton_->setObjectName("secondary");
    rankingHeader->addWidget(rankingButton_);
    rankingLayout->addLayout(rankingHeader);
    rankingTable_ = new QTableWidget(rankingPanel);
    rankingTable_->setObjectName("assistRankingTable");
    rankingTable_->setColumnCount(6);
    rankingTable_->setHorizontalHeaderLabels({QStringLiteral("排名"), QStringLiteral("英雄"), QStringLiteral("强度"),
                                              QStringLiteral("胜率"), QStringLiteral("登场率"), QStringLiteral("禁用率")});
    rankingTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    rankingTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    rankingTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    rankingTable_->verticalHeader()->setVisible(false);
    rankingTable_->horizontalHeader()->setStretchLastSection(true);
    rankingTable_->setIconSize(QSize(30, 30));
    rankingTable_->verticalHeader()->setDefaultSectionSize(38);
    rankingTable_->setMinimumHeight(210);
    rankingLayout->addWidget(rankingTable_);
    root->addWidget(rankingPanel, 1);

    auto *buildPanel = new QFrame(this);
    buildPanel->setObjectName("assistPanel");
    auto *buildLayout = new QVBoxLayout(buildPanel);
    buildLayout->setContentsMargins(16, 14, 16, 14);
    buildTitle_ = new QLabel(QStringLiteral("锁定英雄后在右侧显示 OP.GG 推荐配置"), buildPanel);
    buildTitle_->setObjectName("sectionTitle");
    buildLayout->addWidget(buildTitle_);
    buildDetails_ = new QLabel(QStringLiteral("右侧窗口会显示符文、属性碎片和召唤师技能；没有完整数据时不会覆盖当前配置。"), buildPanel);
    buildDetails_->setObjectName("muted");
    buildDetails_->setWordWrap(true);
    buildLayout->addWidget(buildDetails_);
    applyBuildButton_ = new QPushButton(QStringLiteral("一键应用推荐配置"), buildPanel);
    applyBuildButton_->setObjectName("primary");
    applyBuildButton_->setEnabled(false);
    buildLayout->addWidget(applyBuildButton_, 0, Qt::AlignLeft);
    root->addWidget(buildPanel);

    rebuildChampionOptions(banCombo_, 0);
    rebuildChampionOptions(pickCombo_, 0);
    loadScopeSettings();

    connect(scopeCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { loadScopeSettings(); });
    connect(save, &QPushButton::clicked, this, [this] { saveScopeSettings(); status_->setText(QStringLiteral("辅助设置已保存")); });
    connect(rankingButton_, &QPushButton::clicked, this, [this] { refreshRanking(); });
    connect(applyBuildButton_, &QPushButton::clicked, this, [this] {
        if (!build_.isValid()) return;
        applyBuildButton_->setEnabled(false);
        const QPointer<AssistPage> page(this);
        controller_.applyBuild(build_, [page](bool success, const QString &message) {
            if (!page) return;
            page->applyBuildButton_->setEnabled(page->build_.isValid());
            page->status_->setText(success ? QStringLiteral("推荐配置已应用") : QStringLiteral("配置应用失败"));
            QMessageBox::information(page, success ? QStringLiteral("配置完成") : QStringLiteral("配置未完成"), message);
        });
    });
    connect(&champions_, &ChampionRepository::championsChanged, this, [this] {
        rebuildChampionOptions(banCombo_, comboChampionId(banCombo_));
        rebuildChampionOptions(pickCombo_, comboChampionId(pickCombo_));
        renderRanking(ranking_);
    });
    connect(&champions_, &ChampionRepository::championPortraitsChanged, this, [this] {
        renderRanking(ranking_);
    });
    connect(&controller_, &AssistController::statusChanged, this, [this](const QString &message) {
        if (!message.trimmed().isEmpty()) status_->setText(message);
    });
    setSnapshot({});
}

void AssistPage::rebuildChampionOptions(QComboBox *combo, const int selectedId)
{
    if (!combo) return;
    updatingControls_ = true;
    combo->clear();
    combo->addItem(QStringLiteral("不指定"), 0);
    for (const Champion &champion : champions_.champions()) {
        combo->addItem(champion.name, champion.id);
    }
    const int index = combo->findData(selectedId);
    combo->setCurrentIndex(index >= 0 ? index : 0);
    updatingControls_ = false;
}

int AssistPage::comboChampionId(const QComboBox *combo) const
{
    if (!combo) return 0;
    return combo->currentData().toInt();
}

QString AssistPage::selectedScope() const
{
    return scopeCombo_ ? scopeCombo_->currentData().toString() : QStringLiteral("default");
}

void AssistPage::loadScopeSettings()
{
    const AssistLaneSettings settings = controller_.settingsForLane(selectedScope());
    updatingControls_ = true;
    const auto setCombo = [](QComboBox *combo, int id) {
        const int index = combo->findData(id);
        combo->setCurrentIndex(index >= 0 ? index : 0);
    };
    setCombo(banCombo_, settings.banChampionId);
    setCombo(pickCombo_, settings.pickChampionId);
    autoBan_->setChecked(settings.autoBan);
    autoPick_->setChecked(settings.autoPick);
    autoLock_->setChecked(settings.autoLock);
    updatingControls_ = false;
}

void AssistPage::saveScopeSettings()
{
    if (updatingControls_) return;
    AssistLaneSettings settings;
    settings.banChampionId = comboChampionId(banCombo_);
    settings.pickChampionId = comboChampionId(pickCombo_);
    settings.autoBan = autoBan_->isChecked();
    settings.autoPick = autoPick_->isChecked();
    settings.autoLock = autoLock_->isChecked();
    controller_.setSettingsForLane(selectedScope(), settings);
    loadScopeSettings();
}

void AssistPage::setSnapshot(const GameFlowSnapshot &snapshot)
{
    snapshot_ = snapshot;
    const QString mode = AssistController::modeKey(snapshot_);
    const QString lane = AssistController::effectiveLane(snapshot_);
    mode_->setText(QStringLiteral("模式：") + (snapshot_.isActive() ? mode : QStringLiteral("-")));
    lane_->setText(QStringLiteral("分路：") + AssistController::laneLabel(lane)
                   + (AssistController::usesDefaultSettings(snapshot_) ? QStringLiteral("（默认设置）") : QString{}));
    if (snapshot_.isActive()) status_->setText(snapshot_.phase.compare("ChampSelect", Qt::CaseInsensitive) == 0
        ? QStringLiteral("英雄选择中") : QStringLiteral("等待下一次英雄选择"));
    else status_->setText(QStringLiteral("等待进入英雄选择"));
    const QString desiredScope = AssistController::usesDefaultSettings(snapshot_) ? QStringLiteral("default") : lane;
    const int scopeIndex = scopeCombo_->findData(desiredScope);
    if (scopeIndex >= 0 && scopeCombo_->currentIndex() != scopeIndex) {
        QSignalBlocker blocker(scopeCombo_);
        scopeCombo_->setCurrentIndex(scopeIndex);
        loadScopeSettings();
    }
}

void AssistPage::refreshRanking()
{
    rankingButton_->setEnabled(false);
    rankingButton_->setText(QStringLiteral("读取中…"));
    const QString mode = AssistController::modeKey(snapshot_);
    const QString lane = AssistController::effectiveLane(snapshot_);
    const QString requestLane = lane == "default" ? QString{} : lane;
    const QPointer<AssistPage> page(this);
    opgg_.fetchChampionRanking(mode, requestLane, [page, mode, lane](QList<OpggChampionRank> ranking, QString error) {
        if (!page) return;
        page->rankingButton_->setEnabled(true);
        page->rankingButton_->setText(QStringLiteral("获取当前模式排行"));
        if (!error.isEmpty()) {
            page->status_->setText(QStringLiteral("OP.GG 读取失败"));
            QMessageBox::warning(page, QStringLiteral("OP.GG"), error);
            return;
        }
        page->ranking_ = std::move(ranking);
        page->renderRanking(page->ranking_);
        page->status_->setText(QStringLiteral("已读取 OP.GG · %1 · %2").arg(modeLabel(mode), AssistController::laneLabel(lane)));
    });
    emit opggRequested();
}

void AssistPage::renderRanking(const QList<OpggChampionRank> &ranking)
{
    rankingTable_->setRowCount(0);
    const int count = qMin(25, ranking.size());
    rankingTable_->setRowCount(count);
    for (int row = 0; row < count; ++row) {
        const OpggChampionRank &entry = ranking.at(row);
        const int championId = championIdForRank(champions_, entry);
        const QString name = championId > 0 ? championDisplayName(champions_, championId)
                                            : entry.championName;
        rankingTable_->setItem(row, 0, new QTableWidgetItem(QString::number(entry.rank > 0 ? entry.rank : row + 1)));
        auto *championItem = new QTableWidgetItem(name);
        if (championId > 0) {
            const QPixmap portrait = champions_.portraitFor(championId);
            if (!portrait.isNull()) championItem->setIcon(QIcon(portrait));
        }
        rankingTable_->setItem(row, 1, championItem);
        auto *tierItem = new QTableWidgetItem(displayTier(entry.tier));
        tierItem->setTextAlignment(Qt::AlignCenter);
        rankingTable_->setItem(row, 2, tierItem);
        rankingTable_->setItem(row, 3, new QTableWidgetItem(pct(entry.winRate)));
        rankingTable_->setItem(row, 4, new QTableWidgetItem(pct(entry.pickRate)));
        rankingTable_->setItem(row, 5, new QTableWidgetItem(pct(entry.banRate)));
    }
    rankingTable_->resizeColumnsToContents();
}

void AssistPage::renderBuild(const OpggBuild &build)
{
    const QString localName = build.championId > 0 ? championDisplayName(champions_, build.championId) : QString{};
    buildTitle_->setText(QStringLiteral("OP.GG 推荐配置 · %1").arg(localName.isEmpty()
        ? (build.championName.trimmed().isEmpty() ? QStringLiteral("当前英雄") : build.championName) : localName));
    QStringList lines;
    lines.append(QStringLiteral("主系：%1 · 副系：%2").arg(runeStyleName(build.primaryStyleId), runeStyleName(build.subStyleId)));
    lines.append(QStringLiteral("符文：%1").arg(runeSummary(champions_, build.runeIds)));
    lines.append(QStringLiteral("属性碎片：%1").arg(runeSummary(champions_, build.statShardIds)));
    lines.append(QStringLiteral("召唤师技能：%1").arg(spellSummary(champions_, build.summonerSpellIds)));
    buildDetails_->setText(lines.join("\n"));
    applyBuildButton_->setEnabled(build.isValid());
}

void AssistPage::setRecommendedBuild(const OpggBuild &build)
{
    build_ = build;
    renderBuild(build_);
    status_->setText(QStringLiteral("右侧 OPGG 窗口已显示推荐配置"));
}

void AssistPage::setRecommendedBuildError(const QString &message)
{
    build_ = {};
    buildTitle_->setText(QStringLiteral("OP.GG 推荐配置读取失败"));
    buildDetails_->setText(message);
    applyBuildButton_->setEnabled(false);
}

} // namespace Janna
