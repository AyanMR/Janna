#include "services/AliasStore.h"
#include <QSettings>
namespace Janna {
void AliasStore::ensureLoaded() const { if (loaded_) return; QSettings settings; settings.beginGroup("championAliases"); for (const QString &key : settings.childKeys()) aliases_.insert(key.toInt(), settings.value(key).toString()); settings.endGroup(); loaded_ = true; }
QString AliasStore::aliasFor(int championId) const { ensureLoaded(); return aliases_.value(championId); }
void AliasStore::setAlias(int championId, const QString &alias) { ensureLoaded(); const QString value = alias.trimmed(); if (value.isEmpty()) aliases_.remove(championId); else aliases_.insert(championId, value); QSettings settings; settings.beginGroup("championAliases"); if (value.isEmpty()) settings.remove(QString::number(championId)); else settings.setValue(QString::number(championId), value); settings.endGroup(); }
}
