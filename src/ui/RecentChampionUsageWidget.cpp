#include "ui/RecentChampionUsageWidget.h"

#include "services/ChampionRepository.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QTimer>
#include <QToolButton>

namespace Janna {
namespace {
bool sameUsage(const QList<ChampionUsage> &left, const QList<ChampionUsage> &right)
{
    if (left.size() != right.size()) return false;
    for (qsizetype index = 0; index < left.size(); ++index) {
        const ChampionUsage &first = left.at(index);
        const ChampionUsage &second = right.at(index);
        if (first.championId != second.championId || first.games != second.games || first.wins != second.wins
            || first.losses != second.losses || first.kills != second.kills || first.deaths != second.deaths
            || first.assists != second.assists) {
            return false;
        }
    }
    return true;
}

QString usageTooltip(const QString &name, const ChampionUsage &usage)
{
    const double winRate = usage.games == 0 ? 0.0 : static_cast<double>(usage.wins) * 100.0 / usage.games;
    const double kda = static_cast<double>(usage.kills + usage.assists) / qMax(1, usage.deaths);
    return name + "\n总：" + QString::number(usage.games)
        + "  胜：" + QString::number(usage.wins)
        + "  负：" + QString::number(usage.losses)
        + "\n胜率：" + QString::number(winRate, 'f', 1) + "%"
        + "  KDA：" + QString::number(kda, 'f', 1);
}

} // namespace

RecentChampionUsageWidget::RecentChampionUsageWidget(ChampionRepository &champions, QWidget *parent)
    : QWidget(parent), champions_(champions), layout_(new QHBoxLayout(this))
{
    setObjectName("recentChampionUsage");
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(4);
    connect(&champions_, &ChampionRepository::championPortraitsChanged, this, &RecentChampionUsageWidget::rebuild);
}

void RecentChampionUsageWidget::setUsage(QList<ChampionUsage> usage)
{
    if (sameUsage(usage_, usage)) return;
    usage_ = std::move(usage);
    setVisible(!usage_.isEmpty());
    rebuild();
}

void RecentChampionUsageWidget::setSelectedChampionIds(QSet<int> selected)
{
    if (selectedChampionIds_ == selected) return;
    selectedChampionIds_ = std::move(selected);
    rebuild();
}

QString RecentChampionUsageWidget::championName(const int championId) const
{
    for (const Champion &champion : champions_.champions()) {
        if (champion.id == championId) return champion.name;
    }
    return "英雄 " + QString::number(championId);
}

void RecentChampionUsageWidget::rebuild()
{
    while (QLayoutItem *item = layout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (int index = 0; index < usage_.size(); ++index) {
        const ChampionUsage &usage = usage_.at(index);
        const QString name = championName(usage.championId);
        auto *button = new QToolButton(this);
        button->setObjectName("recentChampionButton");
        button->setCheckable(true);
        button->setChecked(selectedChampionIds_.contains(usage.championId));
        button->setToolTip(usageTooltip(name, usage));
        button->setAccessibleName(name);
        button->setCursor(Qt::PointingHandCursor);
        button->setFixedSize(40, 40);
        button->setIconSize({34, 34});
        const QPixmap portrait = champions_.portraitFor(usage.championId);
        if (portrait.isNull()) button->setText("?");
        else button->setIcon(QIcon(portrait));
        connect(button, &QToolButton::toggled, this, [this, championId = usage.championId](const bool selected) {
            if (selected) selectedChampionIds_.insert(championId);
            else selectedChampionIds_.remove(championId);
            if (selectionNotificationQueued_) return;
            selectionNotificationQueued_ = true;
            QTimer::singleShot(0, this, [this] {
                selectionNotificationQueued_ = false;
                emit selectedChampionIdsChanged(selectedChampionIds_);
            });
        });
        layout_->addWidget(button);
    }
}

} // namespace Janna
