#pragma once

#include <QString>
#include <QStringList>

namespace Janna {

class GamePathResolver {
public:
    static QString leagueClientPath();
    static QString detectedLeagueClientPath();
    static QString gameDirectory();
    static QString configuredLeagueClientPath();
    static bool usesManualPath();
    static bool setManualLeagueClientPath(const QString &path);
    static void useAutomaticPath();
    static QStringList lockfileCandidates();
};

} // namespace Janna
