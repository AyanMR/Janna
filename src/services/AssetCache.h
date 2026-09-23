#pragma once

#include "model/Champion.h"

#include <QByteArray>
#include <QHash>
#include <QJsonDocument>
#include <QObject>
#include <QPixmap>
#include <QQueue>
#include <QSet>
#include <QString>

#include <memory>

namespace Janna {

class CommunityDragonClient;
class LcuClient;

class AssetCache : public QObject {
    Q_OBJECT

public:
    explicit AssetCache(LcuClient &client, QObject *parent = nullptr);
    ~AssetCache() override;

    void cacheChampionPortraits(const QList<Champion> &champions);
    // Queues the public CommunityDragon icon when the local LCU asset is
    // missing.  The request is intentionally lazy so an offline client does
    // not block startup while still allowing OPGG-only data to show portraits.
    void ensureChampionPortrait(int championId);
    void warmStaticAssets();
    QPixmap championPortrait(int championId) const;
    QPixmap profileIcon(int profileIconId) const;
    QPixmap itemIcon(int itemId) const;
    QPixmap summonerSpellIcon(int spellId) const;
    QPixmap runeIcon(int runeId) const;
    QPixmap rankEmblem(const QString &tier) const;
    QString itemName(int itemId) const;
    QString summonerSpellName(int spellId) const;
    QString runeName(int runeId) const;
    void cacheRankEmblem(const QString &tier);

    static QString cacheRoot();

signals:
    void championPortraitAvailable(int championId);
    void profileIconAvailable(int profileIconId);
    void staticIconsAvailable();

private:
    struct StaticAsset {
        QString sourcePath;
        QString name;
    };

    struct Download {
        QString sourcePath;
        QString destinationPath;
        int championId{};
        int profileIconId{-1};
        bool fromCommunityDragon{};
    };

    void queueDownload(const QString &sourcePath, const QString &destinationPath, int championId = 0, int profileIconId = -1);
    void queueCommunityDragonDownload(const QString &sourcePath, const QString &destinationPath, int championId = 0);
    void finishDownload(Download download, QByteArray bytes, QString error);
    void startDownloads();
    void cacheIconPaths(const QJsonDocument &document, QHash<int, StaticAsset> &assetsById,
                        bool fromCommunityDragon = false);
    void cacheProfileIconCatalog(const QJsonDocument &document);
    void cacheCurrentProfileIcon(const QJsonDocument &document);
    QPixmap staticIcon(const QHash<int, StaticAsset> &assetsById, int id) const;
    QString staticAssetName(const QHash<int, StaticAsset> &assetsById, int id, const QString &fallback) const;

    LcuClient &client_;
    std::unique_ptr<CommunityDragonClient> communityDragon_;
    QQueue<Download> pendingDownloads_;
    QSet<QString> queuedSourcePaths_;
    mutable QHash<int, QPixmap> championPortraits_;
    mutable QHash<int, QPixmap> profileIcons_;
    mutable QHash<QString, QPixmap> staticIcons_;
    QHash<int, StaticAsset> itemAssets_;
    QHash<int, StaticAsset> summonerSpellAssets_;
    QHash<int, StaticAsset> runeAssets_;
    int activeDownloads_{};
    bool staticAssetsWarmed_{};
};

} // namespace Janna
