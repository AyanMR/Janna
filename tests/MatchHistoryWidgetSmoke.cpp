#include "services/ChampionRepository.h"
#include "services/GameDataRepository.h"
#include "services/LcuClient.h"
#include "services/MatchRepository.h"
#include "services/SummonerRepository.h"
#include "ui/MatchHistoryWidget.h"

#include <QApplication>
#include <QFrame>
#include <QMouseEvent>
#include <QScrollArea>
#include <QTimer>

#include <algorithm>
#include <iostream>

namespace {

void click(QWidget *widget)
{
    const QPoint localPosition = widget->rect().center();
    const QPoint globalPosition = widget->mapToGlobal(localPosition);
    QMouseEvent press(QEvent::MouseButtonPress, localPosition, globalPosition,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, localPosition, globalPosition,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &release);
}

QFrame *matchRow(Janna::MatchHistoryWidget *history)
{
    const QList<QFrame *> frames = history->findChildren<QFrame *>();
    const auto found = std::find_if(frames.cbegin(), frames.cend(), [](QFrame *frame) {
        return frame->objectName() == "matchRowWin" || frame->objectName() == "matchRowLoss";
    });
    return found == frames.cend() ? nullptr : *found;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);

    Janna::LcuClient client;
    Janna::ChampionRepository champions;
    Janna::GameDataRepository gameData(client);
    Janna::MatchRepository matches(client);
    Janna::SummonerRepository summoner(client);
    auto *history = new Janna::MatchHistoryWidget(champions, matches, gameData);
    QScrollArea scrollArea;
    scrollArea.setWidget(history);
    scrollArea.setWidgetResizable(true);
    scrollArea.resize(1400, 900);
    scrollArea.show();

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &application, [&application] {
        std::cerr << "match-history-widget timed out\n";
        application.exit(2);
    });
    timeout.start(15000);

    qint64 selectedGameId = 0;
    bool clickSent = false;
    bool verificationScheduled = false;
    QObject::connect(&summoner, &Janna::SummonerRepository::profileChanged, &application, [&summoner, &matches] {
        if (summoner.profile().isValid()) matches.refresh(summoner.profile());
    });
    QObject::connect(&summoner, &Janna::SummonerRepository::loadingChanged, &application, [&application](const bool loading, const QString &error) {
        if (!loading && !error.isEmpty()) {
            std::cerr << "summoner-error=" << error.toStdString() << '\n';
            application.exit(1);
        }
    });
    QObject::connect(&matches, &Janna::MatchRepository::loadingChanged, &application, [&application](const bool loading, const QString &error) {
        if (!loading && !error.isEmpty()) {
            std::cerr << "matches-error=" << error.toStdString() << '\n';
            application.exit(1);
        }
    });
    QObject::connect(&matches, &Janna::MatchRepository::matchesChanged, &application,
                     [&application, &matches, history, &selectedGameId, &clickSent] {
                         if (clickSent || matches.matches().isEmpty()) {
                             std::cerr << "match-history-widget has no selectable match\n";
                             application.exit(1);
                             return;
                         }
                         selectedGameId = matches.matches().constFirst().gameId;
                         history->setMatches({matches.matches().constFirst()});
                         QTimer::singleShot(0, history, [&application, history, &clickSent] {
                             QFrame *row = matchRow(history);
                             if (!row) {
                                 std::cerr << "match-history-widget row missing\n";
                                 application.exit(1);
                                 return;
                             }
                             click(row);
                             clickSent = true;
                         });
                     });
    QObject::connect(&matches, &Janna::MatchRepository::matchDetailLoadingChanged, &application,
                     [&application](qint64, const bool loading, const QString &error) {
                         if (!loading && !error.isEmpty()) {
                             std::cerr << "match-detail-error=" << error.toStdString() << '\n';
                             application.exit(1);
                         }
                     });
    QObject::connect(&matches, &Janna::MatchRepository::matchDetailChanged, &application,
                     [&application, history, &selectedGameId, &verificationScheduled](const qint64 gameId) {
                         if (gameId != selectedGameId || verificationScheduled) return;
                         verificationScheduled = true;
                         QTimer::singleShot(350, history, [&application, history] {
                             const QList<QFrame *> details = history->findChildren<QFrame *>("matchDetails");
                             const bool hasFullTeams = !details.isEmpty()
                                 && details.constFirst()->findChild<QFrame *>("matchTeamWin") != nullptr
                                 && details.constFirst()->findChild<QFrame *>("matchTeamLoss") != nullptr;
                             const int height = details.isEmpty() ? 0 : details.constFirst()->maximumHeight();
                             std::cerr << "match-history-widget details=" << details.size()
                                       << " full-teams=" << hasFullTeams
                                       << " maximum-height=" << height << '\n';
                             application.exit(hasFullTeams && height > 150 ? 0 : 1);
                         });
                     });
    summoner.refresh();
    return application.exec();
}
