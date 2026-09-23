#pragma once

#include <QObject>
#include <QString>

namespace Janna {

class LcuClient;

struct SummonerProfile {
    QString gameName;
    QString tagLine;
    QString puuid;
    int level{};
    int profileIconId{};
    int experienceSinceLevel{};
    int experienceLevelCap{};

    [[nodiscard]] bool isValid() const { return !gameName.isEmpty() && !puuid.isEmpty(); }
    [[nodiscard]] QString riotId() const { return tagLine.isEmpty() ? gameName : gameName + "#" + tagLine; }
};

class SummonerRepository : public QObject {
    Q_OBJECT

public:
    explicit SummonerRepository(LcuClient &client, QObject *parent = nullptr);

    [[nodiscard]] const SummonerProfile &profile() const { return profile_; }

public slots:
    void refresh();
    void loadPlayer(const QString &puuid, const QString &summonerId = {});

signals:
    void profileChanged();
    void loadingChanged(bool loading, const QString &message);

private:
    void beginProfileRequest(const QString &path, const QString &fallbackPath = {}, const QString &expectedPuuid = {});
    void requestProfile(const QString &path, const QString &fallbackPath, const QString &expectedPuuid, quint64 generation);

    LcuClient &client_;
    SummonerProfile profile_;
    bool loading_{};
    quint64 requestGeneration_{};
};

} // namespace Janna
