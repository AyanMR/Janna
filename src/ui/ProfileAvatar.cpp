#include "ui/ProfileAvatar.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>

namespace Janna {
namespace {

QColor applicationColor(const char *name, const QColor &fallback)
{
    if (!qApp) return fallback;
    const QColor color = qApp->property(name).value<QColor>();
    return color.isValid() ? color : fallback;
}

} // namespace

ProfileAvatar::ProfileAvatar(QWidget *parent) : QWidget(parent)
{
    setFixedSize(sizeHint());
}

void ProfileAvatar::setProfile(QPixmap icon, const int level, const int experienceSinceLevel, const int experienceLevelCap)
{
    icon_ = std::move(icon);
    level_ = level;
    experienceSinceLevel_ = qMax(0, experienceSinceLevel);
    experienceLevelCap_ = qMax(experienceSinceLevel_, experienceLevelCap);
    setToolTip(experienceLevelCap_ > 0
        ? QString::number(experienceSinceLevel_) + " / " + QString::number(experienceLevelCap_)
        : "0 / 0");
    update();
}

QSize ProfileAvatar::sizeHint() const { return {86, 108}; }

void ProfileAvatar::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF portraitArea(8, 6, 70, 70);
    const QColor accent = applicationColor("jannaAccent", QColor("#16826c"));
    const QColor muted = applicationColor("jannaMuted", QColor("#758078"));
    const QColor surface = applicationColor("jannaSurface", QColor("#ffffff"));

    painter.setPen(QPen(muted.lighter(150), 4.0, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(portraitArea, 90 * 16, -360 * 16);

    const qreal progress = experienceLevelCap_ > 0
        ? qBound(0.0, static_cast<qreal>(experienceSinceLevel_) / experienceLevelCap_, 1.0)
        : 0.0;
    if (progress > 0.0) {
        painter.setPen(QPen(accent, 4.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(portraitArea, 90 * 16, -static_cast<int>(progress * 360.0 * 16.0));
    }

    const QRectF imageArea = portraitArea.adjusted(5, 5, -5, -5);
    QPainterPath clip;
    clip.addEllipse(imageArea);
    painter.save();
    painter.setClipPath(clip);
    if (!icon_.isNull()) {
        painter.drawPixmap(imageArea.toRect(), icon_.scaled(imageArea.size().toSize(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    } else {
        painter.fillRect(imageArea, accent.lighter(185));
        painter.setPen(accent);
        QFont font = painter.font();
        font.setPixelSize(21);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(imageArea, Qt::AlignCenter, "?");
    }
    painter.restore();

    painter.setPen(applicationColor("jannaText", QColor("#17211d")));
    QFont levelFont = painter.font();
    levelFont.setPixelSize(11);
    levelFont.setBold(true);
    painter.setFont(levelFont);
    painter.drawText(QRectF(0, 82, width(), 20), Qt::AlignCenter, level_ > 0 ? "等级 " + QString::number(level_) : "等级 --");
    painter.setPen(surface);
}

} // namespace Janna
