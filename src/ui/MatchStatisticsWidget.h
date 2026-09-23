#pragma once

#include "services/MatchAnalytics.h"

#include <QFrame>

class QVBoxLayout;

namespace Janna {

class MatchStatisticsWidget final : public QFrame {
    Q_OBJECT

public:
    explicit MatchStatisticsWidget(QWidget *parent = nullptr);

    void setStatistics(QList<MatchModeStatistics> statistics);

private:
    void rebuild();

    QVBoxLayout *layout_{};
    QList<MatchModeStatistics> statistics_;
};

} // namespace Janna
