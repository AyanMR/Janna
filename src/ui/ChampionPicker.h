#pragma once

#include <QDialog>
#include <QList>
#include <QPair>
#include <QSet>
#include <QString>

#include "model/Champion.h"

class QGridLayout;
class QHBoxLayout;
class QComboBox;
class QLineEdit;
class QShowEvent;
class QPaintEvent;
class QResizeEvent;
class QTimer;
class QVBoxLayout;

namespace Janna {
class ChampionRepository;

class ChampionPicker : public QDialog {
    Q_OBJECT
public:
    explicit ChampionPicker(ChampionRepository &repository, QWidget *parent = nullptr);
    QList<int> selectedChampionIds() const { return selected_; }
    void setAllowedChampionIds(const QSet<int> &ids);
    void setPoolRestricted(bool restricted);
    void setSelectedChampionIds(const QList<int> &ids);
    // OP.GG can scope the picker to the currently selected lane without
    // changing the main-window picker, whose type filter is still useful.
    void setLaneFilter(const QString &lane, const QSet<int> &laneChampionIds = {});

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void done(int result) override;

private slots:
    void rebuild();
    void populateTypeOptions();
    void populateLaneOptions();
    void selectChampion(int id);
    void saveAlias();

private:
    ChampionRepository &repository_;
    QLineEdit *filter_{};
    QComboBox *typeFilter_{};
    QComboBox *laneFilter_{};
    QHBoxLayout *typeFilterRow_{};
    QHBoxLayout *laneFilterRow_{};
    QLineEdit *alias_{};
    QGridLayout *grid_{};
    QVBoxLayout *selectedLayout_{};
    QTimer *assetRefreshTimer_{};
    QList<int> selected_;
    int focusedChampionId_{};
    bool opening_{};
    bool closing_{};
    bool poolRestricted_{};
    QSet<int> allowedChampionIds_;
    QSet<int> laneChampionIds_;
    QString laneFilterValue_;
    bool laneFilterEnabled_{};
};
}
