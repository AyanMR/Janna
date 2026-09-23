#pragma once

#include <QPixmap>
#include <QWidget>

namespace Janna {

class ProfileAvatar : public QWidget {
public:
    explicit ProfileAvatar(QWidget *parent = nullptr);

    void setProfile(QPixmap icon, int level, int experienceSinceLevel, int experienceLevelCap);

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QPixmap icon_;
    int level_{};
    int experienceSinceLevel_{};
    int experienceLevelCap_{};
};

} // namespace Janna
