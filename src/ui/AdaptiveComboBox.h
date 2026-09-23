#pragma once

#include <QComboBox>

class QEvent;
class QFrame;
class QListView;
class QModelIndex;
class QPaintEvent;
class QStandardItemModel;
class QToolButton;

namespace Janna {

// A compact closed control with a content-sized, detached list popup.
class AdaptiveComboBox final : public QComboBox {
public:
    explicit AdaptiveComboBox(QWidget *parent = nullptr);
    ~AdaptiveComboBox() override;

    void setPopupMaximumVisibleItems(int count);
    void setCollapsedPopupItemCount(int count);
    void setPopupAbove(bool above);
    void setPopupAlignRight(bool alignRight);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void hidePopup() override;
    QSize sizeHint() const override;
    void paintEvent(QPaintEvent *event) override;
    void showPopup() override;

private:
    void ensurePopup();
    void ensureMenu();
    void showMenuPopup();
    void rebuildMenuItems();
    void positionMenu();
    void showMoreMenu();
    void hideMoreMenu();
    void positionPopup();
    void rebuildPopupItems();
    void selectPopupIndex(const QModelIndex &index);

    QFrame *popup_{};
    QFrame *menu_{};
    QFrame *moreMenu_{};
    QListView *popupView_{};
    QStandardItemModel *popupModel_{};
    QListView *menuView_{};
    QListView *moreView_{};
    QStandardItemModel *menuModel_{};
    QStandardItemModel *moreModel_{};
    QToolButton *expandButton_{};
    int popupMaximumVisibleItems_{10};
    int collapsedPopupItemCount_{};
    bool popupAbove_{};
    bool popupAlignRight_{};
    bool popupExpanded_{};
};

} // namespace Janna
