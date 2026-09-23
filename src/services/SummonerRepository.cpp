#include "services/SummonerRepository.h"

#include "services/LcuClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QUrl>

namespace Janna {
namespace {

int integerValue(const QJsonValue &value)
{
    if (value.isDouble()) return value.toInt();
    bool valid = false;
    const int result = value.toString().toInt(&valid);
    return valid ? result : 0;
}

SummonerProfile profileFromDocument(const QJsonDocument &document, const QString &expectedPuuid)
{
    const QJsonObject object = document.object();
    SummonerProfile profile;
    profile.gameName = object.value("gameName").toString();
    if (profile.gameName.isEmpty()) profile.gameName = object.value("displayName").toString();
    if (profile.gameName.isEmpty()) profile.gameName = object.value("summonerName").toString();
    profile.tagLine = object.value("tagLine").toString();
    profile.puuid = object.value("puuid").toString();
    if (profile.puuid.isEmpty()) profile.puuid = expectedPuuid;
    profile.level = integerValue(object.value("summonerLevel"));
    profile.profileIconId = integerValue(object.value("profileIconId"));
    profile.experienceSinceLevel = integerValue(object.value("xpSinceLastLevel"));
    // LCU returns the required experience for the current level here, not the remainder.
    profile.experienceLevelCap = integerValue(object.value("xpUntilNextLevel"));
    return profile;
}

} // namespace

SummonerRepository::SummonerRepository(LcuClient &client, QObject *parent) : QObject(parent), client_(client) {}

void SummonerRepository::refresh()
{
    beginProfileRequest("/lol-summoner/v1/current-summoner");
}

void SummonerRepository::loadPlayer(const QString &puuid, const QString &summonerId)
{
    const QString encodedPuuid = QString::fromLatin1(QUrl::toPercentEncoding(puuid));
    const QString encodedSummonerId = QString::fromLatin1(QUrl::toPercentEncoding(summonerId));
    const QString puuidPath = encodedPuuid.isEmpty() ? QString()
        : "/lol-summoner/v2/summoners/puuid/" + encodedPuuid;
    const QString summonerPath = encodedSummonerId.isEmpty() ? QString()
        : "/lol-summoner/v1/summoners/" + encodedSummonerId;
    if (puuidPath.isEmpty() && summonerPath.isEmpty()) {
        emit loadingChanged(false, "该玩家没有可用的 LCU 身份标识。");
        return;
    }
    beginProfileRequest(puuidPath.isEmpty() ? summonerPath : puuidPath,
                        puuidPath.isEmpty() ? QString() : summonerPath,
                        puuid);
}

void SummonerRepository::beginProfileRequest(const QString &path, const QString &fallbackPath, const QString &expectedPuuid)
{
    const quint64 generation = ++requestGeneration_;
    loading_ = true;
    emit loadingChanged(true, {});
    requestProfile(path, fallbackPath, expectedPuuid, generation);
}

void SummonerRepository::requestProfile(const QString &path, const QString &fallbackPath, const QString &expectedPuuid, const quint64 generation)
{
    const QPointer<SummonerRepository> repository(this);
    client_.get(path, [repository, fallbackPath, expectedPuuid, generation](QJsonDocument document, QString error) {
        if (!repository || repository->requestGeneration_ != generation) return;
        if (!error.isEmpty() && !fallbackPath.isEmpty()) {
            repository->requestProfile(fallbackPath, {}, expectedPuuid, generation);
            return;
        }
        repository->loading_ = false;
        if (!error.isEmpty()) {
            emit repository->loadingChanged(false, std::move(error));
            return;
        }

        SummonerProfile profile = profileFromDocument(document, expectedPuuid);
        if (!profile.isValid()) {
            emit repository->loadingChanged(false, "LCU 未返回该召唤师数据。");
            return;
        }

        repository->profile_ = std::move(profile);
        emit repository->profileChanged();
        emit repository->loadingChanged(false, {});
    });
}

} // namespace Janna
