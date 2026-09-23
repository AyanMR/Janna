#pragma once

#include <QList>
#include <QString>

namespace Janna {

struct SearchHistoryEntry {
    QString riotId;
    QString puuid;
    QString summonerId;
};

class SearchHistoryStore final {
public:
    [[nodiscard]] QList<SearchHistoryEntry> entries() const;
    void record(const SearchHistoryEntry &entry) const;
    void remove(const SearchHistoryEntry &entry) const;
    void clear() const;

private:
    static bool isSamePlayer(const SearchHistoryEntry &left, const SearchHistoryEntry &right);
    static void writeEntries(const QList<SearchHistoryEntry> &entries);
};

} // namespace Janna
