#pragma once

#include <QList>
#include <QPoint>
#include <QString>
#include <QWidget>
#include <memory>

class QFrame;
class QLineEdit;
class QLabel;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;
class QMouseEvent;
class QEvent;
class QPaintEvent;
class QResizeEvent;
class QShowEvent;
class QMoveEvent;
class QVariantAnimation;

namespace Janna {
class ChampionRepository;
class AssistController;
class AssistPage;
class GameFlowRepository;
class GameDataRepository;
class LeagueClientMonitor;
class MatchRepository;
class RankedRepository;
class SearchHistoryStore;
class SummonerRepository;
class ThemeController;
class OpggCrawler;
class OpggWindow;
class MainWindow : public QWidget {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
private slots:
    void updateSearch(const QString &query);
private:
    void setClientAvailable(bool available);
    void setSearchExpanded(bool expanded);
    void showSettingsMenu();
    void updateGamePathPresentation();
    void ensureGameFlowPage();
    void openPlayerProfile(const QString &puuid, const QString &summonerId, const QString &riotId);
    void clearSearchSuggestions();
    void showSearchHistory();
    void updateSuggestionGeometry();

    std::unique_ptr<ChampionRepository> champions_;
    std::unique_ptr<SummonerRepository> summoner_;
    std::unique_ptr<MatchRepository> matches_;
    std::unique_ptr<GameDataRepository> gameData_;
    std::unique_ptr<RankedRepository> ranked_;
    std::unique_ptr<SearchHistoryStore> searchHistory_;
    std::unique_ptr<LeagueClientMonitor> monitor_;
    std::unique_ptr<GameFlowRepository> gameFlow_;
    std::unique_ptr<AssistController> assistController_;
    std::unique_ptr<OpggCrawler> opgg_;
    std::unique_ptr<ThemeController> theme_;
    OpggWindow *opggWindow_{};
    QLineEdit *search_{};
    QFrame *searchShell_{};
    QFrame *suggestions_{};
    QVBoxLayout *suggestionLayout_{};
    QStackedWidget *applicationPages_{};
    QStackedWidget *pages_{};
    QLabel *gamePathLabel_{};
    QToolButton *settingsButton_{};
    QToolButton *summonerNavigationButton_{};
    QToolButton *matchNavigationButton_{};
    QToolButton *assistNavigationButton_{};
    QList<QWidget *> clientControls_;
    QVariantAnimation *searchAnimation_{};
    bool clientAvailable_{};
    bool gameFlowActive_{};
    bool viewingExternalProfile_{};
    bool searchExpanded_{};
    bool initialFocusHandled_{};
    bool dragging_{};
    QPoint dragOffset_;
};
}
