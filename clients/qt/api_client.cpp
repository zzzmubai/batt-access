#include "api_client.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>

bool localBase(const QUrl& url) {
    return url.isValid() && url.scheme() == "http" && (url.host() == "127.0.0.1" || url.host() == "localhost") &&
        url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment() && (url.path().isEmpty() || url.path() == "/");
}
QString exactId(const QJsonValue& value) {
    if (!value.isDouble()) return {};
    const auto encoded = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    static const QRegularExpression pattern("^\\[([1-9][0-9]{0,18})\\]$");
    const auto match = pattern.match(QString::fromLatin1(encoded));
    bool ok = false;
    const auto number = match.captured(1).toLongLong(&ok);
    return match.hasMatch() && ok && number > 0 ? match.captured(1) : QString{};
}
ApiClient::~ApiClient() { cancel(); }
void ApiClient::cancel() {
    const auto active = replies; replies.clear();
    for (auto* reply : active) { reply->disconnect(this); reply->abort(); reply->deleteLater(); }
}
void ApiClient::send(const QUrl& url, const QJsonObject* body, int deadline, std::function<void(ApiResult)> callback) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto* reply = body ? manager.post(request, QJsonDocument(*body).toJson(QJsonDocument::Compact)) : manager.get(request);
    replies.insert(reply);
    auto* timer = new QTimer(reply); timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [reply] { reply->setProperty("expired", true); reply->abort(); });
    timer->start(deadline);
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > 1024 * 1024) { reply->setProperty("large", true); reply->abort(); }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, timer, callback = std::move(callback)] {
        timer->stop(); replies.remove(reply);
        ApiResult result; result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto bytes = reply->isReadable() ? reply->readAll() : QByteArray{};
        if (reply->property("expired").toBool()) result.error = QStringLiteral("HTTP 请求超时，结果待确认");
        else if (reply->property("large").toBool() || bytes.size() > 1024 * 1024) result.error = QStringLiteral("响应过大");
        else if (!result.status || (reply->error() != QNetworkReply::NoError && result.status < 400))
            result.error = QStringLiteral("网络连接失败，请检查服务及 SSH 隧道");
        else {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(bytes, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) result.error = QStringLiteral("响应不是有效 JSON 对象");
            else result.body = document.object();
        }
        reply->deleteLater(); callback(std::move(result));
    });
}
