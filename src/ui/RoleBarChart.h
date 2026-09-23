#pragma once

#include "services/MatchAnalytics.h"

#include <QWidget>

namespace Janna {

class RoleBarChart final : public QWidget {
    Q_OBJECT

public:
    explicit RoleBarChart(QWidget *parent = nullptr);

    void setRoleUsage(QList<RoleUsage> usage);
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QList<RoleUsage> usage_;
};

} // namespace Janna
