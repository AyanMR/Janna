#include "services/ChampionRepository.h"
#include "services/AssetCache.h"
#include "services/LcuClient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QHash>
#include <QRegularExpression>
#include <algorithm>

#include "services/ChampionCatalog.h"

namespace Janna {
namespace {

QString normalizedChampionKey(QString value)
{
    value = value.trimmed().toCaseFolded();
    value.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    if (value == QStringLiteral("nunuwillump")) value = QStringLiteral("nunu");
    if (value == QStringLiteral("monkeyking")) value = QStringLiteral("wukong");
    if (value == QStringLiteral("fiddlesticks")) value = QStringLiteral("fiddle");
    return value;
}

QString championTypeLabel(QString value)
{
    const QString original = value.trimmed();
    if (original == QStringLiteral("辅助") || original == QStringLiteral("坦克")
        || original == QStringLiteral("AD战士") || original == QStringLiteral("AP战士")
        || original == QStringLiteral("AD刺客") || original == QStringLiteral("AP刺客")
        || original == QStringLiteral("AD输出") || original == QStringLiteral("AP输出")
        || original == QStringLiteral("其他") || original == QStringLiteral("战士")
        || original == QStringLiteral("刺客") || original == QStringLiteral("法师")
        || original == QStringLiteral("射手")) {
        return original;
    }

    value = original.toUpper();
    value.replace('-', ' ');
    value.replace('_', ' ');
    value = value.simplified();

    // These labels intentionally mirror OP.GG's Chinese Class filter.  The
    // LCU/Riot role names are class tags, not lane positions, so do not use
    // tactical damage metadata to invent variants such as "AP法师".
    if (value == QStringLiteral("CONTROLLER") || value == QStringLiteral("SUPPORT")) {
        return QStringLiteral("辅助");
    }
    if (value == QStringLiteral("TANK") || value == QStringLiteral("VANGUARD")
        || value == QStringLiteral("WARDEN")) {
        return QStringLiteral("坦克");
    }
    if (value == QStringLiteral("FIGHTER") || value == QStringLiteral("DIVER")) {
        return QStringLiteral("AD战士");
    }
    if (value == QStringLiteral("MAGE")) return QStringLiteral("AP输出");
    if (value == QStringLiteral("SLAYER") || value == QStringLiteral("ASSASSIN")) {
        return QStringLiteral("AD刺客");
    }
    if (value == QStringLiteral("MARKSMAN") || value == QStringLiteral("ADC")
        || value == QStringLiteral("AD CARRY")) {
        return QStringLiteral("AD输出");
    }
    if (value == QStringLiteral("OTHER") || value == QStringLiteral("SPECIALIST")) {
        return QStringLiteral("其他");
    }
    return {};
}

QStringList championTypeLabels(const Champion &champion)
{
    QStringList result;
    for (QString role : champion.roles) {
        const QString label = championTypeLabel(role);
        if (!label.isEmpty() && !result.contains(label)) result.append(label);
    }
    return result;
}

QString damageTypeFromObject(const QJsonObject &object)
{
    const QJsonObject tactical = object.value(QStringLiteral("tacticalInfo")).toObject();
    QString value = tactical.value(QStringLiteral("damageType")).toString().trimmed();
    if (value.isEmpty()) value = object.value(QStringLiteral("damageType")).toString().trimmed();
    return value;
}

QStringList fallbackRolesFor(QString key)
{
    // The local catalog is available before League Client connects. These are
    // Data Dragon's stable champion tags and are only used while the LCU has
    // not supplied the richer champion-summary response yet.
    key = key.trimmed().toCaseFolded();
    key.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    static const QHash<QString, QString> rolesByKey = {
        {QStringLiteral("aatrox"), QStringLiteral("FIGHTER")},
        {QStringLiteral("ahri"), QStringLiteral("MAGE,ASSASSIN")},
        {QStringLiteral("akali"), QStringLiteral("ASSASSIN")},
        {QStringLiteral("akshan"), QStringLiteral("MARKSMAN,ASSASSIN")},
        {QStringLiteral("alistar"), QStringLiteral("TANK,SUPPORT")},
        {QStringLiteral("ambessa"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("amumu"), QStringLiteral("TANK,SUPPORT")},
        {QStringLiteral("anivia"), QStringLiteral("MAGE")},
        {QStringLiteral("annie"), QStringLiteral("MAGE,SUPPORT")},
        {QStringLiteral("aphelios"), QStringLiteral("MARKSMAN")},
        {QStringLiteral("ashe"), QStringLiteral("MARKSMAN,SUPPORT")},
        {QStringLiteral("aurelionsol"), QStringLiteral("MAGE")},
        {QStringLiteral("aurora"), QStringLiteral("MAGE,ASSASSIN")},
        {QStringLiteral("azir"), QStringLiteral("MAGE,MARKSMAN")},
        {QStringLiteral("bard"), QStringLiteral("SUPPORT,MAGE")},
        {QStringLiteral("belveth"), QStringLiteral("FIGHTER")},
        {QStringLiteral("blitzcrank"), QStringLiteral("TANK,SUPPORT")},
        {QStringLiteral("brand"), QStringLiteral("MAGE,SUPPORT")},
        {QStringLiteral("braum"), QStringLiteral("TANK,SUPPORT")},
        {QStringLiteral("briar"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("caitlyn"), QStringLiteral("MARKSMAN")},
        {QStringLiteral("camille"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("cassiopeia"), QStringLiteral("MAGE")},
        {QStringLiteral("chogath"), QStringLiteral("TANK,MAGE")},
        {QStringLiteral("corki"), QStringLiteral("MARKSMAN,MAGE")},
        {QStringLiteral("darius"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("diana"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("draven"), QStringLiteral("MARKSMAN")},
        {QStringLiteral("drmundo"), QStringLiteral("TANK,FIGHTER")},
        {QStringLiteral("ekko"), QStringLiteral("ASSASSIN,MAGE")},
        {QStringLiteral("elise"), QStringLiteral("ASSASSIN,MAGE")},
        {QStringLiteral("evelynn"), QStringLiteral("ASSASSIN,MAGE")},
        {QStringLiteral("ezreal"), QStringLiteral("MARKSMAN,MAGE")},
        {QStringLiteral("fiddlesticks"), QStringLiteral("MAGE,SUPPORT")},
        {QStringLiteral("fiora"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("fizz"), QStringLiteral("ASSASSIN,FIGHTER")},
        {QStringLiteral("galio"), QStringLiteral("TANK,MAGE")},
        {QStringLiteral("gangplank"), QStringLiteral("FIGHTER")},
        {QStringLiteral("garen"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("gnar"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("gragas"), QStringLiteral("FIGHTER,MAGE")},
        {QStringLiteral("graves"), QStringLiteral("MARKSMAN")},
        {QStringLiteral("gwen"), QStringLiteral("FIGHTER")},
        {QStringLiteral("hecarim"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("heimerdinger"), QStringLiteral("MAGE,SUPPORT")},
        {QStringLiteral("hwei"), QStringLiteral("MAGE,SUPPORT")},
        {QStringLiteral("illaoi"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("irelia"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("ivern"), QStringLiteral("SUPPORT,MAGE")},
        {QStringLiteral("janna"), QStringLiteral("SUPPORT,MAGE")},
        {QStringLiteral("jarvaniv"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("jax"), QStringLiteral("FIGHTER")},
        {QStringLiteral("jayce"), QStringLiteral("FIGHTER,MARKSMAN")},
        {QStringLiteral("jhin"), QStringLiteral("MARKSMAN,MAGE")},
        {QStringLiteral("jinx"), QStringLiteral("MARKSMAN")},
        {QStringLiteral("kaisa"), QStringLiteral("MARKSMAN,MAGE")},
        {QStringLiteral("kalista"), QStringLiteral("MARKSMAN")},
        {QStringLiteral("karma"), QStringLiteral("MAGE,SUPPORT")},
        {QStringLiteral("karthus"), QStringLiteral("MAGE")},
        {QStringLiteral("kassadin"), QStringLiteral("ASSASSIN,MAGE")},
        {QStringLiteral("katarina"), QStringLiteral("ASSASSIN,MAGE")},
        {QStringLiteral("kayle"), QStringLiteral("MARKSMAN,MAGE")},
        {QStringLiteral("kayn"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("kennen"), QStringLiteral("MAGE")},
        {QStringLiteral("khazix"), QStringLiteral("ASSASSIN")},
        {QStringLiteral("kindred"), QStringLiteral("MARKSMAN")},
        {QStringLiteral("kled"), QStringLiteral("FIGHTER")},
        {QStringLiteral("kogmaw"), QStringLiteral("MARKSMAN,MAGE")},
        {QStringLiteral("ksante"), QStringLiteral("TANK,FIGHTER")},
        {QStringLiteral("leblanc"), QStringLiteral("ASSASSIN,MAGE")},
        {QStringLiteral("leesin"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("leona"), QStringLiteral("TANK,SUPPORT")},
        {QStringLiteral("lillia"), QStringLiteral("FIGHTER,MAGE")},
        {QStringLiteral("lissandra"), QStringLiteral("MAGE")},
        {QStringLiteral("locke"), QStringLiteral("ASSASSIN,MAGE")},
        {QStringLiteral("lucian"), QStringLiteral("MARKSMAN,ASSASSIN")},
        {QStringLiteral("lulu"), QStringLiteral("SUPPORT,MAGE")},
        {QStringLiteral("lux"), QStringLiteral("MAGE,SUPPORT")},
        {QStringLiteral("malphite"), QStringLiteral("TANK,MAGE")},
        {QStringLiteral("malzahar"), QStringLiteral("MAGE")},
        {QStringLiteral("maokai"), QStringLiteral("TANK,SUPPORT")},
        {QStringLiteral("masteryi"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("mel"), QStringLiteral("MAGE,SUPPORT")},
        {QStringLiteral("milio"), QStringLiteral("SUPPORT,MAGE")},
        {QStringLiteral("missfortune"), QStringLiteral("MARKSMAN,MAGE")},
        {QStringLiteral("monkeyking"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("mordekaiser"), QStringLiteral("FIGHTER,MAGE")},
        {QStringLiteral("morgana"), QStringLiteral("SUPPORT,MAGE")},
        {QStringLiteral("naafiri"), QStringLiteral("ASSASSIN,FIGHTER")},
        {QStringLiteral("nami"), QStringLiteral("SUPPORT,MAGE")},
        {QStringLiteral("nasus"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("nautilus"), QStringLiteral("TANK,SUPPORT")},
        {QStringLiteral("neeko"), QStringLiteral("MAGE,SUPPORT")},
        {QStringLiteral("nidalee"), QStringLiteral("ASSASSIN,MAGE")},
        {QStringLiteral("nilah"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("nocturne"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("nunu"), QStringLiteral("TANK,MAGE")},
        {QStringLiteral("olaf"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("orianna"), QStringLiteral("MAGE,SUPPORT")},
        {QStringLiteral("ornn"), QStringLiteral("TANK")},
        {QStringLiteral("pantheon"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("poppy"), QStringLiteral("TANK,FIGHTER")},
        {QStringLiteral("pyke"), QStringLiteral("SUPPORT,ASSASSIN")},
        {QStringLiteral("qiyana"), QStringLiteral("ASSASSIN")},
        {QStringLiteral("quinn"), QStringLiteral("MARKSMAN,ASSASSIN")},
        {QStringLiteral("rakan"), QStringLiteral("SUPPORT")},
        {QStringLiteral("rammus"), QStringLiteral("TANK")},
        {QStringLiteral("reksai"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("rell"), QStringLiteral("TANK,SUPPORT")},
        {QStringLiteral("renata"), QStringLiteral("SUPPORT,MAGE")},
        {QStringLiteral("renekton"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("rengar"), QStringLiteral("ASSASSIN,FIGHTER")},
        {QStringLiteral("riven"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("rumble"), QStringLiteral("FIGHTER,MAGE")},
        {QStringLiteral("ryze"), QStringLiteral("MAGE")},
        {QStringLiteral("samira"), QStringLiteral("MARKSMAN,ASSASSIN")},
        {QStringLiteral("sejuani"), QStringLiteral("TANK")},
        {QStringLiteral("senna"), QStringLiteral("SUPPORT,MARKSMAN")},
        {QStringLiteral("seraphine"), QStringLiteral("SUPPORT,MAGE")},
        {QStringLiteral("sett"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("shaco"), QStringLiteral("ASSASSIN")},
        {QStringLiteral("shen"), QStringLiteral("TANK")},
        {QStringLiteral("shyvana"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("singed"), QStringLiteral("TANK,MAGE")},
        {QStringLiteral("sion"), QStringLiteral("TANK,FIGHTER")},
        {QStringLiteral("sivir"), QStringLiteral("MARKSMAN")},
        {QStringLiteral("skarner"), QStringLiteral("TANK,FIGHTER")},
        {QStringLiteral("smolder"), QStringLiteral("MARKSMAN,MAGE")},
        {QStringLiteral("sona"), QStringLiteral("SUPPORT,MAGE")},
        {QStringLiteral("soraka"), QStringLiteral("SUPPORT,MAGE")},
        {QStringLiteral("swain"), QStringLiteral("MAGE,SUPPORT")},
        {QStringLiteral("sylas"), QStringLiteral("MAGE,ASSASSIN")},
        {QStringLiteral("syndra"), QStringLiteral("MAGE")},
        {QStringLiteral("tahmkench"), QStringLiteral("TANK,SUPPORT")},
        {QStringLiteral("taliyah"), QStringLiteral("MAGE,SUPPORT")},
        {QStringLiteral("talon"), QStringLiteral("ASSASSIN")},
        {QStringLiteral("taric"), QStringLiteral("SUPPORT,TANK")},
        {QStringLiteral("teemo"), QStringLiteral("MARKSMAN,MAGE")},
        {QStringLiteral("thresh"), QStringLiteral("SUPPORT,TANK")},
        {QStringLiteral("tristana"), QStringLiteral("MARKSMAN,ASSASSIN")},
        {QStringLiteral("trundle"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("tryndamere"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("twistedfate"), QStringLiteral("MAGE,MARKSMAN")},
        {QStringLiteral("twitch"), QStringLiteral("MARKSMAN,ASSASSIN")},
        {QStringLiteral("udyr"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("urgot"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("varus"), QStringLiteral("MARKSMAN,MAGE")},
        {QStringLiteral("vayne"), QStringLiteral("MARKSMAN,ASSASSIN")},
        {QStringLiteral("veigar"), QStringLiteral("MAGE")},
        {QStringLiteral("velkoz"), QStringLiteral("MAGE,SUPPORT")},
        {QStringLiteral("vex"), QStringLiteral("MAGE")},
        {QStringLiteral("vi"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("viego"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("viktor"), QStringLiteral("MAGE")},
        {QStringLiteral("vladimir"), QStringLiteral("MAGE,FIGHTER")},
        {QStringLiteral("volibear"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("warwick"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("xayah"), QStringLiteral("MARKSMAN")},
        {QStringLiteral("xerath"), QStringLiteral("MAGE,SUPPORT")},
        {QStringLiteral("xinzhao"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("yasuo"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("yone"), QStringLiteral("FIGHTER,ASSASSIN")},
        {QStringLiteral("yorick"), QStringLiteral("FIGHTER,TANK")},
        {QStringLiteral("yunara"), QStringLiteral("MARKSMAN")},
        {QStringLiteral("yuumi"), QStringLiteral("SUPPORT,MAGE")},
        {QStringLiteral("zaahen"), QStringLiteral("FIGHTER")},
        {QStringLiteral("zac"), QStringLiteral("TANK,FIGHTER")},
        {QStringLiteral("zed"), QStringLiteral("ASSASSIN")},
        {QStringLiteral("zeri"), QStringLiteral("MARKSMAN")},
        {QStringLiteral("ziggs"), QStringLiteral("MAGE")},
        {QStringLiteral("zilean"), QStringLiteral("SUPPORT,MAGE")},
        {QStringLiteral("zoe"), QStringLiteral("MAGE")},
        {QStringLiteral("zyra"), QStringLiteral("MAGE,SUPPORT")}
    };
    const QString roles = rolesByKey.value(key);
    if (roles.isEmpty()) return {};
    return roles.split(',', Qt::SkipEmptyParts);
}

const ChampionCatalogEntry *catalogFor(const int id, const QString &key = {})
{
    const QString normalizedKey = normalizedChampionKey(key);
    for (const ChampionCatalogEntry &entry : championCatalog()) {
        if ((id > 0 && entry.id == id)
            || (!normalizedKey.isEmpty() && normalizedChampionKey(QString::fromLatin1(entry.key)) == normalizedKey)) {
            return &entry;
        }
    }
    return nullptr;
}

void localizeChampion(Champion &champion)
{
    const ChampionCatalogEntry *catalog = catalogFor(champion.id, champion.key);
    if (!catalog) return;
    champion.key = QString::fromLatin1(catalog->key);
    champion.name = QString::fromUtf8(catalog->name);
}

void sortChampions(QList<Champion> &champions);

QList<Champion> builtInChampions()
{
    QList<Champion> result;
    result.reserve(championCatalog().size());
    for (const ChampionCatalogEntry &entry : championCatalog()) {
        result.append({entry.id, QString::fromUtf8(entry.name), {},
                       QStringLiteral("/lol-game-data/assets/v1/champion-icons/") + QString::number(entry.id) + ".png",
                       {}, {}, QString::fromLatin1(entry.key)});
    }
    sortChampions(result);
    return result;
}

QStringList rolesFromValue(const QJsonValue &value)
{
    QStringList roles;
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) {
            const QString role = entry.toString().trimmed();
            if (!role.isEmpty() && !roles.contains(role, Qt::CaseInsensitive)) roles.append(role);
        }
    } else if (value.isString()) {
        const QStringList values = value.toString().split(QRegularExpression(QStringLiteral("[,/|;]")), Qt::SkipEmptyParts);
        for (const QString &entry : values) {
            const QString role = entry.trimmed();
            if (!role.isEmpty() && !roles.contains(role, Qt::CaseInsensitive)) roles.append(role);
        }
    }
    return roles;
}

QList<Champion> championsFromDocument(const QJsonDocument &document)
{
    QJsonArray entries;
    if (document.isArray()) entries = document.array();
    if (document.isObject()) {
        const QJsonObject root = document.object();
        entries = root.value("champions").toArray();
        if (entries.isEmpty()) entries = root.value("data").toArray();
    }

    QList<Champion> champions;
    champions.reserve(entries.size());
    for (const QJsonValue &value : entries) {
        const QJsonObject object = value.toObject();
        const int id = object.value("id").toInt();
        if (id >= 60000 || id > 10000) continue;
        QString name = object.value("name").toString();
        if (id <= 0 || name.isEmpty()) continue;
        QString key = object.value("alias").toString().trimmed();
        if (key.isEmpty()) key = object.value("key").toString().trimmed();
        if (key.isEmpty()) key = object.value("championKey").toString().trimmed();
        Champion champion{id, name, object.value("title").toString(), object.value("squarePortraitPath").toString(),
                          rolesFromValue(object.value("roles")), damageTypeFromObject(object), key};
        localizeChampion(champion);
        champions.append(std::move(champion));
    }
    return champions;
}

void sortChampions(QList<Champion> &champions)
{
    std::sort(champions.begin(), champions.end(), [](const Champion &left, const Champion &right) {
        return left.name.localeAwareCompare(right.name) < 0;
    });
}

} // namespace

ChampionRepository::ChampionRepository(QObject *parent)
    : QObject(parent), client_(std::make_unique<LcuClient>()), assets_(std::make_unique<AssetCache>(*client_)),
      champions_(builtInChampions())
{
    connect(assets_.get(), &AssetCache::championPortraitAvailable, this, [this](int) { emit championPortraitsChanged(); });
    connect(assets_.get(), &AssetCache::profileIconAvailable, this, [this](const int profileIconId) { emit profileIconAvailable(profileIconId); });
    connect(assets_.get(), &AssetCache::staticIconsAvailable, this, [this] { emit staticIconsChanged(); });
}

ChampionRepository::~ChampionRepository() = default;

QString ChampionRepository::championNameFor(const int id, const QString &fallback) const
{
    for (const Champion &champion : champions_) if (champion.id == id) return champion.name;
    if (const ChampionCatalogEntry *catalog = catalogFor(id)) return QString::fromUtf8(catalog->name);
    return fallback.trimmed();
}

QString ChampionRepository::championKeyFor(const int id) const
{
    for (const Champion &champion : champions_) if (champion.id == id) return champion.key;
    if (const ChampionCatalogEntry *catalog = catalogFor(id)) return QString::fromLatin1(catalog->key);
    return {};
}

QStringList ChampionRepository::championTypeLabelsFor(const int id) const
{
    for (const Champion &champion : champions_) {
        if (champion.id != id) continue;
        if (!champion.roles.isEmpty()) return championTypeLabels(champion);
        Champion fallback = champion;
        fallback.roles = fallbackRolesFor(champion.key);
        return championTypeLabels(fallback);
    }
    return {};
}

int ChampionRepository::resolveChampionId(const int id, const QString &nameOrKey) const
{
    if (id > 0 && id < 10000) {
        for (const Champion &champion : champions_) if (champion.id == id) return id;
        if (catalogFor(id)) return id;
    }
    const QString key = normalizedChampionKey(nameOrKey);
    if (key.isEmpty()) return 0;
    for (const Champion &champion : champions_) {
        if (normalizedChampionKey(champion.key) == key || normalizedChampionKey(champion.name) == key) return champion.id;
    }
    for (const ChampionCatalogEntry &entry : championCatalog()) {
        if (normalizedChampionKey(QString::fromLatin1(entry.key)) == key
            || normalizedChampionKey(QString::fromUtf8(entry.name)) == key) return entry.id;
    }
    return 0;
}

QPixmap ChampionRepository::portraitFor(const int id) const
{
    QPixmap portrait = assets_->championPortrait(id);
    if (portrait.isNull()) assets_->ensureChampionPortrait(id);
    return portrait;
}
QPixmap ChampionRepository::profileIconFor(const int profileIconId) const { return assets_->profileIcon(profileIconId); }
QPixmap ChampionRepository::itemIconFor(const int itemId) const { return assets_->itemIcon(itemId); }
QPixmap ChampionRepository::summonerSpellIconFor(const int spellId) const { return assets_->summonerSpellIcon(spellId); }
QPixmap ChampionRepository::runeIconFor(const int runeId) const { return assets_->runeIcon(runeId); }
QPixmap ChampionRepository::rankEmblemFor(const QString &tier)
{
    assets_->cacheRankEmblem(tier);
    return assets_->rankEmblem(tier);
}
QString ChampionRepository::itemNameFor(const int itemId) const { return assets_->itemName(itemId); }
QString ChampionRepository::summonerSpellNameFor(const int spellId) const { return assets_->summonerSpellName(spellId); }
QString ChampionRepository::runeNameFor(const int runeId) const { return assets_->runeName(runeId); }
LcuClient &ChampionRepository::lcuClient() { return *client_; }
void ChampionRepository::warmStaticAssets() { assets_->warmStaticAssets(); }

void ChampionRepository::setAlias(int id, const QString &alias)
{
    aliases_.setAlias(id, alias);
    emit championsChanged();
}

void ChampionRepository::refresh()
{
    const quint64 generation = ++refreshGeneration_;
    pendingTitleChampionIds_.clear();
    emit loadingChanged(true, "正在从 LCU 获取英雄列表...");
    const QPointer<ChampionRepository> repository(this);
    client_->get("/lol-game-data/assets/v1/champion-summary.json", [repository, generation](QJsonDocument document, QString error) {
        if (!repository || repository->refreshGeneration_ != generation) return;

        QList<Champion> loaded = error.isEmpty() ? championsFromDocument(document) : QList<Champion>{};
        if (loaded.isEmpty()) {
            repository->client_->get("/lol-champions/v1/owned-champions-minimal", [repository, generation, error = std::move(error)](QJsonDocument owned, QString ownedError) {
                if (!repository || repository->refreshGeneration_ != generation) return;
                QList<Champion> fallback = ownedError.isEmpty() ? championsFromDocument(owned) : QList<Champion>{};
                if (fallback.isEmpty()) {
                    repository->champions_ = builtInChampions();
                    emit repository->championsChanged();
                    emit repository->loadingChanged(false, error.isEmpty() ? ownedError : error);
                    return;
                }
                sortChampions(fallback);
                repository->champions_ = std::move(fallback);
                repository->assets_->cacheChampionPortraits(repository->champions_);
                repository->assets_->warmStaticAssets();
                emit repository->championsChanged();
                emit repository->loadingChanged(false, {});
                repository->loadMissingChampionTitles(generation);
            });
            return;
        }

        sortChampions(loaded);
        repository->champions_ = std::move(loaded);
        emit repository->championsChanged();
        emit repository->loadingChanged(false, {});
        repository->loadOwnedChampionDetails(generation);
    });
}

void ChampionRepository::loadOwnedChampionDetails(const quint64 generation)
{
    const QPointer<ChampionRepository> repository(this);
    client_->get("/lol-champions/v1/owned-champions-minimal", [repository, generation](QJsonDocument document, QString error) {
        if (!repository || repository->refreshGeneration_ != generation) return;

        bool changed = false;
        if (error.isEmpty()) {
            QHash<int, int> indexById;
            indexById.reserve(repository->champions_.size());
            for (int index = 0; index < repository->champions_.size(); ++index) {
                indexById.insert(repository->champions_.at(index).id, index);
            }
            for (const Champion &owned : championsFromDocument(document)) {
                const auto found = indexById.constFind(owned.id);
                if (found == indexById.cend()) continue;
                Champion &champion = repository->champions_[found.value()];
                    if (!owned.name.isEmpty() && champion.name != owned.name && !catalogFor(champion.id, champion.key)) {
                    champion.name = owned.name;
                    changed = true;
                }
                if (!owned.title.isEmpty() && champion.title != owned.title) {
                    champion.title = owned.title;
                    changed = true;
                }
                if (!owned.squarePortraitPath.isEmpty() && champion.squarePortraitPath != owned.squarePortraitPath) {
                    champion.squarePortraitPath = owned.squarePortraitPath;
                    changed = true;
                }
                if (!owned.roles.isEmpty() && champion.roles != owned.roles) {
                    champion.roles = owned.roles;
                    changed = true;
                }
            }
        }

        if (changed) sortChampions(repository->champions_);
        repository->assets_->cacheChampionPortraits(repository->champions_);
        repository->assets_->warmStaticAssets();
        if (changed) emit repository->championsChanged();
        repository->loadMissingChampionTitles(generation);
    });
}

void ChampionRepository::loadMissingChampionTitles(const quint64 generation)
{
    if (refreshGeneration_ != generation) return;
    pendingTitleChampionIds_.clear();
    for (const Champion &champion : champions_) {
        if (champion.id > 0 && (champion.title.trimmed().isEmpty() || champion.damageType.trimmed().isEmpty())) {
            pendingTitleChampionIds_.append(champion.id);
        }
    }
    loadNextChampionTitle(generation);
}

void ChampionRepository::loadNextChampionTitle(const quint64 generation)
{
    if (refreshGeneration_ != generation || pendingTitleChampionIds_.isEmpty()) return;
    const int championId = pendingTitleChampionIds_.takeFirst();
    const QPointer<ChampionRepository> repository(this);
    client_->get("/lol-game-data/assets/v1/champions/" + QString::number(championId) + ".json",
        [repository, generation, championId](QJsonDocument document, QString error) {
            if (!repository || repository->refreshGeneration_ != generation) return;
            if (error.isEmpty() && document.isObject()) {
                const QJsonObject object = document.object();
                const QString title = object.value("title").toString();
                const QString name = object.value("name").toString();
                const QString portraitPath = object.value("squarePortraitPath").toString();
                const QStringList roles = rolesFromValue(object.value("roles"));
                const QString damageType = damageTypeFromObject(object);
                for (Champion &champion : repository->champions_) {
                    if (champion.id != championId) continue;
                    bool changed = false;
                    if (!title.isEmpty() && champion.title != title) {
                        champion.title = title;
                        changed = true;
                    }
                    if (!name.isEmpty() && champion.name != name && !catalogFor(champion.id, champion.key)) {
                        champion.name = name;
                        changed = true;
                    }
                    if (!portraitPath.isEmpty() && champion.squarePortraitPath != portraitPath) {
                        champion.squarePortraitPath = portraitPath;
                        changed = true;
                    }
                    if (!roles.isEmpty() && champion.roles != roles) {
                        champion.roles = roles;
                        changed = true;
                    }
                    if (!damageType.isEmpty() && champion.damageType != damageType) {
                        champion.damageType = damageType;
                        changed = true;
                    }
                    localizeChampion(champion);
                    if (changed) {
                        sortChampions(repository->champions_);
                        repository->assets_->cacheChampionPortraits(repository->champions_);
                        emit repository->championsChanged();
                    }
                    break;
                }
            }
            repository->loadNextChampionTitle(generation);
        });
}
}
