#include "services/CommunityDragonClient.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <openssl/ssl.h>
#include <QCoreApplication>
#include <QMetaObject>

#include <cstdint>
#include <limits>
#include <thread>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using asio::ip::tcp;

namespace Janna {
namespace {

constexpr char communityDragonHost[] = "raw.communitydragon.org";
constexpr char profileIconCatalogPath[] = "/latest/plugins/rcp-be-lol-game-data/global/default/v1/profile-icons.json";

} // namespace

class CommunityDragonClient::Impl {
public:
    Impl()
        : work_(asio::make_work_guard(context_)), ssl_(asio::ssl::context::tls_client), worker_([this] { context_.run(); })
    {
        // This public catalog only supplies LCU asset IDs; no user data is sent to it.
        ssl_.set_verify_mode(asio::ssl::verify_none);
    }

    ~Impl()
    {
        work_.reset();
        context_.stop();
        if (worker_.joinable()) worker_.join();
    }

    void fetchProfileIconCatalog(Reply reply)
    {
        fetch(profileIconCatalogPath, std::move(reply));
    }

    void fetchStaticAsset(const QString &path, Reply reply)
    {
        if (!path.startsWith('/') || path.contains("..") || path.contains('\\')) {
            if (auto *application = QCoreApplication::instance()) {
                QMetaObject::invokeMethod(application, [reply = std::move(reply)]() mutable {
                    reply({}, "CommunityDragon 资源路径无效。");
                }, Qt::QueuedConnection);
            }
            return;
        }
        fetch(path, std::move(reply));
    }

private:
    void fetch(QString path, Reply reply)
    {
        asio::post(context_, [this, path = std::move(path), reply = std::move(reply)]() mutable {
            auto request = std::make_shared<Request>(context_, ssl_, std::move(path), std::move(reply));
            request->start();
        });
    }

    struct Request : std::enable_shared_from_this<Request> {
        Request(asio::io_context &context, asio::ssl::context &ssl, QString path, Reply reply)
            : stream(context, ssl), resolver(context), path(std::move(path)), reply(std::move(reply)) {}

        void start()
        {
            resolver.async_resolve(communityDragonHost, "443", [self = shared_from_this()](const boost::system::error_code &error, const tcp::resolver::results_type &endpoints) {
                if (error) return self->finish({}, "无法解析 CommunityDragon 地址：" + QString::fromStdString(error.message()));
                asio::async_connect(self->stream.next_layer(), endpoints, [self](const boost::system::error_code &connectError, const tcp::endpoint &) {
                    if (connectError) return self->finish({}, "无法连接 CommunityDragon：" + QString::fromStdString(connectError.message()));
                    if (::SSL_set_tlsext_host_name(self->stream.native_handle(), communityDragonHost) != 1) {
                        return self->finish({}, "无法为 CommunityDragon 配置 TLS 主机名。");
                    }
                    self->stream.async_handshake(asio::ssl::stream_base::client, [self](const boost::system::error_code &handshakeError) {
                        if (handshakeError) return self->finish({}, "CommunityDragon TLS 握手失败：" + QString::fromStdString(handshakeError.message()));
                        self->send();
                    });
                });
            });
        }

        void send()
        {
            request = {http::verb::get, path.toUtf8().toStdString(), 11};
            request.set(http::field::host, communityDragonHost);
            request.set(http::field::user_agent, "Janna/1.0");
            http::async_write(stream, request, [self = shared_from_this()](const boost::system::error_code &error, std::size_t) {
                if (error) return self->finish({}, "CommunityDragon 请求发送失败：" + QString::fromStdString(error.message()));
                self->read();
            });
        }

        void read()
        {
            parser.body_limit(maximumCatalogBytes);
            http::async_read(stream, response, parser, [self = shared_from_this()](const boost::system::error_code &error, std::size_t) {
                if (error) return self->finish({}, "CommunityDragon 响应读取失败：" + QString::fromStdString(error.message()));
                auto message = self->parser.release();
                if (message.result_int() < 200 || message.result_int() >= 300) {
                    return self->finish({}, "CommunityDragon 返回了非成功响应。");
                }
                const auto &responseBody = message.body();
                if (responseBody.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                    return self->finish({}, "CommunityDragon 返回的数据过大。");
                }
                self->finish(QByteArray(reinterpret_cast<const char *>(responseBody.data()), static_cast<int>(responseBody.size())), {});
            });
        }

        void finish(QByteArray bytes, QString error)
        {
            if (finished) return;
            finished = true;

            boost::system::error_code ignored;
            resolver.cancel();
            stream.next_layer().close(ignored);

            auto callback = std::move(reply);
            if (auto *application = QCoreApplication::instance()) {
                QMetaObject::invokeMethod(application, [callback = std::move(callback), bytes = std::move(bytes), error = std::move(error)] {
                    callback(bytes, error);
                }, Qt::QueuedConnection);
            }
        }

        static constexpr std::size_t maximumCatalogBytes = 2U * 1024U * 1024U;

        asio::ssl::stream<tcp::socket> stream;
        tcp::resolver resolver;
        QString path;
        Reply reply;
        http::request<http::empty_body> request;
        beast::flat_buffer response;
        http::response_parser<http::vector_body<std::uint8_t>> parser;
        bool finished{};
    };

    asio::io_context context_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    asio::ssl::context ssl_;
    std::thread worker_;
};

CommunityDragonClient::CommunityDragonClient() : impl_(std::make_unique<Impl>()) {}
CommunityDragonClient::~CommunityDragonClient() = default;

void CommunityDragonClient::fetchProfileIconCatalog(Reply reply)
{
    impl_->fetchProfileIconCatalog(std::move(reply));
}

void CommunityDragonClient::fetchStaticAsset(const QString &path, Reply reply)
{
    impl_->fetchStaticAsset(path, std::move(reply));
}

} // namespace Janna
