#include "ui/ThemeController.h"

#include <QApplication>
#include <QColor>
#include <QGuiApplication>
#include <QHash>
#include <QPalette>
#include <QSettings>
#include <QStyleHints>

namespace Janna {
namespace {
struct ThemeColors {
    QString window;
    QString surface;
    QString raised;
    QString field;
    QString text;
    QString muted;
    QString border;
    QString matchWinBackground;
    QString matchLossBackground;
    QString matchWinBorder;
    QString matchLossBorder;
    QString accent;
    QString accentSoft;
    QString accentHover;
    QString danger;
    QString dangerSoft;
    QString scrollbar;
    QString opgg;
};

ThemeColors colorsFor(const bool dark)
{
    if (dark) {
        return {
            "#171A18", "#1D211F", "#242A27", "#272E2B", "#EEF3EF", "#929D96", "#38413C",
            "#163A2F", "#49262B", "#2DDB9A", "#FF6573", "#53D9BD", "#213A33", "#73E4CA",
            "#F05D69", "#4B2B2F", "#69766E", "#EF775D"
        };
    }
    return {
        "#F4F7F5", "#FFFFFF", "#F9FBF9", "#F2F6F3", "#202723", "#758078", "#DDE5DF",
        "#EAFBF3", "#FFF0F1", "#00A96B", "#E84454", "#16826C", "#E1F3EC", "#0F967B",
        "#C94F58", "#FBEAEC", "#9AA69E", "#DB644A"
    };
}

QString styleSheet(const ThemeColors &colors)
{
    QString style = QStringLiteral(R"(
        QWidget { color: @text@; font-family: "Microsoft YaHei UI"; }
        QWidget#window { background: @window@; border: 1px solid @border@; border-radius: 10px; }
        QDialog#championPicker { background: @surface@; color: @text@; border: 1px solid @border@; border-radius: 10px; }
        QWidget#topBar { min-height: 62px; max-height: 62px; background: @surface@; border-bottom: 1px solid @border@; border-top-left-radius: 9px; border-top-right-radius: 9px; }
        QWidget#bottomBar { min-height: 60px; max-height: 60px; background: @surface@; border-top: 1px solid @border@; border-bottom-left-radius: 9px; border-bottom-right-radius: 9px; }
        QLabel#brandMark { min-width: 28px; max-width: 28px; min-height: 28px; max-height: 28px; background: @accent@; color: @surface@; border-radius: 7px; font-size: 16px; font-weight: 800; }
        QLabel#brand { color: @text@; font-size: 14px; font-weight: 800; letter-spacing: 2px; }
        QLabel#context, QLabel#eyebrow, QLabel#pickerEyebrow { color: @muted@; font-size: 10px; font-weight: 700; letter-spacing: 1px; }
        QFrame#headerDivider { color: @border@; margin: 14px 6px; }
        QFrame#searchShell { min-height: 38px; max-height: 38px; background: @field@; border: 1px solid @border@; border-radius: 9px; }
        QFrame#searchShell:focus { border-color: @accent@; }
        QLineEdit { background: @field@; color: @text@; border: 1px solid @border@; border-radius: 8px; padding: 8px 10px; selection-background-color: @accent@; selection-color: @surface@; }
        QLineEdit:focus { border-color: @accent@; }
        QLineEdit#globalSearch { background: transparent; border: 0; padding: 0 4px; font-size: 12px; }
        QLineEdit::placeholder { color: @muted@; }
        QLabel#summonerName { color: @text@; font-size: 23px; font-weight: 700; }
        QLabel#summonerName span { color: @muted@; font-size: 14px; font-weight: 500; }
        QLabel#muted, QLabel#empty { color: @muted@; font-size: 12px; }
        QLabel#profilePortrait { background: @accentSoft@; color: @accent@; border: 2px solid @accent@; border-radius: 35px; font-size: 24px; font-weight: 700; }
        QLabel#sectionTitle, QLabel#heading, QLabel#pickerHeading, QLabel#pickerSection { color: @text@; font-weight: 700; }
        QLabel#sectionTitle { font-size: 16px; }
        QLabel#heading { font-size: 25px; }
        QLabel#pickerHeading { font-size: 22px; }
        QLabel#pickerSection { font-size: 14px; }
        QLabel#panelLabel { color: @muted@; font-size: 11px; }
        QLabel#roleChartToggle { color: @muted@; background: transparent; padding: 0; font-size: 11px; }
        QFrame#rolePanel:hover { background: @raised@; border-color: @accent@; }
        QLabel#roleBlank, QLabel#metric { color: @text@; font-size: 12px; }
        QLabel#roleBlank { font-size: 13px; font-weight: 600; }
        QLabel#matchSummary { color: @muted@; font-size: 12px; }
        QLabel#matchTitle { color: @text@; font-size: 13px; font-weight: 700; }
        QLabel#matchKda { color: @text@; font-size: 13px; font-weight: 700; }
        QLabel#matchSmall { color: @muted@; font-size: 11px; }
        QLabel#matchChampionPortrait { background: @surface@; border: 1px solid @border@; border-radius: 7px; color: @muted@; font-size: 10px; }
        QLabel#matchAssetIcon { background: @surface@; border: 1px solid @border@; border-radius: 5px; color: @muted@; font-size: 8px; }
        QLabel#rankEmblem { background: transparent; border: 0; }
        QWidget#matchSummaryItems, QWidget#matchItemIcons, QWidget#matchSpellIcons { background: transparent; }
        QFrame#matchRowWin, QFrame#matchRowLoss { border-radius: 8px; }
        QFrame#matchRowWin { background: @matchWinBackground@; border: 2px solid @matchWinBorder@; }
        QFrame#matchRowLoss { background: @matchLossBackground@; border: 2px solid @matchLossBorder@; }
        QWidget#matchHistory { background: @window@; }
        QFrame#matchDetails { background: @surface@; border: 1px solid @border@; border-radius: 9px; }
        QLabel#matchDetailHeader { color: @text@; font-size: 12px; font-weight: 700; padding: 1px 2px; }
        QLabel#matchDetailPending { color: @muted@; font-size: 12px; }
        QLabel#matchDetailError { color: @danger@; font-size: 12px; }
        QFrame#matchTeamWin, QFrame#matchTeamLoss { border: 1px solid @border@; border-radius: 7px; }
        QFrame#matchTeamWin { background: @accentSoft@; border-left: 3px solid @accent@; }
        QFrame#matchTeamLoss { background: @dangerSoft@; border-left: 3px solid @danger@; }
        QLabel#matchTeamName { color: @text@; font-size: 13px; font-weight: 700; }
        QLabel#matchTeamOutcome { color: @text@; font-size: 12px; font-weight: 700; }
        QLabel#matchTeamObjectives, QLabel#matchTeamKda { color: @muted@; font-size: 11px; }
        QLabel#matchTeamKda { color: @text@; font-weight: 700; }
        QLabel#matchDetailColumnHeader { color: @muted@; font-size: 10px; }
        QFrame#matchPlayerRow { background: @surface@; border: 1px solid @border@; border-radius: 6px; }
        QToolButton#matchPlayerName { background: transparent; border: 0; color: @text@; font-size: 12px; font-weight: 600; padding: 0; text-align: left; }
        QToolButton#matchPlayerName:hover { background: transparent; color: @accent@; text-decoration: underline; }
        QLabel#matchHistoryPrivateLock { background: transparent; border: 0; font-family: "Segoe UI Emoji"; font-size: 12px; }
        QLabel#matchTier { color: @muted@; font-size: 10px; }
        QLabel#matchPlayerKda { color: @text@; font-size: 11px; font-weight: 700; }
        QLabel#matchPlayerCs, QLabel#matchPlayerStats { color: @muted@; font-size: 10px; }
        QFrame#matchChampionCell { background: @raised@; border: 1px solid @border@; border-radius: 6px; }
        QLabel#championLevelBadge { background: @text@; color: @surface@; border-radius: 8px; font-size: 8px; font-weight: 700; }
        QFrame#runeHoverCard { background: @surface@; border: 1px solid @border@; border-radius: 5px; }
        QLabel#runeHoverIcon { background: @field@; border: 1px solid @border@; border-radius: 3px; color: @muted@; font-size: 9px; }
        QLabel#runeHoverName { color: @text@; font-size: 10px; }
        QLabel#emptyTitle { color: @text@; font-size: 16px; font-weight: 700; }
        QLabel#statusPill { background: @accentSoft@; color: @accent@; border-radius: 10px; padding: 4px 9px; font-size: 11px; }
        QPushButton { color: @text@; border: 0; background: transparent; padding: 7px 10px; border-radius: 5px; }
        QPushButton:hover { background: @raised@; }
        QPushButton:disabled, QToolButton:disabled { color: @muted@; }
        QPushButton#primary { background: @accent@; color: @surface@; min-width: 92px; font-weight: 700; border-radius: 8px; }
        QPushButton#primary:hover { background: @accentHover@; }
        QPushButton#secondary { background: @field@; color: @text@; border: 1px solid @border@; min-width: 92px; border-radius: 8px; }
        QPushButton#secondary:hover { background: @raised@; border-color: @accent@; }
        QPushButton#copy { color: @accent@; padding: 0; font-size: 11px; text-align: left; }
        QPushButton#tab { color: @muted@; border-radius: 0; padding: 8px 14px; }
        QPushButton#tab:checked { color: @accent@; border-bottom: 2px solid @accent@; }
        QPushButton#pickerClose { color: @muted@; font-size: 21px; padding: 0; }
        QPushButton#pickerClose:hover { background: @dangerSoft@; color: @danger@; }
        QPushButton#championTile { background: @raised@; border: 1px solid @border@; border-radius: 5px; padding: 0; }
        QPushButton#championTile:hover { background: @accentSoft@; border-color: @accent@; }
        QPushButton#championTile[championSelected="true"] { background: @accentSoft@; border: 2px solid @accent@; }
        QPushButton#remove { color: @muted@; font-size: 18px; padding: 0 5px; }
        QToolButton { border-radius: 6px; }
        QToolButton:hover { background: @raised@; }
        QToolButton:checked { background: @accentSoft@; }
        QToolButton#closeButton:hover { background: @dangerSoft@; }
        QFrame#rolePanel, QFrame#pickerPanel { background: @surface@; border: 1px solid @border@; border-radius: 8px; }
        QFrame#stats { background: @surface@; border: 1px solid @border@; border-radius: 8px; }
        QWidget#statisticsHeader { background: @field@; border-top-left-radius: 7px; border-top-right-radius: 7px; }
        QWidget#statisticsRow { border-top: 1px solid @border@; }
        QLabel#statTableCell { color: @text@; font-size: 11px; min-height: 20px; padding: 0 8px; border-right: 1px solid @border@; }
        QWidget#statisticsHeader QLabel#statTableCell { color: @muted@; font-size: 10px; font-weight: 700; }
        QLabel#statTableCell:last-child { border-right: 0; }
        QWidget#recentChampionUsage { background: transparent; }
        QToolButton#recentChampionButton { background: @surface@; border: 1px solid @border@; border-radius: 5px; padding: 1px; color: @muted@; font-size: 10px; }
        QToolButton#recentChampionButton:hover { border-color: @accent@; background: @accentSoft@; }
        QToolButton#recentChampionButton:checked { border: 2px solid @accent@; background: @accentSoft@; }
        QWidget#recentChampionPanel { background: @surface@; border: 1px solid @border@; border-radius: 8px; }
        QFrame#emptyState { background: @raised@; border: 1px dashed @border@; border-radius: 8px; min-height: 300px; }
        QFrame#suggestions { background: @surface@; border: 1px solid @border@; border-radius: 10px; min-width: 386px; max-width: 456px; }
        QPushButton#option { color: @text@; text-align: left; padding: 9px 10px; }
        QPushButton#option:hover { background: @accentSoft@; }
        QLabel#searchHistoryTitle { color: @muted@; font-size: 11px; font-weight: 700; }
        QPushButton#clearSearchHistory { color: @muted@; font-size: 11px; padding: 2px 5px; }
        QPushButton#clearSearchHistory:hover { color: @danger@; background: @dangerSoft@; }
        QPushButton#searchHistoryEntry { color: @text@; text-align: left; padding: 8px 10px; }
        QPushButton#searchHistoryEntry:hover { background: @accentSoft@; }
        QWidget#searchHistoryEntryRow { background: @surface@; border-radius: 6px; }
        QToolButton#searchHistoryRemove { color: @muted@; background: transparent; border: 0; font-size: 16px; padding: 0; }
        QToolButton#searchHistoryRemove:hover { color: @danger@; background: @dangerSoft@; }
        QWidget#championSearchResult { background: transparent; border-radius: 5px; }
        QLabel#searchChampionPortrait { background: @field@; border: 1px solid @border@; border-radius: 4px; color: @muted@; font-size: 10px; }
        QLabel#searchChampionTitle { color: @muted@; font-size: 11px; }
        QLabel#searchChampionName { color: @text@; font-size: 12px; font-weight: 700; }
        QLabel#result { color: @muted@; padding: 8px 10px; }
        QLabel#portrait { background: @accentSoft@; color: @accent@; border-radius: 21px; font-weight: 700; }
        QLabel#alias { color: @muted@; font-size: 9px; }
        QLabel#championType { color: @accent@; font-size: 9px; }
        QLabel#pickerFilterLabel { color: @muted@; font-size: 11px; }
        QComboBox#championTypeFilter { min-height: 28px; }
        QComboBox { background: @field@; color: @text@; border: 1px solid @border@; border-radius: 8px; min-height: 20px; padding: 4px 10px 4px 9px; min-width: 52px; }
        QComboBox:hover, QComboBox:focus, QComboBox:on { background: @surface@; border-color: @accent@; }
        QComboBox#matchCountCombo { min-width: 0; padding-left: 8px; padding-right: 8px; }
        QComboBox#matchModeCombo { min-width: 0; padding-left: 10px; padding-right: 10px; }
        QFrame#adaptiveComboPopup { background: @surface@; color: @text@; border: 1px solid @border@; border-radius: 8px; }
        QFrame#adaptiveComboPopup QListView#adaptiveComboPopupView, QFrame#adaptiveComboPopup QListView#adaptiveComboPopupView::viewport { background: @surface@; color: @text@; border: 0; outline: 0; selection-background-color: transparent; selection-color: @text@; }
        QFrame#adaptiveComboPopup QListView#adaptiveComboPopupView::item { background: @surface@; min-height: 30px; padding: 0 10px; border-radius: 5px; }
        QFrame#adaptiveComboPopup QListView#adaptiveComboPopupView::item:hover, QFrame#adaptiveComboPopup QListView#adaptiveComboPopupView::item:selected { background: @surface@; color: @text@; }
        QToolButton#adaptiveComboExpand { color: @muted@; background: transparent; border: 0; border-top: 1px solid @border@; border-bottom-left-radius: 6px; border-bottom-right-radius: 6px; font-size: 11px; }
        QToolButton#adaptiveComboExpand:hover { color: @accent@; background: @accentSoft@; }
        QFrame#adaptiveComboMenuWindow, QFrame#adaptiveComboMoreMenuWindow { background: transparent; border: 0; }
        QFrame#adaptiveComboMenu, QFrame#adaptiveComboMoreMenu { background: @surface@; color: @text@; border: 1px solid @border@; border-radius: 9px; }
        QFrame#adaptiveComboMenu QListView, QFrame#adaptiveComboMoreMenu QListView, QFrame#adaptiveComboMenu QListView::viewport, QFrame#adaptiveComboMoreMenu QListView::viewport { background: transparent; color: @text@; border: 0; outline: 0; selection-background-color: transparent; selection-color: @text@; }
        QFrame#adaptiveComboMenu QListView::item, QFrame#adaptiveComboMoreMenu QListView::item { background: @surface@; min-height: 30px; padding: 0 10px; border-radius: 5px; }
        QFrame#adaptiveComboMenu QListView::item:hover, QFrame#adaptiveComboMenu QListView::item:selected, QFrame#adaptiveComboMoreMenu QListView::item:hover, QFrame#adaptiveComboMoreMenu QListView::item:selected { background: @surface@; color: @text@; }
        QToolTip { background-color: @surface@; color: @text@; border: 1px solid @border@; border-radius: 6px; padding: 6px 8px; font-size: 11px; }
         QScrollArea { border: 0; background: @window@; border-radius: 7px; }
         QScrollArea#gameFlowScroll, QScrollArea#gameFlowScroll QWidget#qt_scrollarea_viewport { background: @window@; }
         QScrollArea#championCatalogScroll, QWidget#championCatalogViewport, QAbstractScrollArea#championCatalogScroll QWidget#championCatalogViewport, QWidget#championCatalog, QWidget#selectedChampionView { background: @surface@; }
         QScrollArea#opggDetailScroll, QScrollArea#opggDetailScroll QWidget#qt_scrollarea_viewport, QWidget#opggDetailPage { background: @window@; }
        QScrollBar:vertical { width: 12px; background: transparent; margin: 6px 0; }
        QScrollBar::handle:vertical { background: @scrollbar@; border-radius: 3px; min-height: 32px; margin: 0 3px; }
        QScrollBar::handle:vertical:hover { background: @accent@; border-radius: 5px; margin: 0 1px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
        QFrame#clientPlaceholder { background: @window@; }
        QFrame#placeholderPanel { max-width: 440px; background: @surface@; border: 1px solid @border@; border-radius: 10px; }
        QFrame#placeholderBeacon { min-width: 50px; max-width: 50px; min-height: 50px; max-height: 50px; background: @accentSoft@; border: 1px solid @accent@; border-radius: 25px; }
        QLabel#placeholderSignal { color: @accent@; font-size: 11px; font-weight: 800; letter-spacing: 1px; }
        QLabel#placeholderTitle { color: @text@; font-size: 18px; font-weight: 700; }
        QLabel#placeholderDetail, QLabel#placeholderPath { color: @muted@; font-size: 12px; }
        QLabel#placeholderPath { background: @field@; border-radius: 4px; padding: 7px 9px; }
        QMenu#settingsMenu { background: @surface@; color: @text@; border: 1px solid @border@; padding: 5px; }
        QMenu#settingsMenu::item { padding: 7px 26px 7px 10px; border-radius: 4px; }
        QMenu#settingsMenu::item:selected { background: @accentSoft@; }
        QMenu#settingsMenu::item:disabled { color: @muted@; }
        QMenu#settingsMenu::separator { height: 1px; background: @border@; margin: 5px 7px; }
        QMessageBox { background: @surface@; color: @text@; }
    )");
    style += QStringLiteral(R"(
        QWidget#opggWindow { background: @window@; border: 1px solid @border@; border-radius: 11px; }
        QFrame#opggHeader { background: @surface@; border-bottom: 1px solid @border@; border-top-left-radius: 10px; border-top-right-radius: 10px; }
        QWidget#opggFilters { background: @window@; }
        QLabel#opggFilterLabel { color: @muted@; font-size: 11px; font-weight: 700; }
         QComboBox#opggModeCombo, QComboBox#opggLaneCombo, QComboBox#opggRankCombo, QComboBox#opggVersionCombo, QComboBox#opggDetailLaneCombo { min-height: 18px; padding-top: 3px; padding-bottom: 3px; }
        QLabel#opggTitle { color: @text@; font-size: 18px; font-weight: 800; }
        QLabel#opggContext, QLabel#opggStatus { color: @muted@; font-size: 11px; }
        QLabel#opggSelectedChampion { color: @text@; background: @surface@; border: 1px solid @border@; border-radius: 5px; padding: 5px 7px; }
        QLabel#opggSelectedRanking { color: @text@; background: @accentSoft@; border: 1px solid @border@; border-left: 3px solid @accent@; border-radius: 5px; padding: 6px 8px; }
        QFrame#opggPanel { background: @surface@; border: 1px solid @border@; border-radius: 8px; }
        QLabel#opggSectionTitle { color: @text@; font-size: 14px; font-weight: 700; }
        QLabel#opggDetails { color: @muted@; font-size: 11px; }
        QLabel#opggDetailMetric { color: @text@; font-size: 11px; font-weight: 600; }
        QLabel#opggRankIcon { background: @field@; border: 1px solid @border@; border-radius: 15px; }
        QWidget#opggDetailSummary { background: @raised@; border: 1px solid @border@; border-radius: 6px; }
        QWidget#opggSkillContent { background: transparent; }
        QLabel#opggSkillBadge { background: @field@; color: @text@; border: 1px solid @border@; border-radius: 5px; font-size: 11px; font-weight: 800; }
        QLabel#opggSkillLevel { color: @muted@; font-size: 9px; }
        QLabel#opggAssetSection { color: @muted@; font-size: 10px; font-weight: 700; margin-top: 3px; }
        QWidget#opggAssetChip { background: @raised@; border: 1px solid @border@; border-radius: 6px; }
        QWidget#opggRuneOption { min-width: 78px; }
        QWidget#opggRuneOption[runeSelected="true"] { background: @accentSoft@; border: 2px solid @accent@; }
        QWidget#opggRuneOption[runeSelected="false"] { background: @raised@; border: 1px solid @border@; }
        QWidget#opggRuneOption:disabled { background: @field@; border: 1px dashed @border@; }
        QWidget#opggRuneOption[runeSelected="true"] QLabel#opggAssetIcon { border-color: @accent@; }
        QLabel#opggAssetIcon { background: @field@; border: 1px solid @border@; border-radius: 5px; color: @muted@; font-size: 16px; font-weight: 700; }
        QWidget#opggRuneOption QLabel#opggAssetIcon { border-radius: 50%; background: transparent; }
        QLabel#opggAssetName { color: @text@; font-size: 10px; }
        QLabel#opggRuneRowLabel { color: @muted@; font-size: 10px; font-weight: 700; padding-top: 2px; }
        QFrame#opggItemGroup { background: @raised@; border: 1px solid @border@; border-radius: 5px; }
        QLabel#opggItemGroupLabel { color: @muted@; font-size: 10px; font-weight: 700; }
        QLabel#opggItemRate { color: @accent@; font-size: 10px; font-weight: 700; min-width: 38px; }
        QFrame#opggCounterCard { background: @raised@; border: 1px solid @border@; border-radius: 6px; }
        QLabel#opggCounterPortrait { background: @field@; border: 1px solid @border@; border-radius: 21px; color: @muted@; }
        QLabel#opggCounterName { color: @text@; font-size: 11px; font-weight: 700; }
        QLabel#opggCounterGood { color: @accent@; font-size: 10px; font-weight: 700; }
        QLabel#opggCounterBad { color: @danger@; font-size: 10px; font-weight: 700; }
        QLabel#opggCounterRate { color: @muted@; font-size: 9px; }
        QTableWidget#opggRankingTable { background: @raised@; alternate-background-color: @surface@; color: @text@; border: 1px solid @border@; gridline-color: transparent; selection-background-color: @accentSoft@; selection-color: @text@; }
        QTableWidget#opggRankingTable QHeaderView::section { background: @field@; color: @muted@; border: 0; border-bottom: 1px solid @border@; padding: 6px 7px; font-size: 10px; font-weight: 700; }
        QTableWidget#opggRankingTable::item { padding: 5px 7px; }
        QFrame#assistPanel { background: @surface@; border: 1px solid @border@; border-radius: 8px; }
        QTableWidget#assistRankingTable { background: @raised@; alternate-background-color: @surface@; color: @text@; border: 1px solid @border@; gridline-color: @border@; selection-background-color: @accentSoft@; selection-color: @text@; }
        QTableWidget#assistRankingTable QHeaderView::section { background: @field@; color: @muted@; border: 0; border-bottom: 1px solid @border@; padding: 6px 8px; font-size: 10px; font-weight: 700; }
        QTableWidget#assistRankingTable::item { padding: 5px 8px; }
        QLabel#gameFlowMeta { color: @muted@; font-size: 12px; }
        QLabel#gameFlowTimer { color: @accent@; min-width: 48px; font-size: 14px; font-weight: 700; }
        QFrame#gameFlowTeamOwn, QFrame#gameFlowTeamEnemy { background: @surface@; border: 1px solid @border@; border-radius: 10px; }
        QFrame#gameFlowRecentPanel { background: @surface@; border: 1px solid @border@; border-radius: 10px; }
        QFrame#gameFlowRecentColumn { background: @raised@; border: 1px solid @border@; border-radius: 7px; }
        QLabel#gameFlowRecentPlayer, QToolButton#gameFlowRecentPlayer { color: @text@; font-size: 10px; font-weight: 700; padding: 0; }
        QToolButton#gameFlowRecentPlayer:hover { color: @accent@; text-decoration: underline; }
        QFrame#gameFlowRecentMatchPending { background: @field@; border: 1px solid @border@; border-radius: 4px; }
        QFrame#gameFlowRecentMatchWin { background: @matchWinBackground@; border: 1px solid @matchWinBorder@; border-radius: 4px; }
        QFrame#gameFlowRecentMatchLoss { background: @matchLossBackground@; border: 1px solid @matchLossBorder@; border-radius: 4px; }
        QLabel#gameFlowRecentMatchText { color: @muted@; font-size: 10px; }
        QLabel#gameFlowRecentMatchMeta { color: @muted@; font-size: 8px; }
        QLabel#gameFlowTeamTitle { color: @text@; font-size: 14px; font-weight: 700; }
        QFrame#gameFlowPlayerRowOwn, QFrame#gameFlowPlayerRowEnemy { background: @raised@; border: 1px solid @border@; border-radius: 8px; }
        QFrame#gameFlowPlayerRowOwn { border-left: 3px solid @accent@; }
        QFrame#gameFlowPlayerRowEnemy { border-left: 3px solid @danger@; }
        QLabel#gameFlowPortrait { background: @field@; border: 1px solid @border@; border-radius: 7px; color: @muted@; font-size: 10px; }
        QLabel#gameFlowPlayerName, QToolButton#gameFlowPlayerName { color: @text@; font-size: 12px; font-weight: 700; padding: 0; text-align: left; }
        QToolButton#gameFlowPlayerName:hover { color: @accent@; text-decoration: underline; }
        QLabel#gameFlowPlayerStatus, QLabel#gameFlowBanTitle { color: @muted@; font-size: 10px; }
        QWidget#gameFlowRankColumn { background: transparent; }
        QLabel#gameFlowRank { color: @muted@; font-size: 9px; }
        QLabel#gameFlowPlayerKda { color: @text@; font-size: 9px; font-weight: 700; }
        QWidget#gameFlowSpellIcons { background: transparent; }
        QLabel#gameFlowSpellIcon { background: @field@; border: 1px solid @border@; border-radius: 4px; color: @muted@; font-size: 8px; }
        QLabel#gameFlowBanTitle { font-weight: 700; padding-top: 3px; }
    )");

    const QHash<QString, QString> replacements = {
        {"@window@", colors.window}, {"@surface@", colors.surface}, {"@raised@", colors.raised},
        {"@field@", colors.field}, {"@text@", colors.text}, {"@muted@", colors.muted},
        {"@border@", colors.border}, {"@matchWinBackground@", colors.matchWinBackground}, {"@matchLossBackground@", colors.matchLossBackground},
        {"@matchWinBorder@", colors.matchWinBorder}, {"@matchLossBorder@", colors.matchLossBorder}, {"@accent@", colors.accent}, {"@accentSoft@", colors.accentSoft},
        {"@accentHover@", colors.accentHover}, {"@danger@", colors.danger}, {"@dangerSoft@", colors.dangerSoft},
        {"@scrollbar@", colors.scrollbar}
    };
    for (auto it = replacements.cbegin(); it != replacements.cend(); ++it) style.replace(it.key(), it.value());
    return style;
}

ThemeController::Mode readMode()
{
    QSettings settings;
    const QString value = settings.value("appearance/theme", "light").toString();
    if (value == "dark") return ThemeController::Mode::Dark;
    if (value == "system") return ThemeController::Mode::System;
    return ThemeController::Mode::Light;
}

QString modeValue(const ThemeController::Mode mode)
{
    switch (mode) {
    case ThemeController::Mode::Light: return "light";
    case ThemeController::Mode::Dark: return "dark";
    case ThemeController::Mode::System: return "system";
    }
    return "light";
}
} // namespace

ThemeController::ThemeController(QObject *parent) : QObject(parent), mode_(readMode())
{
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (mode_ == Mode::System) apply();
    });
    apply();
}

void ThemeController::setMode(const Mode mode)
{
    if (mode_ == mode) return;
    mode_ = mode;
    QSettings settings;
    settings.setValue("appearance/theme", modeValue(mode_));
    apply();
    emit modeChanged(mode_);
}

bool ThemeController::useDarkPalette() const
{
    if (mode_ == Mode::Dark) return true;
    if (mode_ != Mode::System) return false;
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

void ThemeController::apply()
{
    const ThemeColors colors = colorsFor(useDarkPalette());
    QPalette palette = QApplication::palette();
    palette.setColor(QPalette::Window, QColor(colors.window));
    palette.setColor(QPalette::WindowText, QColor(colors.text));
    palette.setColor(QPalette::Base, QColor(colors.surface));
    palette.setColor(QPalette::Text, QColor(colors.text));
    palette.setColor(QPalette::Button, QColor(colors.surface));
    palette.setColor(QPalette::ButtonText, QColor(colors.text));
    palette.setColor(QPalette::Highlight, QColor(colors.accent));
    palette.setColor(QPalette::HighlightedText, QColor(colors.surface));
    QApplication::setPalette(palette);
    qApp->setProperty("jannaAccent", QColor(colors.accent));
    qApp->setProperty("jannaField", QColor(colors.field));
    qApp->setProperty("jannaIcon", QColor(colors.muted));
    qApp->setProperty("jannaMuted", QColor(colors.muted));
    qApp->setProperty("jannaBorder", QColor(colors.border));
    qApp->setProperty("jannaSurface", QColor(colors.surface));
    qApp->setProperty("jannaText", QColor(colors.text));
    qApp->setProperty("jannaOpgg", QColor(colors.opgg));
    qApp->setStyleSheet(styleSheet(colors));
}

} // namespace Janna
