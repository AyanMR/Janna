#pragma once
#include <QHash>
#include <QString>
namespace Janna {
class AliasStore { public: QString aliasFor(int championId) const; void setAlias(int championId, const QString &alias); private: void ensureLoaded() const; mutable bool loaded_{}; mutable QHash<int, QString> aliases_; };
}
