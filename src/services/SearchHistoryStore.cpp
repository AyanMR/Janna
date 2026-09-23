#include "services/SearchHistoryStore.h"

#include <QSettings>
#include <QVariant>

namespace Janna {
namespace {

constexpr int maximumHistoryEntries = 6;
constexpr auto historySettingsKey = "search/history";

bool canOpen(const SearchHistoryEntry &entry)
{
    return !entry.riotId.trimmed().isEmpty()
        && (!entry.puuid.trimmed().isEmpty() || !entry.summonerId.trimmed().isEmpty());
}

} // namespace

QList<SearchHistoryEntry> SearchHistoryStore::entries() const
{
    QSettings settings;
    // A separate QSettings instance may still have a cached native-format
    // value after a previous remove/clear operation.  Sync before reading so
    // the suggestion panel always reflects the just-written history.
    settings.sync();
    const QVariantList storedEntries = settings.value(historySettingsKey).toList();
    QList<SearchHistoryEntry> result;
    result.reserve(qMin(storedEntries.size(), maximumHistoryEntries));
    for (const QVariant &storedEntry : storedEntries) {
        const QVariantMap values = storedEntry.toMap();
        SearchHistoryEntry entry{
            values.value("riotId").toString().trimmed(),
            values.value("puuid").toString().trimmed(),
            values.value("summonerId").toString().trimmed()
        };
        if (canOpen(entry)) result.append(std::move(entry));
        if (result.size() == maximumHistoryEntries) break;
    }
    return result;
}

void SearchHistoryStore::record(const SearchHistoryEntry &entry) const
{
    SearchHistoryEntry normalized{entry.riotId.trimmed(), entry.puuid.trimmed(), entry.summonerId.trimmed()};
    if (!canOpen(normalized)) return;

    QList<SearchHistoryEntry> updated = entries();
    for (int index = updated.size() - 1; index >= 0; --index) {
        if (isSamePlayer(updated.at(index), normalized)) updated.removeAt(index);
    }
    updated.prepend(std::move(normalized));
    while (updated.size() > maximumHistoryEntries) updated.removeLast();
    writeEntries(updated);
}

void SearchHistoryStore::remove(const SearchHistoryEntry &entry) const
{
    const SearchHistoryEntry normalized{entry.riotId.trimmed(), entry.puuid.trimmed(), entry.summonerId.trimmed()};
    QList<SearchHistoryEntry> updated = entries();
    for (int index = updated.size() - 1; index >= 0; --index) {
        if (isSamePlayer(updated.at(index), normalized)) updated.removeAt(index);
    }
    writeEntries(updated);
}

void SearchHistoryStore::clear() const
{
    writeEntries(QList<SearchHistoryEntry>{});
}

bool SearchHistoryStore::isSamePlayer(const SearchHistoryEntry &left, const SearchHistoryEntry &right)
{
    if (!left.puuid.isEmpty() && !right.puuid.isEmpty()
        && left.puuid.compare(right.puuid, Qt::CaseInsensitive) == 0) return true;
    if (!left.summonerId.isEmpty() && !right.summonerId.isEmpty()
        && left.summonerId.compare(right.summonerId, Qt::CaseInsensitive) == 0) return true;
    return left.riotId.compare(right.riotId, Qt::CaseInsensitive) == 0;
}

void SearchHistoryStore::writeEntries(const QList<SearchHistoryEntry> &entries)
{
    QVariantList serialized;
    serialized.reserve(qMin(entries.size(), maximumHistoryEntries));
    for (int index = 0; index < entries.size() && index < maximumHistoryEntries; ++index) {
        const SearchHistoryEntry &entry = entries.at(index);
        if (!canOpen(entry)) continue;
        serialized.append(QVariantMap{
            {"riotId", entry.riotId},
            {"puuid", entry.puuid},
            {"summonerId", entry.summonerId}
        });
    }
    QSettings settings;
    if (serialized.isEmpty()) settings.remove(historySettingsKey);
    else settings.setValue(historySettingsKey, serialized);
    settings.sync();
}

} // namespace Janna
