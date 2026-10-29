#pragma once
#include "api_client.hpp"
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QTimer>
#include <QUrlQuery>

class Controller : public QObject {
    Q_OBJECT
public:
    explicit Controller(QObject* parent = nullptr);
    bool select(const QString& base, const QString& device, const QString& filter);
    void refresh();
    void createQuery();
    void retryQuery();
    void alarms(bool reset);
    void setAutoRefresh(bool enabled);
    void setTiming(int requestMs, int refreshMs, int pollMs, int observeMs);
    QJsonObject device, task;
    QJsonArray alarmRows;
    QString statusText, queryText, alarmText, requestId, taskId, nextCursor;
    bool statusBusy = false, queryBusy = false, alarmBusy = false, queryUncertain = false, queryActive = false;
    bool more = true;
signals:
    void changed();
private:
    struct Saved { QString request, id; QJsonObject task; bool uncertain = false, active = false; };
    QHash<QString, Saved> contexts;
    QString baseText, dev, filter, contextKey;
    QUrl baseUrl;
    quint64 generation = 0;
    ApiClient client;
    QTimer refreshTimer, pollTimer;
    QElapsedTimer observation;
    int deadline = 3000, observeLimit = 10000;
    void query(bool post);
    QUrl url(const QString& path, const QUrlQuery& parameters = {}) const;
    void save();
};
