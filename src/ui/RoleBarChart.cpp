#include "ui/RoleBarChart.h"

#include <QApplication>
#include <QPainter>

#include <algorithm>

namespace Janna {
namespace {

QColor applicationColor(const char *name, const QColor &fallback)
{
    if (!qApp) return fallback;
    const QColor color = qApp->property(name).value<QColor>();
    return color.isValid() ? color : fallback;
}

} // namespace

RoleBarChart::RoleBarChart(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(sizeHint());
}

void RoleBarChart::setRoleUsage(QList<RoleUsage> usage)
{
    usage_ = std::move(usage);
    update();
}

QSize RoleBarChart::sizeHint() const { return {250, 82}; }

void RoleBarChart::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor accent = applicationColor("jannaAccent", QColor("#16826c"));
    const QColor muted = applicationColor("jannaMuted", QColor("#758078"));
    const QColor border = applicationColor("jannaBorder", QColor("#dde5df"));
    const QColor text = applicationColor("jannaText", QColor("#202723"));
    if (usage_.isEmpty()) {
        painter.setPen(muted);
        painter.drawText(rect(), Qt::AlignCenter, "暂无可用数据");
        return;
    }
    int maximum = 1;
    for (const RoleUsage &role : usage_) maximum = qMax(maximum, role.games);
    const int count = qMax(1, usage_.size());
    const int left = 4;
    const int right = 4;
    const int chartTop = 14;
    const int chartBottom = height() - 22;
    const int gap = 8;
    const int barWidth = qMax(12, (width() - left - right - gap * (count - 1)) / count);

    QFont countFont = painter.font();
    countFont.setPixelSize(10);
    painter.setFont(countFont);
    for (int index = 0; index < usage_.size(); ++index) {
        const RoleUsage &role = usage_.at(index);
        const int x = left + index * (barWidth + gap);
        const QRect base(x, chartTop, barWidth, qMax(1, chartBottom - chartTop));
        const int barHeight = qRound(base.height() * static_cast<qreal>(role.games) / maximum);
        const QRect filled(x, base.bottom() - barHeight + 1, barWidth, barHeight);
        painter.setPen(Qt::NoPen);
        painter.setBrush(border);
        painter.drawRoundedRect(base, 2, 2);
        if (barHeight > 0) {
            painter.setBrush(accent);
            painter.drawRoundedRect(filled, 2, 2);
        }
        painter.setPen(muted);
        painter.drawText(QRect(x - 4, 0, barWidth + 8, 13), Qt::AlignCenter, QString::number(role.games));
        painter.setPen(text);
        painter.drawText(QRect(x - 8, chartBottom + 5, barWidth + 16, 14), Qt::AlignCenter, role.label);
    }
}

} // namespace Janna
