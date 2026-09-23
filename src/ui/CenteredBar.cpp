#include "ui/CenteredBar.h"

#include <QResizeEvent>

namespace Janna {
namespace {

constexpr int kContentGap = 12;

QSize preferredSize(QWidget *widget, const int maximumHeight)
{
    if (!widget) return {};
    QSize size = widget->sizeHint().expandedTo(widget->minimumSizeHint());
    size.setHeight(qMin(size.height(), maximumHeight));
    return size;
}

} // namespace

CenteredBar::CenteredBar(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground);
}

void CenteredBar::setLeadingWidget(QWidget *widget)
{
    leading_ = widget;
    if (leading_) leading_->setParent(this);
    arrangeWidgets();
}

void CenteredBar::setCenterWidget(QWidget *widget)
{
    center_ = widget;
    if (center_) center_->setParent(this);
    arrangeWidgets();
}

void CenteredBar::setTrailingWidget(QWidget *widget)
{
    trailing_ = widget;
    if (trailing_) trailing_->setParent(this);
    arrangeWidgets();
}

void CenteredBar::updateLayout()
{
    arrangeWidgets();
}

void CenteredBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    arrangeWidgets();
}

void CenteredBar::arrangeWidgets()
{
    const QMargins margins = contentsMargins();
    const int contentHeight = qMax(0, height() - margins.top() - margins.bottom());
    const QSize leadingSize = preferredSize(leading_, contentHeight);
    const QSize trailingSize = preferredSize(trailing_, contentHeight);

    if (leading_) {
        leading_->setGeometry(margins.left(), margins.top() + (contentHeight - leadingSize.height()) / 2,
                              leadingSize.width(), leadingSize.height());
        leading_->show();
    }
    if (trailing_) {
        trailing_->setGeometry(width() - margins.right() - trailingSize.width(), margins.top() + (contentHeight - trailingSize.height()) / 2,
                               trailingSize.width(), trailingSize.height());
        trailing_->show();
    }
    if (!center_) return;

    QSize centerSize = preferredSize(center_, contentHeight);
    const int leftEdge = leading_ ? leading_->geometry().right() + 1 + kContentGap : margins.left();
    const int rightEdge = trailing_ ? trailing_->geometry().left() - kContentGap : width() - margins.right();
    centerSize.setWidth(qMin(centerSize.width(), qMax(0, rightEdge - leftEdge)));
    int centerX = (width() - centerSize.width()) / 2;
    centerX = qBound(leftEdge, centerX, qMax(leftEdge, rightEdge - centerSize.width()));
    center_->setGeometry(centerX, margins.top() + (contentHeight - centerSize.height()) / 2,
                         centerSize.width(), centerSize.height());
    center_->show();
}

} // namespace Janna
