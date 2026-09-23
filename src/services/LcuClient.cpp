#include "services/LcuClient.h"
#include "services/LcuCredentialProvider.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonDocument>
#include <QMetaObject>

#include <cstdint>
#include <deque>
#include <limits>
#include <thread>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using asio::ip::tcp;

namespace Janna {
class LcuClient::Impl {
public:
    Impl() : work_(asio::make_work_guard(context_)), ssl_(asio::ssl::context::tls_client), worker_([this] { context_.run(); })
    {
        ssl_.set_verify_mode(asio::ssl::verify_none); // LCU uses a local self-signed certificate.
    }
    ~Impl() { work_.reset(); context_.stop(); if (worker_.joinable()) worker_.join(); }

    void request(const QString &method, const QString &path, const QByteArray &body, BytesReply reply)
    {
        asio::post(context_, [this, method, path, body, reply = std::move(reply)]() mutable {
            queuedRequests_.push_back({method, path, body, std::move(reply)});
            startNextRequest();
        });
    }

    void getBytes(const QString &path, BytesReply reply)
    {
        request("GET", path, {}, std::move(reply));
    }

private:
    struct QueuedRequest {
        QString method;
        QString path;
        QByteArray body;
        BytesReply reply;
    };

    struct Connection {
        Connection(asio::io_context &io, asio::ssl::context &context, LcuCredentials credentials)
            : stream(io, context), resolver(io), credentials(std::move(credentials)) {}

        void close()
        {
            ready = false;
            boost::system::error_code ignored;
            resolver.cancel();
            stream.next_layer().close(ignored);
        }

        asio::ssl::stream<tcp::socket> stream;
        tcp::resolver resolver;
        LcuCredentials credentials;
        bool ready{};
    };

    struct Request : std::enable_shared_from_this<Request> {
        Request(std::shared_ptr<Connection> connection, QString method, QString path,
                QByteArray body, BytesReply reply)
            : connection(std::move(connection)), method(std::move(method)), path(std::move(path)),
              body(std::move(body)), reply(std::move(reply)) {}

        void start()
        {
            if (connection->ready && connection->stream.next_layer().is_open()) return send();

            connection->close();
            connection->resolver.async_resolve("127.0.0.1", connection->credentials.port.toStdString(), [self = shared_from_this()](const boost::system::error_code &resolveError, const tcp::resolver::results_type &endpoints) {
                if (resolveError) return self->complete("无法解析 LCU 地址：" + QString::fromStdString(resolveError.message()));
                asio::async_connect(self->connection->stream.next_layer(), endpoints, [self](const boost::system::error_code &ec, const tcp::endpoint &) {
                if (ec) return self->complete("无法连接到 LCU：" + QString::fromStdString(ec.message()));
                self->connection->stream.async_handshake(asio::ssl::stream_base::client, [self](const boost::system::error_code &handshakeError) {
                    if (handshakeError) return self->complete("LCU TLS 握手失败：" + QString::fromStdString(handshakeError.message()));
                    self->connection->ready = true;
                    self->send();
                });
                });
            });
        }

        void send()
        {
            const QByteArray token = QByteArray("riot:") + connection->credentials.token.toUtf8();
            const QString normalizedMethod = method.trimmed().toUpper();
            http::verb verb = http::verb::get;
            if (normalizedMethod == "POST") verb = http::verb::post;
            else if (normalizedMethod == "PUT") verb = http::verb::put;
            else if (normalizedMethod == "PATCH") verb = http::verb::patch;
            else if (normalizedMethod == "DELETE") verb = http::verb::delete_;
            request = {verb, path.toUtf8().toStdString(), 11};
            request.set(http::field::host, "127.0.0.1:" + connection->credentials.port.toStdString());
            request.set(http::field::authorization, "Basic " + token.toBase64().toStdString());
            request.set(http::field::connection, "keep-alive");
            if (!body.isEmpty()) {
                request.set(http::field::content_type, "application/json");
                request.body() = body.toStdString();
                request.prepare_payload();
            }
            http::async_write(connection->stream, request, [self = shared_from_this()](const boost::system::error_code &ec, std::size_t) {
                if (ec) return self->complete("LCU 请求发送失败：" + QString::fromStdString(ec.message()));
                self->read();
            });
        }

        void read()
        {
            parser.body_limit(maximumResponseBytes);
            http::async_read(connection->stream, response, parser, [self = shared_from_this()](const boost::system::error_code &ec, std::size_t) {
                if (ec) return self->complete("LCU 响应读取失败：" + QString::fromStdString(ec.message()));
                self->finishResponse();
            });
        }

        void finishResponse()
        {
            auto message = parser.release();
            if (message.result_int() < 200 || message.result_int() >= 300) {
                // Some LCU services leave an error response connection in a
                // non-reusable state even when it advertises keep-alive.
                connection->close();
                return complete("LCU 返回了非成功响应（HTTP " + QString::number(message.result_int()) + "）。");
            }

            const auto &responseBody = message.body();
            const std::size_t bodySize = responseBody.size();
            if (bodySize > static_cast<std::size_t>(std::numeric_limits<int>::max())) return complete("LCU 响应过大。");
            QByteArray body(reinterpret_cast<const char *>(responseBody.data()), static_cast<int>(bodySize));
            if (!message.keep_alive()) connection->close();
            finish(std::move(body));
        }

        void finish(QByteArray body)
        {
            auto callback = std::move(reply);
            callback(std::move(body), {});
        }

        void complete(QString error)
        {
            connection->close();
            auto callback = std::move(reply);
            callback({}, std::move(error));
        }
        std::shared_ptr<Connection> connection;
        static constexpr std::size_t maximumResponseBytes = 64U * 1024U * 1024U;

        QString path;
        QString method;
        QByteArray body;
        BytesReply reply;
        http::request<http::string_body> request;
        beast::flat_buffer response;
        http::response_parser<http::vector_body<std::uint8_t>> parser;
    };

    void startNextRequest()
    {
        if (requestInFlight_ || queuedRequests_.empty()) return;
        requestInFlight_ = true;
        QueuedRequest queued = std::move(queuedRequests_.front());
        queuedRequests_.pop_front();

        QString error;
        const LcuCredentials credentials = LcuCredentialProvider::discover(error);
        if (!error.isEmpty()) {
            requestInFlight_ = false;
            finish(std::move(queued.reply), {}, std::move(error));
            startNextRequest();
            return;
        }

        const bool requiresFreshConnection = !connection_
            || !connection_->ready
            || !connection_->stream.next_layer().is_open()
            || connection_->credentials.port != credentials.port
            || connection_->credentials.token != credentials.token;
        if (requiresFreshConnection) {
            if (connection_) connection_->close();
            connection_ = std::make_shared<Connection>(context_, ssl_, credentials);
        }
        auto request = std::make_shared<Request>(connection_, std::move(queued.method),
            std::move(queued.path), std::move(queued.body),
            [this, reply = std::move(queued.reply)](QByteArray bytes, QString requestError) mutable {
                requestInFlight_ = false;
                finish(std::move(reply), std::move(bytes), std::move(requestError));
                asio::post(context_, [this] { startNextRequest(); });
            });
        request->start();
    }

    static void finish(BytesReply reply, QByteArray bytes, QString error)
    {
        QMetaObject::invokeMethod(QCoreApplication::instance(), [reply = std::move(reply), bytes = std::move(bytes), error = std::move(error)] { reply(bytes, error); }, Qt::QueuedConnection);
    }

    asio::io_context context_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    asio::ssl::context ssl_;
    std::deque<QueuedRequest> queuedRequests_;
    bool requestInFlight_{};
    std::shared_ptr<Connection> connection_;
    std::thread worker_;
};

LcuClient::LcuClient() : impl_(std::make_unique<Impl>()) {}
LcuClient::~LcuClient() = default;
void LcuClient::request(const QString &method, const QString &path, const QByteArray &body, BytesReply reply)
{
    impl_->request(method, path, body, std::move(reply));
}

void LcuClient::requestJson(const QString &method, const QString &path, const QJsonObject &body, Reply reply)
{
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    request(method, path, payload, [reply = std::move(reply)](QByteArray bytes, QString error) mutable {
        if (!error.isEmpty()) {
            reply({}, std::move(error));
            return;
        }
        if (bytes.trimmed().isEmpty()) {
            reply(QJsonDocument(QJsonObject{}), {});
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(bytes);
        if (document.isNull()) {
            reply({}, "LCU 返回的数据不是有效 JSON。");
            return;
        }
        reply(document, {});
    });
}

void LcuClient::get(const QString &path, Reply reply)
{
    getBytes(path, [reply = std::move(reply)](QByteArray bytes, QString error) mutable {
        if (!error.isEmpty()) {
            reply({}, std::move(error));
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(bytes);
        if (document.isNull()) {
            reply({}, "LCU 返回的数据不是有效 JSON。");
            return;
        }
        reply(document, {});
    });
}

void LcuClient::getBytes(const QString &path, BytesReply reply) { impl_->getBytes(path, std::move(reply)); }
void LcuClient::post(const QString &path, const QByteArray &body, BytesReply reply)
{
    request("POST", path, body, std::move(reply));
}

void LcuClient::put(const QString &path, const QByteArray &body, BytesReply reply)
{
    request("PUT", path, body, std::move(reply));
}

void LcuClient::patch(const QString &path, const QByteArray &body, BytesReply reply)
{
    request("PATCH", path, body, std::move(reply));
}

void LcuClient::postJson(const QString &path, const QJsonObject &body, Reply reply)
{
    requestJson("POST", path, body, std::move(reply));
}

void LcuClient::putJson(const QString &path, const QJsonObject &body, Reply reply)
{
    requestJson("PUT", path, body, std::move(reply));
}

void LcuClient::patchJson(const QString &path, const QJsonObject &body, Reply reply)
{
    requestJson("PATCH", path, body, std::move(reply));
}
}
