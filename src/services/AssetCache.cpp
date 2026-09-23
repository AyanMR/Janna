#include "services/AssetCache.h"

#include "services/CommunityDragonClient.h"
#include "services/LcuClient.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QUrl>

namespace Janna {
namespace {

constexpr int maximumConcurrentDownloads = 4;

bool isLocalAssetPath(const QString &path)
{
    return path.startsWith('/') && !path.contains("..") && !path.contains('\\');
}

bool isCacheableStaticIconPath(const QString &path)
{
    if (!isLocalAssetPath(path) || !path.endsWith(".png", Qt::CaseInsensitive)) return false;

    const QString normalizedPath = QUrl(path).path().toLower();
    return normalizedPath.contains("/items/")
        || normalizedPath.contains("/items2d/")
        || normalizedPath.contains("/perks/")
        || normalizedPath.contains("/perk-images/")
        || normalizedPath.contains("/spells/");
}

int profileIconIdFromPath(const QString &path)
{
    static constexpr auto prefix = "/lol-game-data/assets/v1/profile-icons/";
    const QString normalizedPath = QUrl(path).path();
    if (!isLocalAssetPath(path) || !normalizedPath.startsWith(prefix, Qt::CaseInsensitive)
        || !normalizedPath.endsWith(".jpg", Qt::CaseInsensitive)) {
        return -1;
    }

    QString idText = normalizedPath.mid(static_cast<int>(std::char_traits<char>::length(prefix)));
    idText.chop(4);
    bool validId = false;
    const int id = idText.toInt(&validId);
    return validId && id >= 0 ? id : -1;
}

QString imageExtension(const QString &path)
{
    const QString suffix = QFileInfo(QUrl(path).path()).suffix().toLower();
    if (suffix == "png" || suffix == "jpg" || suffix == "jpeg" || suffix == "webp" || suffix == "svg") return suffix;
    return "bin";
}

QString staticAssetCachePath(const QString &sourcePath)
{
    const QByteArray hash = QCryptographicHash::hash(sourcePath.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(AssetCache::cacheRoot()).filePath("assets/" + QString::fromLatin1(hash) + '.' + imageExtension(sourcePath));
}

QString championCachePath(const int championId)
{
    return QDir(AssetCache::cacheRoot()).filePath("champions/" + QString::number(championId) + ".png");
}

QString profileCachePath(const int profileIconId)
{
    return QDir(AssetCache::cacheRoot()).filePath("profiles/" + QString::number(profileIconId) + ".jpg");
}

QString normalizedRankTier(const QString &tier)
{
    const QString key = tier.trimmed().toLower();
    static const QSet<QString> supportedTiers = {
        "unranked", "iron", "bronze", "silver", "gold", "platinum", "emerald", "diamond", "master", "grandmaster", "challenger"
    };
    return supportedTiers.contains(key) ? key : QString{};
}

QString rankEmblemFileExtension(const QString &tier)
{
    // CommunityDragon publishes the Emerald mini crest as SVG and the other
    // League tiers as PNG. Keeping the source extension also lets QPixmap use
    // the correct image reader from the local cache.
    return tier == "emerald" ? "svg" : "png";
}

QString rankEmblemSourcePath(const QString &tier)
{
    return "/latest/plugins/rcp-fe-lol-static-assets/global/default/images/ranked-mini-crests/"
        + tier + "." + rankEmblemFileExtension(tier);
}

QString rankEmblemCachePath(const QString &tier)
{
    return QDir(AssetCache::cacheRoot()).filePath("ranks/mini-" + tier + "." + rankEmblemFileExtension(tier));
}

} // namespace

AssetCache::AssetCache(LcuClient &client, QObject *parent)
    : QObject(parent), client_(client), communityDragon_(std::make_unique<CommunityDragonClient>()) {}
AssetCache::~AssetCache() = default;

QString AssetCache::cacheRoot()
{
    return QDir(QString::fromUtf8(JANNA_PROJECT_ROOT)).filePath("resources/cache");
}

void AssetCache::cacheChampionPortraits(const QList<Champion> &champions)
{
    for (const Champion &champion : champions) {
        if (champion.id <= 0 || !isLocalAssetPath(champion.squarePortraitPath)) continue;
        queueDownload(champion.squarePortraitPath, championCachePath(champion.id), champion.id);
    }
}

void AssetCache::ensureChampionPortrait(const int championId)
{
    if (championId <= 0) return;
    const QString destination = championCachePath(championId);
    if (QFileInfo::exists(destination)) return;
    queueCommunityDragonDownload(
        "/latest/plugins/rcp-be-lol-game-data/global/default/v1/champion-icons/"
            + QString::number(championId) + ".png",
        destination, championId);
}

void AssetCache::warmStaticAssets()
{
    if (staticAssetsWarmed_) return;
    staticAssetsWarmed_ = true;

    for (const QString &tier : {"unranked", "iron", "bronze", "silver", "gold", "platinum", "emerald", "diamond", "master", "grandmaster", "challenger"}) {
        cacheRankEmblem(tier);
    }

    const QPointer<AssetCache> cache(this);
    client_.get("/lol-game-data/assets/v1/items.json", [cache](QJsonDocument document, QString error) {
        if (!cache) return;
        if (!error.isEmpty()) {
            cache->staticAssetsWarmed_ = false;
            return;
        }
        cache->cacheIconPaths(document, cache->itemAssets_);
        emit cache->staticIconsAvailable();
    });
    client_.get("/lol-game-data/assets/v1/summoner-spells.json", [cache](QJsonDocument document, QString error) {
        if (!cache) return;
        if (!error.isEmpty()) {
            cache->staticAssetsWarmed_ = false;
            return;
        }
        cache->cacheIconPaths(document, cache->summonerSpellAssets_);
        emit cache->staticIconsAvailable();
    });
    client_.get("/lol-game-data/assets/v1/perks.json", [cache](QJsonDocument document, QString error) {
        if (!cache) return;
        if (!error.isEmpty()) {
            cache->staticAssetsWarmed_ = false;
            return;
        }
        cache->cacheIconPaths(document, cache->runeAssets_);
        emit cache->staticIconsAvailable();
    });
    client_.get("/lol-perks/v1/styles", [cache](QJsonDocument document, QString error) {
        if (!cache) return;
        if (!error.isEmpty()) {
            cache->staticAssetsWarmed_ = false;
            return;
        }
        cache->cacheIconPaths(document, cache->runeAssets_);
        emit cache->staticIconsAvailable();
    });

    // The LCU endpoints above are unavailable until League Client has fully
    // started.  Fetch the public CommunityDragon catalogs in parallel so a
    // first launch can still populate rune, shard, item and spell icons for
    // the OPGG panel.  The catalog paths use the same asset paths as LCU,
    // but downloads are routed through the public client below.
    const auto fetchCommunityCatalog = [cache, this](const QString &path,
                                                       QHash<int, StaticAsset> *assets) {
        communityDragon_->fetchStaticAsset(path, [cache, assets](QByteArray bytes, QString error) {
            if (!cache || !error.isEmpty()) return;
            const QJsonDocument document = QJsonDocument::fromJson(bytes);
            if (document.isNull()) return;
            cache->cacheIconPaths(document, *assets, true);
            emit cache->staticIconsAvailable();
        });
    };
    fetchCommunityCatalog("/latest/plugins/rcp-be-lol-game-data/global/default/v1/perks.json", &runeAssets_);
    fetchCommunityCatalog("/latest/plugins/rcp-be-lol-game-data/global/default/v1/summoner-spells.json", &summonerSpellAssets_);
    fetchCommunityCatalog("/latest/plugins/rcp-be-lol-game-data/global/default/v1/items.json", &itemAssets_);
    communityDragon_->fetchProfileIconCatalog([cache](QByteArray bytes, QString error) {
        if (!cache || !error.isEmpty()) return;
        const QJsonDocument document = QJsonDocument::fromJson(bytes);
        if (document.isArray()) cache->cacheProfileIconCatalog(document);
    });
    client_.get("/lol-summoner/v1/current-summoner", [cache](QJsonDocument document, QString error) {
        if (!cache || !error.isEmpty()) return;
        cache->cacheCurrentProfileIcon(document);
    });
}

QPixmap AssetCache::championPortrait(const int championId) const
{
    const auto cached = championPortraits_.constFind(championId);
    if (cached != championPortraits_.cend()) return cached.value();

    QPixmap portrait(championCachePath(championId));
    if (!portrait.isNull()) championPortraits_.insert(championId, portrait);
    return portrait;
}

QPixmap AssetCache::profileIcon(const int profileIconId) const
{
    const auto cached = profileIcons_.constFind(profileIconId);
    if (cached != profileIcons_.cend()) return cached.value();

    QPixmap icon(profileCachePath(profileIconId));
    if (!icon.isNull()) profileIcons_.insert(profileIconId, icon);
    return icon;
}

QPixmap AssetCache::itemIcon(const int itemId) const { return staticIcon(itemAssets_, itemId); }
QPixmap AssetCache::summonerSpellIcon(const int spellId) const { return staticIcon(summonerSpellAssets_, spellId); }
QPixmap AssetCache::runeIcon(const int runeId) const { return staticIcon(runeAssets_, runeId); }
QPixmap AssetCache::rankEmblem(const QString &tier) const
{
    const QString normalizedTier = normalizedRankTier(tier);
    if (normalizedTier.isEmpty()) return {};
    const QString sourcePath = rankEmblemSourcePath(normalizedTier);
    const auto cached = staticIcons_.constFind(sourcePath);
    if (cached != staticIcons_.cend()) return cached.value();

    QPixmap icon(rankEmblemCachePath(normalizedTier));
    if (!icon.isNull()) staticIcons_.insert(sourcePath, icon);
    return icon;
}
QString AssetCache::itemName(const int itemId) const { return staticAssetName(itemAssets_, itemId, "装备"); }
QString AssetCache::summonerSpellName(const int spellId) const { return staticAssetName(summonerSpellAssets_, spellId, "召唤师技能"); }
QString AssetCache::runeName(const int runeId) const { return staticAssetName(runeAssets_, runeId, "符文"); }

void AssetCache::cacheRankEmblem(const QString &tier)
{
    const QString normalizedTier = normalizedRankTier(tier);
    if (normalizedTier.isEmpty()) return;
    queueCommunityDragonDownload(rankEmblemSourcePath(normalizedTier), rankEmblemCachePath(normalizedTier));
}

QPixmap AssetCache::staticIcon(const QHash<int, StaticAsset> &assetsById, const int id) const
{
    const QString sourcePath = assetsById.value(id).sourcePath;
    if (sourcePath.isEmpty()) return {};

    const auto cached = staticIcons_.constFind(sourcePath);
    if (cached != staticIcons_.cend()) return cached.value();

    QPixmap icon(staticAssetCachePath(sourcePath));
    if (!icon.isNull()) staticIcons_.insert(sourcePath, icon);
    return icon;
}

QString AssetCache::staticAssetName(const QHash<int, StaticAsset> &assetsById, const int id, const QString &fallback) const
{
    const QString name = assetsById.value(id).name;
    return name.isEmpty() ? fallback + " " + QString::number(id) : name;
}

void AssetCache::queueDownload(const QString &sourcePath, const QString &destinationPath, const int championId, const int profileIconId)
{
    if (!isLocalAssetPath(sourcePath) || queuedSourcePaths_.contains(sourcePath) || QFileInfo::exists(destinationPath)) return;
    if (!QDir().mkpath(QFileInfo(destinationPath).absolutePath())) return;

    queuedSourcePaths_.insert(sourcePath);
    pendingDownloads_.enqueue({sourcePath, destinationPath, championId, profileIconId, false});
    startDownloads();
}

void AssetCache::queueCommunityDragonDownload(const QString &sourcePath, const QString &destinationPath, const int championId)
{
    if (!isLocalAssetPath(sourcePath) || queuedSourcePaths_.contains(sourcePath) || QFileInfo::exists(destinationPath)) return;
    if (!QDir().mkpath(QFileInfo(destinationPath).absolutePath())) return;

    queuedSourcePaths_.insert(sourcePath);
    pendingDownloads_.enqueue({sourcePath, destinationPath, championId, -1, true});
    startDownloads();
}

void AssetCache::finishDownload(Download download, QByteArray bytes, QString error)
{
    --activeDownloads_;
    bool saved = false;
    if (error.isEmpty() && !bytes.isEmpty() && !QFileInfo::exists(download.destinationPath)) {
        QFile file(download.destinationPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            saved = file.write(bytes) == bytes.size();
            file.close();
        }
    }
    if (saved && download.championId > 0) {
        championPortraits_.remove(download.championId);
        emit championPortraitAvailable(download.championId);
    }
    if (saved && download.profileIconId >= 0) {
        profileIcons_.remove(download.profileIconId);
        emit profileIconAvailable(download.profileIconId);
    }
    if (saved && download.championId == 0 && download.profileIconId < 0) {
        staticIcons_.remove(download.sourcePath);
        emit staticIconsAvailable();
    }
    startDownloads();
}

void AssetCache::startDownloads()
{
    while (activeDownloads_ < maximumConcurrentDownloads && !pendingDownloads_.isEmpty()) {
        const Download download = pendingDownloads_.dequeue();
        ++activeDownloads_;
        const QPointer<AssetCache> cache(this);
        const auto completed = [cache, download](QByteArray bytes, QString error) {
            if (!cache) return;
            cache->finishDownload(download, std::move(bytes), std::move(error));
        };
        if (download.fromCommunityDragon) communityDragon_->fetchStaticAsset(download.sourcePath, completed);
        else client_.getBytes(download.sourcePath, completed);
    }
}

void AssetCache::cacheIconPaths(const QJsonDocument &document, QHash<int, StaticAsset> &assetsById,
                                const bool fromCommunityDragon)
{
    QSet<QString> iconPaths;
    const auto collect = [&](auto &&self, const QJsonValue &value) -> void {
        if (value.isArray()) {
            for (const QJsonValue &entry : value.toArray()) self(self, entry);
            return;
        }
        if (!value.isObject()) return;

        const QJsonObject object = value.toObject();
        const QString iconPath = object.value("iconPath").toString();
        if (isCacheableStaticIconPath(iconPath)) {
            iconPaths.insert(iconPath);
            bool validId = false;
            const int id = object.value("id").toVariant().toInt(&validId);
            if (validId && id > 0) {
                QString name = object.value("name").toString();
                if (name.isEmpty()) name = object.value("displayName").toString();
                if (name.isEmpty()) name = object.value("title").toString();
                const StaticAsset existing = assetsById.value(id);
                assetsById.insert(id, {iconPath, name.isEmpty() ? existing.name : name});
            }
        }
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) self(self, it.value());
    };
    if (document.isArray()) collect(collect, document.array());
    if (document.isObject()) collect(collect, document.object());
    for (const QString &path : iconPaths) {
        if (fromCommunityDragon) queueCommunityDragonDownload(path, staticAssetCachePath(path));
        else queueDownload(path, staticAssetCachePath(path));
    }
}

void AssetCache::cacheProfileIconCatalog(const QJsonDocument &document)
{
    for (const QJsonValue &value : document.array()) {
        const int profileIconId = profileIconIdFromPath(value.toObject().value("iconPath").toString());
        if (profileIconId < 0) continue;
        const QString sourcePath = "/lol-game-data/assets/v1/profile-icons/" + QString::number(profileIconId) + ".jpg";
        queueDownload(sourcePath, profileCachePath(profileIconId), 0, profileIconId);
    }
}

void AssetCache::cacheCurrentProfileIcon(const QJsonDocument &document)
{
    const QJsonValue profileIconValue = document.object().value("profileIconId");
    bool validId = false;
    const int profileIconId = profileIconValue.toVariant().toInt(&validId);
    if (!validId || profileIconId < 0) return;
    queueDownload("/lol-game-data/assets/v1/profile-icons/" + QString::number(profileIconId) + ".jpg", profileCachePath(profileIconId), 0, profileIconId);
}

} // namespace Janna
