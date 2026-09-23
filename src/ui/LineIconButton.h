#pragma once

#include <QToolButton>

namespace Janna {

class LineIconButton : public QToolButton {
public:
    enum class Icon {
        Minimize,
        Close,
        Search,
        Refresh,
        Profile,
        Match,
        Assist,
        Favorite,
        Opgg,
        Notice,
        Settings,
        Copy
    };

    explicit LineIconButton(Icon icon, const QString &tooltip, QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Icon icon_;
};

} // namespace Janna
