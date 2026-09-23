#include "ui/AdaptiveComboBox.h"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QGraphicsDropShadowEffect>
#include <QKeyEvent>
#include <QListView>
#include <QModelIndex>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QResizeEvent>
#include <QScreen>
#include <QStandardItemModel>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>

namespace Janna {
namespace {

constexpr int kPopupGap = 6;
constexpr int kPopupMargin = 6;
constexpr int kPopupShadowMargin = 8;
constexpr int kPopupRowHeight = 30;
constexpr int kPopupCornerRadius = 8;

class RoundedPopupFrame final : public QFrame {
public:
    explicit RoundedPopupFrame(QWidget *parent = nullptr, Qt::WindowFlags flags = {}) : QFrame(parent, flags) {}

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QFrame::resizeEvent(event);
        // Translucent popup shells use the styled child surface for their
        // rounded silhouette. A mask would clip the drop shadow to the
        // content bounds and can reintroduce a dark fringe on Windows.
        if (testAttribute(Qt::WA_TranslucentBackground)) {
            clearMask();
            return;
        }
        QPainterPath path;
        path.addRoundedRect(rect().adjusted(0, 0, -1, -1), kPopupCornerRadius, kPopupCornerRadius);
        setMask(QRegion(path.toFillPolygon().toPolygon()));
    }
};

void addPopupShadow(QWidget *widget)
{
    auto *shadow = new QGraphicsDropShadowEffect(widget);
    shadow->setBlurRadius(18);
    shadow->setOffset(0, 4);
    shadow->setColor(QColor(0, 0, 0, 42));
    widget->setGraphicsEffect(shadow);
}

class CenteredComboItemDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem centered = option;
        centered.displayAlignment = Qt::AlignCenter;
        centered.textElideMode = Qt::ElideNone;
        centered.backgroundBrush = Qt::NoBrush;
        const bool highlighted = centered.state.testFlag(QStyle::State_MouseOver)
            || centered.state.testFlag(QStyle::State_Selected);
        if (highlighted) {
            const QColor surface = qApp->property("jannaSurface").value<QColor>();
            painter->save();
            painter->setPen(Qt::NoPen);
            painter->setBrush(surface.isValid() ? surface : centered.palette.color(QPalette::Base));
            painter->drawRoundedRect(centered.rect.adjusted(2, 1, -2, -1), 5, 5);
            painter->restore();
        }
        // The native item view selection fill is opaque and rectangular on
        // Windows. The delegate owns the highlight above, so leave only text
        // and icon painting to the style.
        centered.state &= ~(QStyle::State_MouseOver | QStyle::State_Selected | QStyle::State_HasFocus);
        QStyledItemDelegate::paint(painter, centered, index);
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        hint.setHeight(qMax(kPopupRowHeight, hint.height()));
        hint.setWidth(qMax(hint.width(), option.fontMetrics.horizontalAdvance(index.data(Qt::DisplayRole).toString()) + 32));
        return hint;
    }
};

QScreen *screenFor(const QWidget *widget, const QPoint &globalPoint)
{
    if (QScreen *screen = QGuiApplication::screenAt(globalPoint)) return screen;
    if (widget && widget->screen()) return widget->screen();
    return QGuiApplication::primaryScreen();
}

bool containsGlobalPoint(const QWidget *widget, const QPoint &globalPoint)
{
    return widget && widget->isVisible()
        && widget->rect().contains(widget->mapFromGlobal(globalPoint));
}

} // namespace

AdaptiveComboBox::AdaptiveComboBox(QWidget *parent) : QComboBox(parent)
{
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(0);
    qApp->installEventFilter(this);
    connect(this, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](const int) {
        if (popup_ && popup_->isVisible()) {
            rebuildPopupItems();
            positionPopup();
        }
    });
}

AdaptiveComboBox::~AdaptiveComboBox()
{
    qApp->removeEventFilter(this);
    QFrame *popup = popup_;
    popup_ = nullptr;
    delete popup;
    // menu_ and moreMenu_ are parented to this combo; QObject's child
    // cleanup owns their destruction after this destructor body returns.
    menu_ = nullptr;
    moreMenu_ = nullptr;
}

void AdaptiveComboBox::setPopupMaximumVisibleItems(const int count)
{
    popupMaximumVisibleItems_ = qMax(1, count);
}

void AdaptiveComboBox::setCollapsedPopupItemCount(const int count)
{
    collapsedPopupItemCount_ = qMax(0, count);
    if (menu_ && menu_->isVisible()) {
        showMenuPopup();
        return;
    }
    if (popup_ && popup_->isVisible()) {
        rebuildPopupItems();
        positionPopup();
    }
}

void AdaptiveComboBox::setPopupAbove(const bool above)
{
    popupAbove_ = above;
}

void AdaptiveComboBox::setPopupAlignRight(const bool alignRight)
{
    popupAlignRight_ = alignRight;
}

bool AdaptiveComboBox::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == popup_ && event->type() == QEvent::Hide) {
        popupExpanded_ = false;
        update();
    }
    const bool menuVisible = menu_ && menu_->isVisible();
    const bool moreVisible = moreMenu_ && moreMenu_->isVisible();
    if ((popup_ && popup_->isVisible()) || menuVisible || moreVisible) {
        if (event->type() == QEvent::MouseButtonPress) {
            const auto *mouseEvent = static_cast<const QMouseEvent *>(event);
            const QPoint globalPoint = mouseEvent->globalPosition().toPoint();
            const bool insidePopup = containsGlobalPoint(popup_, globalPoint);
            const bool insideMenu = containsGlobalPoint(menu_, globalPoint);
            const bool insideMoreMenu = containsGlobalPoint(moreMenu_, globalPoint);
            const bool insideCombo = containsGlobalPoint(this, globalPoint);
            if (!insidePopup && !insideMenu && !insideMoreMenu && !insideCombo) {
                // Qt::Popup swallows the first click outside its window. This
                // list deliberately remains a non-activating tool window so
                // the dismissing click can still open a match row underneath.
                hidePopup();
            }
        } else if (event->type() == QEvent::KeyPress
                   && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            hidePopup();
            return true;
        } else if (event->type() == QEvent::ApplicationDeactivate) {
            hidePopup();
        }
    }
    return QComboBox::eventFilter(watched, event);
}

void AdaptiveComboBox::hidePopup()
{
    popupExpanded_ = false;
    if (popup_) popup_->hide();
    hideMoreMenu();
    if (menu_) menu_->hide();
}

QSize AdaptiveComboBox::sizeHint() const
{
    QFontMetrics metrics(font());
    int textWidth = metrics.horizontalAdvance(QString(qMax(0, minimumContentsLength()), QLatin1Char('M')));
    for (int index = 0; index < count(); ++index) {
        textWidth = qMax(textWidth, metrics.horizontalAdvance(itemText(index)));
    }
    QSize hint = QComboBox::sizeHint();
    // The closed control has no arrow subcontrol, so its width is just the
    // widest label plus the border and symmetric text padding.
    hint.setWidth(qMax(52, textWidth + 24));
    return hint;
}

void AdaptiveComboBox::paintEvent(QPaintEvent *)
{
    QStyleOptionComboBox option;
    initStyleOption(&option);
    // Do not ask the native combo style to paint this control. On Windows it
    // reserves and then paints a drop-down subcontrol even when the arrow is
    // disabled, which overlaps short labels such as "20". The combo remains
    // fully interactive; only its closed appearance is drawn here.
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPalette::ColorGroup group = isEnabled() ? QPalette::Active : QPalette::Disabled;
    QColor background = qApp->property("jannaField").value<QColor>();
    QColor border = qApp->property("jannaBorder").value<QColor>();
    QColor textColor = qApp->property("jannaText").value<QColor>();
    QColor accent = qApp->property("jannaAccent").value<QColor>();
    if (!background.isValid()) background = palette().color(group, QPalette::Base);
    if (!border.isValid()) border = palette().color(group, QPalette::Mid);
    if (!textColor.isValid()) textColor = palette().color(group, QPalette::ButtonText);
    if (!accent.isValid()) accent = palette().color(group, QPalette::Highlight);
    if (hasFocus() || underMouse()) border = accent;
    painter.setBrush(background);
    painter.setPen(QPen(border, 1));
    painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 8, 8);
    painter.setPen(isEnabled() ? textColor : palette().color(QPalette::Disabled, QPalette::ButtonText));
    painter.drawText(rect().adjusted(8, 1, -8, -1), Qt::AlignCenter | Qt::TextSingleLine, option.currentText);
}

void AdaptiveComboBox::showPopup()
{
    if (!isEnabled() || count() == 0) return;
    // A collapsed control with a recent-item count always uses the two-level
    // menu.  Keep the "更多" submenu even when every known option currently
    // appears in the recent list; otherwise the menu shape changes as soon as
    // the catalog and match history happen to contain the same set of modes.
    if (collapsedPopupItemCount_ > 0 && count() > 0) {
        if (menu_ && menu_->isVisible()) {
            hidePopup();
            return;
        }
        if (popup_ && popup_->isVisible()) popup_->hide();
        showMenuPopup();
        return;
    }
    ensurePopup();
    if (popup_->isVisible()) {
        hidePopup();
        return;
    }

    popupExpanded_ = false;
    popup_->setPalette(palette());
    popupView_->setPalette(palette());
    popupView_->viewport()->setPalette(palette());
    rebuildPopupItems();
    positionPopup();
    popup_->show();
    popup_->raise();
    popupView_->setFocus(Qt::PopupFocusReason);
}

void AdaptiveComboBox::ensureMenu()
{
    if (menu_) return;

    const auto createListPopup = [this](const char *objectName, QFrame **frameOut,
                                        QListView **viewOut, QStandardItemModel **modelOut) {
        // Keep both transient panels owned by the combo. They still use the
        // tool-window flag, but parent ownership prevents a hidden panel from
        // outliving the combo during application shutdown.
        auto *frame = new RoundedPopupFrame(
            this, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        frame->setObjectName(QString::fromLatin1(objectName) + "Window");
        frame->setAttribute(Qt::WA_ShowWithoutActivating);
        frame->setAttribute(Qt::WA_TranslucentBackground);
        frame->setAttribute(Qt::WA_OpaquePaintEvent, false);
        frame->setAttribute(Qt::WA_NoSystemBackground);
        frame->setAutoFillBackground(false);
        frame->setPalette(palette());
        frame->setAttribute(Qt::WA_DeleteOnClose, false);
        frame->installEventFilter(this);

        auto *outerLayout = new QVBoxLayout(frame);
        outerLayout->setContentsMargins(kPopupShadowMargin, kPopupShadowMargin,
                                        kPopupShadowMargin, kPopupShadowMargin);
        outerLayout->setSpacing(0);

        auto *surface = new QFrame(frame);
        surface->setObjectName(objectName);
        surface->setFrameShape(QFrame::NoFrame);
        surface->setAttribute(Qt::WA_StyledBackground);
        surface->setAutoFillBackground(false);
        surface->setPalette(palette());
        addPopupShadow(surface);

        auto *layout = new QVBoxLayout(surface);
        layout->setContentsMargins(kPopupMargin, kPopupMargin, kPopupMargin, kPopupMargin);
        layout->setSpacing(0);
        auto *model = new QStandardItemModel(frame);
        auto *view = new QListView(frame);
        view->setObjectName(QString::fromLatin1(objectName) + "View");
        view->setFrameShape(QFrame::NoFrame);
        view->setLineWidth(0);
        view->setModel(model);
        view->setItemDelegate(new CenteredComboItemDelegate(view));
        view->setSelectionMode(QAbstractItemView::SingleSelection);
        view->setUniformItemSizes(true);
        view->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        view->setTextElideMode(Qt::ElideNone);
        view->setMouseTracking(true);
        view->setAutoFillBackground(false);
        view->viewport()->setMouseTracking(true);
        view->viewport()->setAttribute(Qt::WA_OpaquePaintEvent, false);
        view->viewport()->setAutoFillBackground(false);
        view->viewport()->setAttribute(Qt::WA_StyledBackground, true);
        layout->addWidget(view);
        outerLayout->addWidget(surface);
        *frameOut = frame;
        *viewOut = view;
        *modelOut = model;
    };

    createListPopup("adaptiveComboMenu", &menu_, &menuView_, &menuModel_);
    createListPopup("adaptiveComboMoreMenu", &moreMenu_, &moreView_, &moreModel_);
    menu_->setFocusProxy(menuView_);
    moreMenu_->setFocusProxy(moreView_);

    connect(menuView_, &QListView::clicked, this, [this](const QModelIndex &index) {
        if (!index.isValid()) return;
        bool valid = false;
        const int sourceIndex = index.data(Qt::UserRole).toInt(&valid);
        if (!valid) return;
        if (sourceIndex == -1) {
            showMoreMenu();
            return;
        }
        if (sourceIndex >= 0 && sourceIndex < count()) {
            setCurrentIndex(sourceIndex);
            hidePopup();
        }
    });
    connect(menuView_, &QListView::activated, menuView_, [this](const QModelIndex &index) {
        if (!index.isValid()) return;
        bool valid = false;
        const int sourceIndex = index.data(Qt::UserRole).toInt(&valid);
        if (!valid) return;
        if (sourceIndex == -1) {
            showMoreMenu();
            return;
        }
        if (sourceIndex >= 0 && sourceIndex < count()) {
            setCurrentIndex(sourceIndex);
            hidePopup();
        }
    });
    connect(menuView_, &QListView::entered, this, [this](const QModelIndex &index) {
        if (!index.isValid()) return;
        bool valid = false;
        const int sourceIndex = index.data(Qt::UserRole).toInt(&valid);
        if (valid && sourceIndex == -1) showMoreMenu();
        else hideMoreMenu();
    });
    connect(moreView_, &QListView::clicked, this, [this](const QModelIndex &index) {
        if (!index.isValid()) return;
        bool valid = false;
        const int sourceIndex = index.data(Qt::UserRole).toInt(&valid);
        if (valid && sourceIndex >= 0 && sourceIndex < count()) {
            setCurrentIndex(sourceIndex);
            hidePopup();
        }
    });
    connect(moreView_, &QListView::activated, moreView_, [this](const QModelIndex &index) {
        if (!index.isValid()) return;
        bool valid = false;
        const int sourceIndex = index.data(Qt::UserRole).toInt(&valid);
        if (valid && sourceIndex >= 0 && sourceIndex < count()) {
            setCurrentIndex(sourceIndex);
            hidePopup();
        }
    });
}

void AdaptiveComboBox::showMenuPopup()
{
    ensureMenu();
    if (popup_) popup_->hide();
    hideMoreMenu();
    menu_->setPalette(palette());
    menuView_->setPalette(palette());
    menuView_->viewport()->setPalette(palette());
    moreMenu_->setPalette(palette());
    moreView_->setPalette(palette());
    moreView_->viewport()->setPalette(palette());
    rebuildMenuItems();
    positionMenu();
    menu_->show();
    menu_->raise();
    menuView_->setFocus(Qt::PopupFocusReason);
}

void AdaptiveComboBox::rebuildMenuItems()
{
    if (!menuModel_ || !moreModel_) return;
    menuModel_->clear();
    moreModel_->clear();

    const int recentCount = qBound(1, collapsedPopupItemCount_, count());
    for (int index = 0; index < recentCount; ++index) {
        auto *item = new QStandardItem(itemIcon(index), itemText(index));
        item->setData(index, Qt::UserRole);
        menuModel_->appendRow(item);
    }
    auto *more = new QStandardItem(QStringLiteral("更多"));
    more->setData(-1, Qt::UserRole);
    menuModel_->appendRow(more);

    for (int index = 0; index < count(); ++index) {
        auto *item = new QStandardItem(itemIcon(index), itemText(index));
        item->setData(index, Qt::UserRole);
        moreModel_->appendRow(item);
    }
    menuView_->setModel(menuModel_);
    moreView_->setModel(moreModel_);
    menuView_->setCurrentIndex({});
    moreView_->setCurrentIndex({});
    for (int row = 0; row < moreModel_->rowCount(); ++row) {
        if (moreModel_->index(row, 0).data(Qt::UserRole).toInt() == currentIndex()) {
            moreView_->setCurrentIndex(moreModel_->index(row, 0));
            break;
        }
    }
}

void AdaptiveComboBox::positionMenu()
{
    if (!menu_ || !menuView_ || !menuModel_) return;
    QStyleOptionViewItem option;
    option.initFrom(this);
    int contentWidth = width();
    for (int row = 0; row < menuModel_->rowCount(); ++row) {
        contentWidth = qMax(contentWidth, option.fontMetrics.horizontalAdvance(menuModel_->index(row, 0).data().toString()) + 42);
    }
    const QPoint anchor = mapToGlobal(QPoint(width() - contentWidth, 0));
    const QScreen *screen = screenFor(this, anchor);
    const QRect available = screen ? screen->availableGeometry() : QRect(anchor, QSize(contentWidth, 600));
    const int surfaceWidth = qMin(contentWidth, qMax(1, available.width() - 24));
    const int windowWidth = surfaceWidth + kPopupShadowMargin * 2;
    const int belowSurfaceY = mapToGlobal(QPoint(0, height() + kPopupGap)).y();
    const int aboveSurfaceBottom = mapToGlobal(QPoint(0, -kPopupGap)).y();
    const int belowWindowY = belowSurfaceY - kPopupShadowMargin;
    const int roomBelow = available.bottom() - belowWindowY + 1;
    const int roomAbove = aboveSurfaceBottom - available.top() + kPopupShadowMargin + 1;
    const int desiredRows = qMin(menuModel_->rowCount(),
                                 qMax(1, (available.height() - kPopupMargin * 2
                                          - kPopupShadowMargin * 2) / kPopupRowHeight));
    const int desiredWindowHeight = desiredRows * kPopupRowHeight
        + kPopupMargin * 2 + kPopupShadowMargin * 2;
    const bool showBelow = !popupAbove_
        && (roomBelow >= desiredWindowHeight || roomBelow >= roomAbove);
    const int room = qMax(kPopupRowHeight + kPopupMargin * 2 + kPopupShadowMargin * 2,
                          showBelow ? roomBelow : roomAbove);
    const int visibleRows = qBound(1, qMin(menuModel_->rowCount(),
                                           (room - kPopupMargin * 2
                                            - kPopupShadowMargin * 2) / kPopupRowHeight),
                                   menuModel_->rowCount());
    const int surfaceHeight = visibleRows * kPopupRowHeight + kPopupMargin * 2;
    const int windowHeight = surfaceHeight + kPopupShadowMargin * 2;
    const int requestedX = mapToGlobal(QPoint(width() - surfaceWidth - kPopupShadowMargin, 0)).x();
    const int x = qBound(available.left() + 2, requestedX, available.right() - windowWidth - 2);
    const int y = showBelow
        ? qBound(available.top() + 2, belowWindowY, available.bottom() - windowHeight + 1)
        : qBound(available.top() + 2, aboveSurfaceBottom - windowHeight + kPopupShadowMargin,
                 available.bottom() - windowHeight + 1);
    menuView_->setFixedHeight(visibleRows * kPopupRowHeight);
    menu_->setGeometry(x, y, windowWidth, windowHeight);
}

void AdaptiveComboBox::showMoreMenu()
{
    if (!menu_ || !menu_->isVisible() || !moreMenu_ || !moreModel_ || moreModel_->rowCount() == 0) return;
    QStyleOptionViewItem option;
    option.initFrom(this);
    int contentWidth = width();
    for (int row = 0; row < moreModel_->rowCount(); ++row) {
        contentWidth = qMax(contentWidth, option.fontMetrics.horizontalAdvance(moreModel_->index(row, 0).data().toString()) + 42);
    }
    const QPoint primary = menu_->mapToGlobal(QPoint(0, 0));
    const QScreen *screen = screenFor(this, primary);
    const QRect available = screen ? screen->availableGeometry() : QRect(primary, QSize(contentWidth, 600));
    const int surfaceWidth = qMin(contentWidth, qMax(1, available.width() - 24));
    const int windowWidth = surfaceWidth + kPopupShadowMargin * 2;
    const int maxRows = qMax(1, (available.height() - kPopupMargin * 2
                                 - kPopupShadowMargin * 2 - 16) / kPopupRowHeight);
    const int visibleRows = qMin(moreModel_->rowCount(), maxRows);
    const QSize surfaceSize(surfaceWidth, visibleRows * kPopupRowHeight + kPopupMargin * 2);
    const QSize windowSize(surfaceSize.width() + kPopupShadowMargin * 2,
                           surfaceSize.height() + kPopupShadowMargin * 2);
    int x = primary.x() - surfaceSize.width();
    if (x < available.left() + 2) x = primary.x() + menu_->width();
    x = qBound(available.left() + 2, x, available.right() - windowSize.width() - 2);
    const int y = qBound(available.top() + 2, primary.y(),
                         available.bottom() - windowSize.height() + 1);
    moreView_->setFixedHeight(visibleRows * kPopupRowHeight);
    moreMenu_->setGeometry(x, y, windowSize.width(), windowSize.height());
    moreMenu_->show();
    moreMenu_->raise();
}

void AdaptiveComboBox::hideMoreMenu()
{
    if (moreMenu_) moreMenu_->hide();
}

void AdaptiveComboBox::rebuildPopupItems()
{
    if (!popupModel_ || !popupView_) return;

    popupModel_->clear();
    QList<int> sourceRows;
    const int collapsedCount = qBound(0, collapsedPopupItemCount_, count());
    const bool canExpand = !popupExpanded_ && collapsedCount > 0 && collapsedCount < count();
    if (canExpand) {
        for (int row = 0; row < collapsedCount; ++row) sourceRows.append(row);
        if (currentIndex() >= collapsedCount) sourceRows.append(currentIndex());
    } else {
        sourceRows.reserve(count());
        for (int row = 0; row < count(); ++row) sourceRows.append(row);
    }

    for (const int sourceRow : sourceRows) {
        auto *item = new QStandardItem(itemIcon(sourceRow), itemText(sourceRow));
        item->setData(sourceRow, Qt::UserRole);
        popupModel_->appendRow(item);
    }

    // Keep the secondary list action inside the QListView. A separate button
    // in this non-activating tool window can lose mouse activation on Windows.
    if (canExpand) {
        auto *more = new QStandardItem(QStringLiteral("更多"));
        more->setData(-1, Qt::UserRole);
        popupModel_->appendRow(more);
    }
    expandButton_->setVisible(false);
    popupView_->setModel(popupModel_);
    popupView_->setTextElideMode(Qt::ElideNone);
    for (int row = 0; row < popupModel_->rowCount(); ++row) {
        if (popupModel_->index(row, 0).data(Qt::UserRole).toInt() != currentIndex()) continue;
        popupView_->setCurrentIndex(popupModel_->index(row, 0));
        return;
    }
    popupView_->setCurrentIndex({});
}

void AdaptiveComboBox::positionPopup()
{
    if (!popup_ || !popupView_ || !popupModel_) return;

    QStyleOptionViewItem option;
    option.initFrom(this);
    int contentWidth = width();
    for (int row = 0; row < popupModel_->rowCount(); ++row) {
        contentWidth = qMax(contentWidth, option.fontMetrics.horizontalAdvance(popupModel_->index(row, 0).data().toString()) + 42);
    }
    const int desiredRows = qMin(popupModel_->rowCount(), popupMaximumVisibleItems_);
    if (popupModel_->rowCount() > desiredRows) {
        contentWidth += style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, popupView_);
    }
    const int expandHeight = expandButton_->isVisible() ? kPopupRowHeight : 0;
    if (expandButton_->isVisible()) {
        contentWidth = qMax(contentWidth, option.fontMetrics.horizontalAdvance(expandButton_->text()) + 36);
    }

    const QPoint anchor = mapToGlobal(QPoint(width() / 2, height()));
    QScreen *screen = screenFor(this, anchor);
    if (!screen) return;
    const QRect available = screen->availableGeometry();
    const int maximumWidth = qMax(width(), available.width() - 24);
    const int popupWidth = qMin(contentWidth, maximumWidth);
    const int requestedX = mapToGlobal(QPoint(popupAlignRight_ ? width() - popupWidth : (width() - popupWidth) / 2, 0)).x();
    const int belowY = mapToGlobal(QPoint(0, height() + kPopupGap)).y();
    const int aboveY = mapToGlobal(QPoint(0, -kPopupGap)).y();
    const int roomBelow = available.bottom() - belowY + 1;
    const int roomAbove = aboveY - available.top() + 1;
    const int desiredHeight = desiredRows * kPopupRowHeight + expandHeight + kPopupMargin * 2;
    const bool showBelow = !popupAbove_ && (roomBelow >= desiredHeight || roomBelow >= roomAbove);
    const int room = qMax(1, showBelow ? roomBelow : roomAbove);
    const int rowsThatFit = qMax(1, (room - expandHeight - kPopupMargin * 2) / kPopupRowHeight);
    const int visibleRows = qMin(desiredRows, rowsThatFit);
    const int height = visibleRows * kPopupRowHeight + expandHeight + kPopupMargin * 2;
    const int x = qBound(available.left() + 8, requestedX, available.right() - popupWidth - 7);
    const int y = showBelow
        ? qBound(available.top() + 8, belowY, available.bottom() - height + 1)
        : qBound(available.top() + 8, aboveY - height, available.bottom() - height + 1);

    popupView_->setFixedHeight(visibleRows * kPopupRowHeight);
    popup_->setGeometry(x, y, popupWidth, height);
    popupView_->scrollTo(popupView_->currentIndex(), QAbstractItemView::PositionAtCenter);
}

void AdaptiveComboBox::ensurePopup()
{
    if (popup_) return;

    popup_ = new RoundedPopupFrame(nullptr, Qt::Tool | Qt::FramelessWindowHint);
    popup_->setObjectName("adaptiveComboPopup");
    popup_->setAttribute(Qt::WA_StyledBackground);
    popup_->setAttribute(Qt::WA_TranslucentBackground, false);
    popup_->setAttribute(Qt::WA_OpaquePaintEvent);
    popup_->setAttribute(Qt::WA_ShowWithoutActivating);
    popup_->setAutoFillBackground(true);
    popup_->setPalette(palette());
    popup_->setAttribute(Qt::WA_DeleteOnClose, false);
    popup_->installEventFilter(this);

    auto *layout = new QVBoxLayout(popup_);
    layout->setContentsMargins(kPopupMargin, kPopupMargin, kPopupMargin, kPopupMargin);
    layout->setSpacing(0);
    popupModel_ = new QStandardItemModel(popup_);
    popupView_ = new QListView(popup_);
    popupView_->setObjectName("adaptiveComboPopupView");
    popupView_->setModel(popupModel_);
    popupView_->setItemDelegate(new CenteredComboItemDelegate(popupView_));
    popupView_->setSelectionMode(QAbstractItemView::SingleSelection);
    popupView_->setUniformItemSizes(true);
    popupView_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    popupView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    popupView_->setTextElideMode(Qt::ElideNone);
    popupView_->setMouseTracking(true);
    popupView_->setAutoFillBackground(false);
    popupView_->viewport()->setMouseTracking(true);
    popupView_->viewport()->setAttribute(Qt::WA_OpaquePaintEvent, false);
    popupView_->viewport()->setAutoFillBackground(false);
    popupView_->viewport()->setAttribute(Qt::WA_StyledBackground, true);
    layout->addWidget(popupView_);
    expandButton_ = new QToolButton(popup_);
    expandButton_->setObjectName("adaptiveComboExpand");
    expandButton_->setText("展开");
    expandButton_->setFixedHeight(kPopupRowHeight);
    expandButton_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(expandButton_);
    popup_->setFocusProxy(popupView_);

    connect(popupView_, &QListView::clicked, this, &AdaptiveComboBox::selectPopupIndex);
    connect(popupView_, &QListView::activated, this, &AdaptiveComboBox::selectPopupIndex);
    connect(expandButton_, &QToolButton::clicked, this, [this] {
        popupExpanded_ = true;
        rebuildPopupItems();
        positionPopup();
        popup_->raise();
        popupView_->setFocus(Qt::PopupFocusReason);
    });
}

void AdaptiveComboBox::selectPopupIndex(const QModelIndex &index)
{
    if (!index.isValid()) return;
    bool validSourceIndex = false;
    const int sourceIndex = index.data(Qt::UserRole).toInt(&validSourceIndex);
    if (validSourceIndex && sourceIndex == -1) {
        popupExpanded_ = true;
        rebuildPopupItems();
        positionPopup();
        popup_->raise();
        popupView_->setFocus(Qt::PopupFocusReason);
        return;
    }
    if (!validSourceIndex || sourceIndex < 0 || sourceIndex >= count()) return;
    setCurrentIndex(sourceIndex);
    hidePopup();
}

} // namespace Janna
