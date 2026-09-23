#include "services/ChampionRepository.h"
#include "ui/ChampionPicker.h"

#include <QApplication>
#include <QComboBox>
#include <QPushButton>

#include <iostream>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);

    Janna::ChampionRepository repository;
    Janna::ChampionPicker picker(repository);
    picker.setSelectedChampionIds({238, 103}); // Zed + Ahri
    QCoreApplication::processEvents();

    if (picker.selectedChampionIds() != QList<int>{238, 103}) {
        std::cerr << "multi-select state was not retained\n";
        return 1;
    }

    auto *typeFilter = picker.findChild<QComboBox *>(QStringLiteral("championTypeFilter"));
    if (!typeFilter || typeFilter->findData(QStringLiteral("AD刺客")) < 0
        || typeFilter->findData(QStringLiteral("AP输出")) < 0
        || typeFilter->findData(QStringLiteral("AP法师")) >= 0) {
        std::cerr << "fallback champion class options missing\n";
        return 2;
    }

    int selectedTiles = 0;
    for (QPushButton *tile : picker.findChildren<QPushButton *>(QStringLiteral("championTile"))) {
        if (tile->property("championSelected").toBool()) ++selectedTiles;
    }
    if (selectedTiles != 2) {
        std::cerr << "selected tile state missing\n";
        return 3;
    }


    QPushButton *garenTile = nullptr;
    for (QPushButton *tile : picker.findChildren<QPushButton *>(QStringLiteral("championTile"))) {
        if (tile->toolTip().contains(QStringLiteral("德玛西亚之力"))) {
            garenTile = tile;
            break;
        }
    }
    if (!garenTile) {
        std::cerr << "champion tile lookup failed\n";
        return 4;
    }
    garenTile->click();
    QCoreApplication::processEvents();
    if (picker.selectedChampionIds().size() != 3) {
        std::cerr << "third champion was not added\n";
        return 5;
    }

    garenTile = nullptr;
    for (QPushButton *tile : picker.findChildren<QPushButton *>(QStringLiteral("championTile"))) {
        if (tile->toolTip().contains(QStringLiteral("德玛西亚之力"))) {
            garenTile = tile;
            break;
        }
    }
    if (!garenTile) return 6;
    garenTile->click();
    QCoreApplication::processEvents();
    if (picker.selectedChampionIds().size() != 3) {
        std::cerr << "duplicate champion was added\n";
        return 7;
    }


    std::cout << "champion picker smoke passed\n";
    return 0;
}
