#pragma once

#include "services/AssistController.h"

#include <QList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QVBoxLayout;

namespace Janna {

class ChampionRepository;
class GameDataRepository;
class OpggCrawler;

class AssistPage final : public QWidget {
    Q_OBJECT

public:
    AssistPage(ChampionRepository &champions, GameDataRepository &gameData,
               GameFlowRepository &gameFlow, AssistController &controller,
               OpggCrawler &opgg, QWidget *parent = nullptr);

    void setSnapshot(const GameFlowSnapshot &snapshot);
    void refreshRanking();
    void setRecommendedBuild(const OpggBuild &build);
    void setRecommendedBuildError(const QString &message);

signals:
    void opggRequested();

private:
    void rebuildChampionOptions(QComboBox *combo, int selectedId);
    void loadScopeSettings();
    void saveScopeSettings();
    void renderRanking(const QList<OpggChampionRank> &ranking);
    void renderBuild(const OpggBuild &build);
    QString selectedScope() const;
    int comboChampionId(const QComboBox *combo) const;

    ChampionRepository &champions_;
    GameDataRepository &gameData_;
    GameFlowRepository &gameFlow_;
    AssistController &controller_;
    OpggCrawler &opgg_;
    GameFlowSnapshot snapshot_;
    QList<OpggChampionRank> ranking_;
    OpggBuild build_;
    QString buildLane_;
    QString buildMode_;
    bool updatingControls_{};
    QString scope_{"default"};

    QLabel *status_{nullptr};
    QLabel *mode_{nullptr};
    QLabel *lane_{nullptr};
    QComboBox *scopeCombo_{nullptr};
    QComboBox *banCombo_{nullptr};
    QComboBox *pickCombo_{nullptr};
    QCheckBox *autoBan_{nullptr};
    QCheckBox *autoPick_{nullptr};
    QCheckBox *autoLock_{nullptr};
    QPushButton *rankingButton_{nullptr};
    QTableWidget *rankingTable_{nullptr};
    QLabel *buildTitle_{nullptr};
    QLabel *buildDetails_{nullptr};
    QPushButton *applyBuildButton_{nullptr};
};

} // namespace Janna
