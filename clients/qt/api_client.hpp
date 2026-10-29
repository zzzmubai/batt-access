#pragma once
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QSet>
#include <functional>

struct ApiResult {
    int status = 0;
    QJsonObject body;
    QString error;
    bool valid() const { return error.isEmpty(); }
};
class ApiClient : public QObject {
public:
    explicit ApiClient(QObject* parent = nullptr) : QObject(parent) {}
    ~ApiClient() override;
    void send(const QUrl& url, const QJsonObject* body, int deadline,
              std::function<void(ApiResult)> callback);
    void cancel();
private:
    QNetworkAccessManager manager;
    QSet<QNetworkReply*> replies;
};
QString exactId(const QJsonValue& value);
bool localBase(const QUrl& url);
