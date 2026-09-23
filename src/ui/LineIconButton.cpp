#include "ui/LineIconButton.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <cmath>

namespace Janna {
namespace {
QColor appColor(const char *name, const QColor &fallback)
{
    if (!qApp) return fallback;
    const QColor color = qApp->property(name).value<QColor>();
    return color.isValid() ? color : fallback;
}
}

LineIconButton::LineIconButton(Icon icon, const QString &tooltip, QWidget *parent)
    : QToolButton(parent), icon_(icon)
{
    setToolTip(tooltip);
    setAccessibleName(tooltip);
    setAutoRaise(true);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(38, 38);
}

void LineIconButton::paintEvent(QPaintEvent *)
{
    QStyleOptionToolButton option;
    initStyleOption(&option);
    QPainter painter(this);
    style()->drawComplexControl(QStyle::CC_ToolButton, &option, &painter, this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QColor color = !isEnabled()
        ? palette().color(QPalette::Disabled, QPalette::ButtonText)
        : isChecked() ? appColor("jannaAccent", QColor("#16826c")) : appColor("jannaIcon", QColor("#758078"));
    painter.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    const QRectF area = rect().adjusted(10, 10, -10, -10);
    const QPointF center = area.center();

    switch (icon_) {
    case Icon::Minimize:
        painter.drawLine(area.left(), area.bottom(), area.right(), area.bottom());
        break;
    case Icon::Close:
        painter.drawLine(area.topLeft(), area.bottomRight());
        painter.drawLine(area.bottomLeft(), area.topRight());
        break;
    case Icon::Search:
        painter.drawEllipse(area.adjusted(1, 1, -4, -4));
        painter.drawLine(center.x() + 3, center.y() + 3, area.right() + 1, area.bottom() + 1);
        break;
    case Icon::Refresh:
        painter.drawArc(area, 35 * 16, 285 * 16);
        painter.drawLine(area.right() - 1, area.top() + 3, area.right() - 1, area.top() + 9);
        painter.drawLine(area.right() - 1, area.top() + 3, area.right() - 7, area.top() + 3);
        break;
    case Icon::Profile:
        painter.drawEllipse(QRectF(center.x() - 3.6, area.top(), 7.2, 7.2));
        painter.drawArc(QRectF(area.left() + 1, center.y() - 1, area.width() - 2, 10), 180 * 16, 180 * 16);
        break;
    case Icon::Match:
        painter.drawLine(center.x(), area.top(), center.x(), area.bottom());
        painter.drawArc(QRectF(area.left(), area.top(), area.width() / 2, area.height()), 270 * 16, 180 * 16);
        painter.drawArc(QRectF(center.x(), area.top(), area.width() / 2, area.height()), 90 * 16, 180 * 16);
        break;
    case Icon::Assist:
        painter.drawLine(center.x() - 7, center.y(), center.x() + 7, center.y());
        painter.drawLine(center.x(), center.y() - 7, center.x(), center.y() + 7);
        break;
    case Icon::Favorite: {
        QPainterPath star;
        for (int point = 0; point < 10; ++point) {
            const double angle = -1.5708 + point * 0.6283;
            const double radius = point % 2 == 0 ? 9.2 : 4.2;
            const QPointF position(center.x() + radius * std::cos(angle), center.y() + radius * std::sin(angle));
            point == 0 ? star.moveTo(position) : star.lineTo(position);
        }
        star.closeSubpath();
        if (isChecked()) painter.setBrush(QColor("#D89F37"));
        painter.drawPath(star);
        break;
    }
    case Icon::Opgg: {
        QFont font = painter.font();
        font.setBold(true);
        font.setPixelSize(8);
        painter.setFont(font);
        painter.setPen(appColor("jannaOpgg", QColor("#db644a")));
        painter.drawText(rect(), Qt::AlignCenter, "OP.GG");
        break;
    }
    case Icon::Notice:
        painter.drawArc(area.adjusted(2, 2, -2, -4), 200 * 16, 140 * 16);
        painter.drawLine(area.left() + 2, area.bottom() - 3, area.right() - 2, area.bottom() - 3);
        painter.drawEllipse(QRectF(center.x() - 1.2, area.bottom() - 1, 2.4, 2.4));
        break;
    case Icon::Settings:
        painter.drawEllipse(area.adjusted(4, 4, -4, -4));
        painter.drawEllipse(QRectF(center.x() - 1.8, center.y() - 1.8, 3.6, 3.6));
        for (int tooth = 0; tooth < 8; ++tooth) {
            const double angle = tooth * 0.785;
            painter.drawLine(center.x() + 6 * std::cos(angle), center.y() + 6 * std::sin(angle), center.x() + 9 * std::cos(angle), center.y() + 9 * std::sin(angle));
        }
        break;
    case Icon::Copy:
        painter.drawRect(area.adjusted(3, 0, 0, -3));
        painter.drawRect(area.adjusted(0, 3, -3, 0));
        break;
    }
}

} // namespace Janna
