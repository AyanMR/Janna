#pragma once

#include <QString>
#include <QStringList>

namespace Janna {
struct Champion {
    int id{};
    QString name;
    QString title;
    QString squarePortraitPath;
    QStringList roles;
    // Riot's tactical damage class (for example kPhysical/kMagic/kMixed).
    // It is kept separately from roles because a champion can have several
    // classes and the class is not a lane/position.
    QString damageType;
    // Stable Riot/Data Dragon identifier used to resolve public OPGG records.
    QString key;
};
}
