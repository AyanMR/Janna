#pragma once

#include <QWidget>

class QResizeEvent;

namespace Janna {

// Keeps the center content at the window's true midpoint regardless of side controls.
class CenteredBar final : public QWidget {
public:
    explicit CenteredBar(QWidget *parent = nullptr);

    void setLeadingWidget(QWidget *widget);
    void setCenterWidget(QWidget *widget);
    void setTrailingWidget(QWidget *widget);
    void updateLayout();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void arrangeWidgets();

    QWidget *leading_{};
    QWidget *center_{};
    QWidget *trailing_{};
};

} // namespace Janna
