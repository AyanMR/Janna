#pragma once

#include "services/GameFlowRepository.h"
#include "services/OpggCrawler.h"

#include <QHash>
#include <QList>
#include <QPixmap>
#include <QSet>
#include <QWidget>

class QCloseEvent;
class QPaintEvent;
class QResizeEvent;
class QComboBox;
class QLayout;
class QScrollArea;
class QStackedWidget;
class QLabel;
class QPushButton;
class QToolButton;
class QTableWidget;

namespace Janna {

class AssistController;
class ChampionRepository;

// A small modeless tool window for the OPGG data that follows the main
// window.  Keeping it separate from AssistPage lets the user inspect builds
// while the champ-select/game-flow page remains visible.
class OpggWindow final : public QWidget {
    Q_OBJECT

public:
    OpggWindow(ChampionRepository &champions, AssistController &controller,
               GameFlowRepository &gameFlow, OpggCrawler &opgg,
               QWidget *owner = nullptr);

    void setSnapshot(const GameFlowSnapshot &snapshot);
    void showFor(QWidget *owner = nullptr);
    void refreshRanking();
    void syncToOwner();
    void setFollowOwner(bool follow);

signals:
    void buildReady(const OpggBuild &build);
    void buildFailed(const QString &message);
    void closed();

protected:
    void closeEvent(QCloseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
#ifdef JANNA_OPGG_VISUAL_SMOKE
    friend class OpggVisualSmokeAccess;
#endif

    void populateModeOptions();
    void populateLaneOptions(const QString &mode, const QString &preferredLane = {});
    void populateDetailLaneOptions(int championId, const QString &mode, const QString &preferredLane);
    void updateChampionPickerState();
    void populateRankOptions();
    void populateVersionOptions();
    void updateDetectedVersion(const QString &version);
    void updateModeChampionPool();
    void resetModeChampionPool(const QString &mode);
    bool modeUsesRestrictedChampionPool(const QString &mode) const;
    void syncFiltersToSnapshot();
    void applySelectedChampion(int championId, bool revealWindow = false);
    void openChampionPicker();
    void openChampionDetail(int championId);
    void showRankingPage();
    void showDetailPage();
    QString selectedMode() const;
    QString selectedLane() const;
    QString selectedRankTier() const;
    QString selectedVersion() const;
    bool modeSupportsLane(const QString &mode) const;
    void updateContextLabels();
    void renderRanking(const QList<OpggChampionRank> &ranking);
    void renderSelectedRanking();
    void handleChampionLocked(int championId, const QString &lane, const QString &mode,
                              bool revealWindow);
    void fetchBuild(int championId, const QString &lane, const QString &mode);
    void renderBuild(const OpggBuild &build);
    void renderRuneContent(const OpggBuild &build);
    void renderSkillContent(const OpggBuild &build);
    void renderItemContent(const OpggBuild &build);
    void renderCounterContent(const OpggBuild &build);
    void applyRuneVariant(int index);
    void applyItemVariant(int index);
    void setContentPlaceholder(QWidget *content, const QString &text);
    int localLockedChampion(const GameFlowSnapshot &snapshot) const;

    ChampionRepository &champions_;
    AssistController &controller_;
    GameFlowRepository &gameFlow_;
    OpggCrawler &opgg_;
    QWidget *owner_{};
    GameFlowSnapshot snapshot_;
    QList<OpggChampionRank> ranking_;
    OpggBuild build_;
    QList<int> selectedChampionIds_;
    QString rankingMode_;
    QString rankingLane_;
    QString rankingTier_;
    QString rankingVersion_;
    QString detectedVersion_;
    QString automaticMode_;
    QString automaticLane_;
    QString lockedLane_;
    QString lockedMode_;
    qint64 lockedGameId_{};
    int lockedChampionId_{};
    bool rankingRequestInFlight_{};
    bool buildRequestInFlight_{};
    bool buildAttempted_{};
    bool modeChampionPoolRestricted_{};
    bool modeChampionPoolReady_{true};
    QSet<int> modeChampionIds_;

    // OP.GG carries the authoritative rune image URL in the build payload.
    // Keep a small in-memory cache so a missing local CommunityDragon icon can
    // be filled asynchronously without blocking the detail page.
    QHash<int, QString> runeImageUrls_;
    QHash<QString, QPixmap> runeImageCache_;
    QSet<QString> runeImageRequests_;
    QSet<QString> runeImageFailures_;
    bool runeRenderInProgress_{};
    bool runeRefreshQueued_{};

    QLabel *context_{nullptr};
    QLabel *status_{nullptr};
    QComboBox *modeCombo_{nullptr};
    QComboBox *laneCombo_{nullptr};
    QComboBox *rankCombo_{nullptr};
    QComboBox *versionCombo_{nullptr};
    QLabel *selectedChampionLabel_{nullptr};
    QLabel *selectedRankingLabel_{nullptr};
    QPushButton *chooseChampionButton_{nullptr};
    QToolButton *backButton_{nullptr};
    QStackedWidget *contentStack_{nullptr};
    QWidget *rankingPage_{nullptr};
    QWidget *detailPage_{nullptr};
    QScrollArea *detailScroll_{nullptr};
    QComboBox *detailLaneCombo_{nullptr};
    QLabel *detailTitle_{nullptr};
    QLabel *detailMeta_{nullptr};
    QWidget *detailRunes_{nullptr};
    QWidget *detailSkills_{nullptr};
    QWidget *detailItems_{nullptr};
    QWidget *detailCounters_{nullptr};
    QComboBox *runeVariantCombo_{nullptr};
    QComboBox *itemVariantCombo_{nullptr};
    QLabel *buildTitle_{nullptr};
    QLabel *buildDetails_{nullptr};
    QLabel *detailRankIcon_{nullptr};
    QLabel *detailRankText_{nullptr};
    QLabel *detailLaneStats_{nullptr};
    QLabel *detailRole_{nullptr};
    QLabel *detailVersion_{nullptr};
    QPushButton *refreshButton_{nullptr};
    QPushButton *followButton_{nullptr};
    QPushButton *applyButton_{nullptr};
    QPushButton *openLinkButton_{nullptr};
    QTableWidget *rankingTable_{nullptr};
    bool syncingFilters_{};
    bool followOwner_{true};
};

} // namespace Janna
