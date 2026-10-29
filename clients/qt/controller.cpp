#include "controller.hpp"
#include <QDateTime>
#include <QJsonDocument>
#include <QUuid>
#include <QRegularExpression>

namespace {
QString failure(const ApiResult& result) {
    if (!result.error.isEmpty()) return result.error;
    // Show only a bounded application code; no SQL/server error message is rendered.
    auto code = result.body.value("error").toObject().value("code").toString();
    static const QRegularExpression safe("^[a-z_]{1,64}$");
    if (!safe.match(code).hasMatch()) code = QStringLiteral("请求被拒绝");
    return QStringLiteral("HTTP %1 / %2").arg(result.status).arg(code);
}
bool terminal(const QString& state) { return state == "success" || state == "timeout" || state == "link_lost"; }
}
Controller::Controller(QObject* parent) : QObject(parent) {
    refreshTimer.setInterval(2000); pollTimer.setInterval(500);
    connect(&refreshTimer, &QTimer::timeout, this, &Controller::refresh);
    connect(&pollTimer, &QTimer::timeout, this, [this] {
        if (observation.elapsed() >= observeLimit) {
            pollTimer.stop(); queryText = QStringLiteral("观察超时 / 结果待确认，可继续查看原任务"); emit changed();
        } else query(false);
    });
}
void Controller::setTiming(int requestMs, int refreshMs, int pollMs, int observeMs) {
    deadline = qBound(50, requestMs, 30000); refreshTimer.setInterval(qBound(100, refreshMs, 60000));
    pollTimer.setInterval(qBound(50, pollMs, 5000)); observeLimit = qBound(100, observeMs, 60000);
}
void Controller::save() {
    if (!contextKey.isEmpty()) contexts[contextKey] = {requestId, taskId, task, queryUncertain, queryActive};
}
bool Controller::select(const QString& base, const QString& deviceName, const QString& status) {
    const QUrl candidate(base.trimmed());
    if (!localBase(candidate) || deviceName.isEmpty() || deviceName.toUtf8().size() > 64 ||
        (status != "" && status != "open" && status != "cleared")) {
        save(); ++generation; client.cancel(); pollTimer.stop(); refreshTimer.stop();
        contextKey.clear(); baseText.clear(); statusBusy = queryBusy = alarmBusy = false;
        device = {}; task = {}; alarmRows = {}; requestId.clear(); taskId.clear(); nextCursor.clear();
        queryUncertain = queryActive = false; more = true;
        statusText = QStringLiteral("请输入本机 HTTP 地址、1–64 字节设备 ID 和有效筛选"); emit changed(); return false;
    }
    if (baseText == base.trimmed() && dev == deviceName && filter == status) return true;
    save(); ++generation; client.cancel(); pollTimer.stop();
    baseText = base.trimmed(); baseUrl = candidate; dev = deviceName; filter = status;
    contextKey = baseUrl.toString() + '\n' + dev;
    const auto saved = contexts.value(contextKey);
    requestId = saved.request; taskId = saved.id; task = saved.task;
    queryUncertain = saved.uncertain; queryActive = saved.active;
    statusBusy = queryBusy = alarmBusy = false;
    device = {}; alarmRows = {}; nextCursor.clear(); more = true;
    statusText = QStringLiteral("等待读取当前设备"); alarmText = QStringLiteral("等待读取告警");
    queryText = requestId.isEmpty() ? QStringLiteral("尚未查询") : QStringLiteral("已保留原请求上下文，可同号恢复");
    emit changed(); return true;
}
QUrl Controller::url(const QString& path, const QUrlQuery& parameters) const {
    auto result = baseUrl; result.setPath(path); result.setQuery(parameters); return result;
}
void Controller::setAutoRefresh(bool enabled) { if (enabled) refreshTimer.start(); else refreshTimer.stop(); }
void Controller::refresh() {
    if (statusBusy || contextKey.isEmpty()) return;
    statusBusy = true; const auto current = generation; emit changed();
    // Encode the entire identifier as one path segment; slash must remain %2F.
    const auto path = QByteArray("/v1/devices/") + QUrl::toPercentEncoding(dev);
    auto target = baseUrl; target.setPath(QString::fromLatin1(path), QUrl::StrictMode);
    client.send(target, nullptr, deadline, [this, current](ApiResult result) {
        if (current != generation) return;
        statusBusy = false;
        const auto state = result.body.value("state").toString();
        const bool fields = result.body.contains("voltage_v") && result.body.contains("current_a") &&
            result.body.contains("soc") && result.body.contains("temperature_c");
        if (result.valid() && result.status == 200 && result.body.value("devId").toString() == dev && fields &&
            (state == "online_fresh" || state == "offline" || state == "connected_no_data" || state == "connected_stale")) {
            device = result.body; statusText = QStringLiteral("当前状态：%1 / 读取时间 %2").arg(state, QDateTime::currentDateTime().toString("HH:mm:ss"));
        } else statusText = (result.valid() && result.status == 200 ? QStringLiteral("设备响应字段无效") : failure(result)) +
            QStringLiteral("；已有数值为旧结果，不能据此判断设备离线");
        emit changed();
    });
}
void Controller::createQuery() {
    if (queryBusy || queryActive || queryUncertain || contextKey.isEmpty()) return;
    requestId = QUuid::createUuid().toString(QUuid::WithoutBraces); taskId.clear(); task = {};
    queryUncertain = true; save(); query(true);
}
void Controller::retryQuery() {
    if (requestId.isEmpty() || queryBusy) return;
    observation.restart(); query(taskId.isEmpty());
}
void Controller::query(bool post) {
    if (queryBusy || contextKey.isEmpty() || (!post && taskId.isEmpty())) return;
    queryBusy = true; const auto current = generation;
    const QJsonObject body{{"devId", dev}, {"clientRequestId", requestId}};
    client.send(url(post ? "/v1/queries" : "/v1/queries/" + taskId), post ? &body : nullptr, deadline,
        [this, current, post](ApiResult result) {
            if (current != generation) return;
            queryBusy = false;
            const auto id = exactId(result.body.value("txnNo"));
            const auto state = result.body.value("state").toString();
            const bool success = result.valid() && (result.status == 200 || (post && result.status == 201));
            if (success && !id.isEmpty() && result.body.value("devId").toString() == dev &&
                (state == "pending" || terminal(state)) && result.body.value("reason").isString() &&
                result.body.contains("response") && (taskId.isEmpty() || taskId == id)) {
                taskId = id; task = result.body; queryUncertain = false; queryActive = state == "pending";
                queryText = QStringLiteral("任务 %1 / %2 / %3").arg(id, state, result.body.value("reason").toString());
                if (queryActive) {
                    if (post || !observation.isValid()) observation.restart();
                    if (observation.elapsed() < observeLimit) pollTimer.start();
                } else pollTimer.stop();
            } else {
                pollTimer.stop();
                const bool rejected = post && result.valid() && (result.status == 400 || result.status == 404 ||
                    result.status == 409 || (result.status == 503 && result.body.value("outcome") == "not_created"));
                if (rejected) { queryUncertain = false; queryActive = false; }
                else if (post) queryUncertain = true;
                queryText = (success ? QStringLiteral("任务响应字段无效") : failure(result)) +
                    (rejected ? QStringLiteral(" / 明确未创建") : QStringLiteral(" / 结果待确认，请保留原号恢复"));
            }
            save(); emit changed();
        });
    emit changed();
}
void Controller::alarms(bool reset) {
    if (alarmBusy || contextKey.isEmpty()) return;
    if (reset) { alarmRows = {}; nextCursor.clear(); more = true; }
    if (!more) return;
    alarmBusy = true; const auto current = generation;
    QUrlQuery parameters; parameters.addQueryItem("devId", dev); parameters.addQueryItem("limit", "20");
    if (!filter.isEmpty()) parameters.addQueryItem("status", filter);
    if (!nextCursor.isEmpty()) parameters.addQueryItem("beforeId", nextCursor);
    client.send(url("/v1/alarms", parameters), nullptr, deadline, [this, current](ApiResult result) {
        if (current != generation) return;
        alarmBusy = false;
        const auto items = result.body.value("items"); const auto cursor = result.body.value("nextBeforeId");
        bool valid = result.valid() && result.status == 200 && items.isArray() && items.toArray().size() <= 20 &&
            (cursor.isNull() || !exactId(cursor).isEmpty());
        QString last = nextCursor;
        for (const auto& entry : items.toArray()) {
            const auto row = entry.toObject(); const auto id = exactId(row.value("id"));
            bool less = last.isEmpty() || id.toLongLong() < last.toLongLong();
            valid = valid && !id.isEmpty() && less && row.value("devId").toString() == dev &&
                row.value("desc").isString() && (row.value("state") == "open" || row.value("state") == "cleared");
            last = id;
        }
        if (!cursor.isNull()) valid = valid && !last.isEmpty() && exactId(cursor) == last;
        if (valid) {
            for (const auto& row : items.toArray()) alarmRows.append(row);
            nextCursor = cursor.isNull() ? QString{} : exactId(cursor); more = !cursor.isNull();
            alarmText = QStringLiteral("已读取 %1 条%2").arg(alarmRows.size()).arg(more ? QStringLiteral("，还有下一页") : QStringLiteral("，已到末页"));
        } else alarmText = result.valid() && result.status == 200 ? QStringLiteral("告警响应字段或游标无效") : failure(result);
        emit changed();
    });
    emit changed();
}
