#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

namespace Janna {

class LcuClient;

struct GameQueue {
    int id{};
    QString name;
};

class GameDataRepository final : public QObject {
    Q_OBJECT

public:
    explicit GameDataRepository(LcuClient &client, QObject *parent = nullptr);

    [[nodiscard]] QString queueName(int queueId) const;
    [[nodiscard]] QString mapName(int mapId) const;
    [[nodiscard]] QString modeName(int queueId, int mapId, const QString &rawGameMode = {}) const;
    [[nodiscard]] QList<GameQueue> queues() const;

public slots:
    void refresh();

signals:
    void catalogChanged();

private:
    void applyCatalog(const QJsonDocument &document, bool queueCatalog, quint64 generation, const QString &error);

    LcuClient &client_;
    QHash<int, QString> queueNames_;
    QHash<int, QString> mapNames_;
    quint64 refreshGeneration_{};
    int pendingCatalogs_{};
    bool catalogChangedDuringRefresh_{};
};

} // namespace Janna
