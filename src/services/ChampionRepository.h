#pragma once
#include "model/Champion.h"
#include "services/AliasStore.h"
#include <QList>
#include <QObject>
#include <QPixmap>
#include <memory>
namespace Janna {

class AssetCache;
class LcuClient;

class ChampionRepository : public QObject {
    Q_OBJECT

public:
    explicit ChampionRepository(QObject *parent = nullptr);
    ~ChampionRepository() override;

    const QList<Champion> &champions() const { return champions_; }
    QString aliasFor(int id) const { return aliases_.aliasFor(id); }
    [[nodiscard]] QString championNameFor(int id, const QString &fallback = {}) const;
    [[nodiscard]] QString championKeyFor(int id) const;
    // Champion classes (for example AD fighter / AD assassin) are separate
    // from lane positions and may contain more than one value.
    [[nodiscard]] QStringList championTypeLabelsFor(int id) const;
    [[nodiscard]] int resolveChampionId(int id, const QString &nameOrKey = {}) const;
    QPixmap portraitFor(int id) const;
    QPixmap profileIconFor(int profileIconId) const;
    QPixmap itemIconFor(int itemId) const;
    QPixmap summonerSpellIconFor(int spellId) const;
    QPixmap runeIconFor(int runeId) const;
    QPixmap rankEmblemFor(const QString &tier);
    QString itemNameFor(int itemId) const;
    QString summonerSpellNameFor(int spellId) const;
    QString runeNameFor(int runeId) const;
    LcuClient &lcuClient();
    void setAlias(int id, const QString &alias);
    // Starts the first-launch static asset prefetch (runes, shards, items,
    // spells and rank icons) without requiring a champion refresh first.
    void warmStaticAssets();

public slots:
    void refresh();

signals:
    void championsChanged();
    void championPortraitsChanged();
    void profileIconAvailable(int profileIconId);
    void staticIconsChanged();
    void loadingChanged(bool loading, const QString &message);

private:
    void loadOwnedChampionDetails(quint64 generation);
    void loadMissingChampionTitles(quint64 generation);
    void loadNextChampionTitle(quint64 generation);

    std::unique_ptr<LcuClient> client_;
    std::unique_ptr<AssetCache> assets_;
    AliasStore aliases_;
    QList<Champion> champions_;
    QList<int> pendingTitleChampionIds_;
    quint64 refreshGeneration_{};
};

} // namespace Janna
