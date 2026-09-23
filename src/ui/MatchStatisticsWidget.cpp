#include "ui/MatchStatisticsWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace Janna {
namespace {

QLabel *cell(QWidget *parent, const QString &text, const int width, const int stretch, QHBoxLayout *layout)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName("statTableCell");
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setToolTip(text);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    if (width > 0) label->setFixedWidth(width);
    layout->addWidget(label, stretch);
    return label;
}

QWidget *statisticsRow(QWidget *parent, const QStringList &values, const bool header)
{
    auto *row = new QWidget(parent);
    row->setObjectName(header ? "statisticsHeader" : "statisticsRow");
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, header ? 8 : 7, 0, header ? 7 : 7);
    layout->setSpacing(0);
    for (const QString &value : values) cell(row, value, 0, 1, layout);
    return row;
}

QString percentText(const int wins, const int games)
{
    if (games <= 0) return "---";
    return QString::number(static_cast<double>(wins) * 100.0 / games, 'f', 1) + "%";
}

} // namespace

MatchStatisticsWidget::MatchStatisticsWidget(QWidget *parent) : QFrame(parent), layout_(new QVBoxLayout(this))
{
    setObjectName("stats");
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);
    rebuild();
}

void MatchStatisticsWidget::setStatistics(QList<MatchModeStatistics> statistics)
{
    statistics_ = std::move(statistics);
    rebuild();
}

void MatchStatisticsWidget::rebuild()
{
    while (QLayoutItem *item = layout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    layout_->addWidget(statisticsRow(this, {"类型", "总场次", "胜率", "胜场", "败场", "段位", "赛季最高段位"}, true));
    if (statistics_.isEmpty()) {
        layout_->addWidget(statisticsRow(this, {"---", "---", "---", "---", "---", "---", "---"}, false));
        return;
    }
    for (const MatchModeStatistics &statistics : statistics_) {
        layout_->addWidget(statisticsRow(this, {
            statistics.modeName,
            QString::number(statistics.games),
            percentText(statistics.wins, statistics.games),
            QString::number(statistics.wins),
            QString::number(statistics.losses),
            statistics.rank,
            statistics.highestRank
        }, false));
    }
}

} // namespace Janna
