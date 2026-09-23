#pragma once

#include <QObject>
#include <memory>

namespace Janna {

class LeagueClientMonitor : public QObject {
    Q_OBJECT

public:
    explicit LeagueClientMonitor(QObject *parent = nullptr);
    ~LeagueClientMonitor() override;

    static bool isLeagueClientRunning();
    void start();

signals:
    void availabilityChanged(bool available);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Janna
