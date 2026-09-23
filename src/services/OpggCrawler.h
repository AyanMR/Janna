#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

namespace Janna {

struct OpggChampionReference {
    int championId{};
    QString championName;
    QString championKey;
    double winRate{};
    // Win rate of the selected champion in the matchup.  OP.GG's counter
    // page reports both sides; keeping both values lets the UI explain which
    // side has the advantage instead of showing an unlabeled list.
    double championWinRate{};
    int games{};
    QString relation;

    [[nodiscard]] bool isValid() const { return championId > 0 || !championName.trimmed().isEmpty(); }
};

// One OP.GG rune candidate.  The name and image URL come from the page and
// are deliberately kept alongside the id because the LCU catalogue can be
// stale or unavailable while the web page is still current.
struct OpggRuneChoice {
    int id{};
    QString name;
    QString imageUrl;
    bool active{};
    double pickRate{};
    double winRate{};
    int games{};

    [[nodiscard]] bool isValid() const { return id > 0 || !name.trimmed().isEmpty(); }
};

struct OpggLaneStat {
    QString position;
    double pickRate{};
    int games{};

    [[nodiscard]] bool isValid() const { return !position.trimmed().isEmpty() && pickRate > 0.0; }
};

struct OpggSpellBuild {
    QList<int> spellIds;
    double winRate{};
    double pickRate{};
    int games{};
    QStringList spellNames;
    QStringList spellImageUrls;

    [[nodiscard]] bool isValid() const { return spellIds.size() >= 2; }
};

struct OpggChampionRank {
    int championId{};
    QString championName;
    QString championKey;
    int rank{};
    QString tier;
    double winRate{};
    double pickRate{};
    double banRate{};
    int games{};
    QString position;
    // OP.GG class labels returned by the Chinese page, for example
    // "AP输出" and "AD战士". These are distinct from the lane in `position`.
    QStringList roles;
    // The first entries are the champions OP.GG marks as a disadvantage for
    // this row.  They are kept on the row so the leaderboard can render the
    // matchup without making a second request.
    QList<OpggChampionReference> weakAgainst;
    QList<OpggChampionReference> strongAgainst;
    QString version;

    [[nodiscard]] bool isValid() const { return championId > 0 || !championName.trimmed().isEmpty(); }
};

struct OpggRuneBuild {
    QString label;
    int primaryStyleId{};
    int subStyleId{};
    QList<int> runeIds;
    QList<int> statShardIds;
    QList<int> summonerSpellIds;
    // Candidate ids in each visual row.  OP.GG changes the rune catalogue
    // between patches, so the UI should prefer these over a hard-coded list.
    QList<QList<int>> primaryRows;
    QList<QList<int>> subRows;
    QList<QList<int>> statRows;
    // The structured rows preserve OP.GG order and per-choice metadata.  The
    // integer rows above remain for compatibility with the LCU apply path.
    QList<QList<OpggRuneChoice>> primaryChoices;
    QList<QList<OpggRuneChoice>> subChoices;
    QList<QList<OpggRuneChoice>> statChoices;
    double winRate{};
    double pickRate{};
    int games{};

    [[nodiscard]] bool isValid() const
    {
        return primaryStyleId > 0 && !runeIds.isEmpty();
    }
};

struct OpggItemBuild {
    QString label;
    QList<int> starterItemIds;
    QList<int> bootsItemIds;
    QList<int> supportItemIds;
    QList<int> coreItemIds;
    QList<int> finalItemIds;
    // Flattened in the order shown by OP.GG. This is used by the one-click
    // preview while the stage-specific lists keep the UI labels intact.
    QList<int> itemIds;
    double winRate{};
    double pickRate{};
    int games{};

    struct Choice {
        int itemId{};
        QString name;
        QString imageUrl;
        double pickRate{};
        double winRate{};
        int games{};

        [[nodiscard]] bool isValid() const { return itemId > 0 || !name.trimmed().isEmpty(); }
    };

    struct Group {
        QString label;
        QList<Choice> choices;
        double pickRate{};
        double winRate{};
        int games{};

        [[nodiscard]] bool isValid() const { return !choices.isEmpty(); }
    };

    // Each list is rendered as columns/rows of the same OP.GG table.  A
    // group is one table row, so alternatives are never flattened into a
    // misleading single purchase sequence.
    QList<Group> starterGroups;
    QList<Group> bootsGroups;
    QList<Group> supportGroups;
    QList<Group> coreGroups;
    QList<Group> fourthGroups;
    QList<Group> fifthGroups;
    QList<Group> sixthGroups;

    [[nodiscard]] bool isValid() const { return !itemIds.isEmpty(); }
};

struct OpggBuild {
    int championId{};
    QString championName;
    QString championKey;
    QString position;
    int primaryStyleId{};
    int subStyleId{};
    QList<int> runeIds;
    QList<int> statShardIds;
    QList<int> summonerSpellIds;
    QList<int> itemIds;
    QList<OpggChampionReference> counters;
    QList<OpggChampionReference> favorableCounters;
    QList<OpggChampionReference> weakCounters;
    QList<OpggRuneBuild> runeBuilds;
    QList<OpggItemBuild> itemBuilds;
    QList<OpggLaneStat> laneStats;
    QList<OpggSpellBuild> spellBuilds;
    QStringList skillOrder;
    QString version;
    // The selected OP.GG filter (for example emerald_plus).  This is kept
    // separately from the champion's strength tier.
    QString rankTier;
    QString sourceUrl;
    // A champion may expose more than one OP.GG class (for example Zed is
    // both AD刺客 and AD战士). Keep all labels in page order.
    QStringList roles;

    [[nodiscard]] bool isValid() const
    {
        return championId > 0 || !championName.trimmed().isEmpty()
            || !runeIds.isEmpty() || primaryStyleId > 0
            || !runeBuilds.isEmpty() || !itemBuilds.isEmpty();
    }
};

class OpggCrawler {
public:
    using Reply = std::function<void(QString html, QString error)>;
    using BinaryReply = std::function<void(QByteArray bytes, QString error)>;
    using RankingReply = std::function<void(QList<OpggChampionRank> ranking, QString error)>;
    using BuildReply = std::function<void(OpggBuild build, QString error)>;
    OpggCrawler();
    ~OpggCrawler();
    OpggCrawler(const OpggCrawler &) = delete;
    OpggCrawler &operator=(const OpggCrawler &) = delete;

    // Fetches one public profile page after an explicit user search.
    void fetchProfile(const QString &region, const QString &gameName, const QString &tagLine, Reply reply);
    // Fetches a public OP.GG image URL (used as a fallback when the local
    // LCU/CommunityDragon static catalog has not supplied a rune icon yet).
    void fetchImage(const QString &url, BinaryReply reply);

    // Fetches the OP.GG champion tier list for a queue/mode and optional
    // position.  An empty position intentionally requests the mode default.
    void fetchChampionRanking(const QString &mode, const QString &position, RankingReply reply);
    void fetchChampionRanking(const QString &mode, const QString &position, RankingReply reply,
                              const QString &rankTier, const QString &version);
    void fetchChampionBuild(int championId, const QString &mode, const QString &position, BuildReply reply,
                            const QString &championSlug = {});
    void fetchChampionBuild(int championId, const QString &mode, const QString &position, BuildReply reply,
                            const QString &championSlug, const QString &rankTier, const QString &version);

    // Kept public so fixture-based tests and future API shape changes can be
    // handled without opening a network connection.
    [[nodiscard]] static QList<OpggChampionRank> parseChampionRanking(const QByteArray &payload,
                                                                       QString *error = nullptr);
    [[nodiscard]] static OpggBuild parseBuild(const QByteArray &payload, QString *error = nullptr);
    [[nodiscard]] static QList<OpggChampionReference> parseCounterPage(const QByteArray &payload,
                                                                         QString *error = nullptr);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
