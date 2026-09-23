#include "services/LeagueClientMonitor.h"

#include <boost/asio.hpp>
#include <QMetaObject>
#include <QPointer>
#include <Windows.h>
#include <TlHelp32.h>
#include <thread>

namespace asio = boost::asio;

namespace Janna {
namespace {
bool leagueClientIsRunning()
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"LeagueClient.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}
}

class LeagueClientMonitor::Impl {
public:
    explicit Impl(LeagueClientMonitor *owner)
        : owner_(owner), work_(asio::make_work_guard(context_)), timer_(context_), worker_([this] { context_.run(); }) {}

    ~Impl()
    {
        work_.reset();
        timer_.cancel();
        context_.stop();
        if (worker_.joinable()) worker_.join();
    }

    void start()
    {
        asio::post(context_, [this] { poll(); });
    }

private:
    void poll()
    {
        const bool available = leagueClientIsRunning();
        const QPointer<LeagueClientMonitor> owner = owner_;
        if (owner) {
            QMetaObject::invokeMethod(owner.data(), [owner, available] {
                if (owner) emit owner->availabilityChanged(available);
            }, Qt::QueuedConnection);
        }
        timer_.expires_after(std::chrono::seconds(2));
        timer_.async_wait([this](const boost::system::error_code &error) {
            if (!error) poll();
        });
    }

    LeagueClientMonitor *owner_{};
    asio::io_context context_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    asio::steady_timer timer_;
    std::thread worker_;
};

LeagueClientMonitor::LeagueClientMonitor(QObject *parent) : QObject(parent), impl_(std::make_unique<Impl>(this)) {}
LeagueClientMonitor::~LeagueClientMonitor() = default;
bool LeagueClientMonitor::isLeagueClientRunning() { return leagueClientIsRunning(); }
void LeagueClientMonitor::start() { impl_->start(); }

} // namespace Janna
