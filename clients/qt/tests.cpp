#include "controller.hpp"
#include "window.hpp"
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#include <QFontDatabase>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QSettings>
#include <QTemporaryDir>

struct Response { int code = 200; QByteArray body; int delay = 0; bool lost = false; };
struct Request { QByteArray method, target, body; };
class FakeHttp : public QTcpServer {
public:
    QList<Request> received;
    std::function<Response(const Request&)> respond;
    FakeHttp() {
        if (!listen(QHostAddress::LocalHost)) qFatal("fixture listen failed");
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    auto bytes = socket->property("buffer").toByteArray() + socket->readAll();
                    socket->setProperty("buffer", bytes);
                    if (socket->property("handled").toBool()) return;
                    const auto split = bytes.indexOf("\r\n\r\n"); if (split < 0) return;
                    int length = 0;
                    for (const auto& line : bytes.left(split).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (bytes.size() < split + 4 + length) return;
                    socket->setProperty("handled", true);
                    const auto first = bytes.left(bytes.indexOf("\r\n")).split(' ');
                    Request request{first[0], first[1], bytes.mid(split + 4, length)}; received.append(request);
                    const auto response = respond(request);
                    QTimer::singleShot(response.delay, socket, [socket, response] {
                        if (!response.lost) socket->write("HTTP/1.1 " + QByteArray::number(response.code) + " Test\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                            QByteArray::number(response.body.size()) + "\r\n\r\n" + response.body);
                        socket->disconnectFromHost();
                    });
                });
            }
        });
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()); }
};
QByteArray json(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QJsonObject state(const QString& dev, const QString& value = "online_fresh") {
    return {{"devId",dev},{"state",value},{"voltage_v",52.8},{"current_a",-1.25},{"soc",80},{"temperature_c",28},{"data_age_ms",10}};
}
QJsonObject task(const QString& dev, const QString& status, qint64 id = 9007199254740993LL) {
    return {{"devId",dev},{"state",status},{"txnNo",id},{"reason",""},{"response",QJsonValue::Null}};
}
QJsonObject alarm(qint64 id) {
    return {{"id",id},{"devId","BT000"},{"desc","03"},{"state","open"},{"first_ms",1},{"clear_ms",QJsonValue::Null}};
}
class Operations : public QObject {
    Q_OBJECT
    QTemporaryDir settingsDirectory;
private slots:
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settingsDirectory.path());
#ifdef Q_OS_WIN
        const auto id=QFontDatabase::addApplicationFont(qEnvironmentVariable("SystemRoot")+"/Fonts/msyh.ttc");
        if (id>=0) QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(id).first(),10));
#endif
    }
    void integerPrecisionAndInvalidIds() {
        for (const qint64 id : {Q_INT64_C(9007199254740993), Q_INT64_C(9223372036854775807)})
            QCOMPARE(exactId(QJsonDocument::fromJson("{\"id\":" + QByteArray::number(id) + "}").object().value("id")), QString::number(id));
        for (const auto& bytes : {"0","-1","1.25","9223372036854775808","\"42\"","null"})
            QVERIFY(exactId(QJsonDocument::fromJson(QByteArray("{\"id\":")+bytes+"}").object().value("id")).isEmpty());
    }
    void statusFactsAndTransportFailure() {
        FakeHttp server; QString value="online_fresh";
        server.respond=[&](const Request&) { return Response{200,json(state("BT000",value))}; };
        Controller controller; QVERIFY(controller.select(server.base(),"BT000",""));
        for (const auto& selected : {"online_fresh","connected_no_data","connected_stale","offline"}) {
            value=selected; controller.refresh(); QTRY_VERIFY(!controller.statusBusy);
            QCOMPARE(controller.device.value("state").toString(),value);
        }
        server.respond=[](const Request&) { return Response{404,"{\"error\":{\"code\":\"device_not_found\"}}"}; };
        controller.refresh(); QTRY_VERIFY(!controller.statusBusy); QVERIFY(controller.statusText.contains("404"));
        server.respond=[](const Request&) { return Response{200,{},0,true}; };
        controller.refresh(); QTRY_VERIFY(!controller.statusBusy);
        QCOMPARE(controller.device.value("state").toString(),QString("offline"));
        QVERIFY(controller.statusText.contains(QStringLiteral("旧结果")));
    }
    void slowResponseDoesNotFreezeOrPileUp() {
        FakeHttp server; server.respond=[](const Request&) { return Response{200,json(state("BT000")),350}; };
        Controller controller; controller.select(server.base(),"BT000",""); controller.setTiming(1000,100,50,1000);
        int ticks=0; QTimer heartbeat; connect(&heartbeat,&QTimer::timeout,&controller,[&] { ++ticks; }); heartbeat.start(10);
        controller.refresh(); controller.setAutoRefresh(true);
        QTest::qWait(250); QCOMPARE(server.received.size(),1); QVERIFY(ticks>3);
        QTRY_VERIFY(!controller.device.isEmpty()); controller.setAutoRefresh(false);
    }
    void lateDeviceAndInvalidAddressCannotOverwrite() {
        FakeHttp server; server.respond=[](const Request& request) {
            const bool old=request.target.contains("OLD"); return Response{200,json(state(old?"OLD":"NEW")),old?300:0};
        };
        Controller controller; controller.select(server.base(),"OLD",""); controller.refresh();
        QTRY_COMPARE(server.received.size(),1);
        controller.select(server.base(),"NEW",""); controller.refresh(); QTRY_VERIFY(!controller.statusBusy);
        QTest::qWait(350); QCOMPARE(controller.device.value("devId").toString(),QString("NEW"));
        QVERIFY(!controller.select("http://example.com","NEW","")); QTest::qWait(50); QVERIFY(controller.device.isEmpty());
    }
    void unknownPostRecoversSameRequestAcrossDeviceSwitch() {
        FakeHttp server; QList<QString> keys;
        server.respond=[&](const Request& request) {
            if (request.method=="POST") {
                keys.append(QJsonDocument::fromJson(request.body).object().value("clientRequestId").toString());
                if (keys.size()==1) return Response{503,"{\"outcome\":\"unknown\"}"};
                return Response{200,json(task("BT000","pending"))};
            }
            return Response{200,json(task("BT000","success"))};
        };
        Controller controller; controller.select(server.base(),"BT000",""); controller.createQuery();
        QTRY_VERIFY(!controller.queryBusy); QVERIFY(controller.queryUncertain); const auto key=controller.requestId;
        controller.createQuery(); QCOMPARE(keys.size(),1);
        controller.select(server.base(),"OTHER",""); controller.select(server.base(),"BT000","");
        QCOMPARE(controller.requestId,key); controller.retryQuery(); QTRY_VERIFY(!controller.taskId.isEmpty());
        QCOMPARE(keys.size(),2); QCOMPARE(keys[0],keys[1]); QCOMPARE(controller.taskId,QString("9007199254740993"));
        QTRY_COMPARE(controller.task.value("state").toString(),QString("success"));
        QVERIFY(server.received.last().target.endsWith("9007199254740993"));
    }
    void lostPostAndExplicitRejection() {
        FakeHttp server; server.respond=[](const Request&) { return Response{200,{},0,true}; };
        Controller controller; controller.select(server.base(),"BT000",""); controller.createQuery();
        QTRY_VERIFY(!controller.queryBusy); QVERIFY(controller.queryUncertain);
        const auto key=controller.requestId;
        server.respond=[](const Request&) { return Response{409,"{\"error\":{\"code\":\"busy\"}}"}; };
        controller.retryQuery(); QTRY_VERIFY(!controller.queryBusy);
        QVERIFY(!controller.queryUncertain); QVERIFY(controller.queryText.contains(QStringLiteral("明确未创建")));
        QCOMPARE(controller.requestId,key);
    }
    void acceptedAndTerminalStates() {
        for (const auto& status : {"success","timeout","link_lost"}) {
            FakeHttp server; server.respond=[=](const Request&) { return Response{201,json(task("BT000",status))}; };
            Controller controller; controller.select(server.base(),"BT000",""); controller.createQuery();
            QTRY_VERIFY(!controller.queryBusy); QCOMPARE(controller.task.value("state").toString(),QString(status));
            QVERIFY(!controller.queryActive); QTest::qWait(600); QCOMPARE(server.received.size(),1);
        }
    }
    void observationTimeoutIsNotServerTimeout() {
        FakeHttp server; server.respond=[](const Request& request) { return Response{request.method=="POST"?201:200,json(task("BT000","pending"))}; };
        Controller controller; controller.setTiming(1000,2000,50,200); controller.select(server.base(),"BT000",""); controller.createQuery();
        QTRY_VERIFY(controller.queryText.contains(QStringLiteral("观察超时")));
        const auto count=server.received.size(); QTest::qWait(200); QCOMPARE(server.received.size(),count);
        QCOMPARE(controller.task.value("state").toString(),QString("pending"));
    }
    void alarmPagingPrecisionFilterAndReset() {
        FakeHttp server; server.respond=[](const Request& request) {
            if (request.target.contains("beforeId=")) return Response{200,json({{"items",QJsonArray{alarm(9007199254740992LL)}},{"nextBeforeId",QJsonValue::Null}})};
            return Response{200,json({{"items",QJsonArray{alarm(9007199254740993LL)}},{"nextBeforeId",Q_INT64_C(9007199254740993)}})};
        };
        Controller controller; controller.select(server.base(),"BT000","open"); controller.alarms(true);
        QTRY_VERIFY(!controller.alarmBusy); QCOMPARE(controller.nextCursor,QString("9007199254740993"));
        controller.alarms(false); QTRY_VERIFY(!controller.alarmBusy);
        QVERIFY(server.received.last().target.contains("beforeId=9007199254740993")); QCOMPARE(controller.alarmRows.size(),2); QVERIFY(!controller.more);
        controller.alarms(false); QCOMPARE(server.received.size(),2);
        controller.select(server.base(),"BT000","cleared"); QVERIFY(controller.alarmRows.isEmpty()); QVERIFY(controller.nextCursor.isEmpty());
        controller.alarms(true); QTRY_VERIFY(!controller.alarmBusy); QVERIFY(server.received.last().target.contains("status=cleared"));
    }
    void invalidJsonFieldsDeadlineAndClose() {
        FakeHttp server; server.respond=[](const Request&) { return Response{200,"bad json"}; };
        Controller controller; controller.select(server.base(),"BT000",""); controller.refresh(); QTRY_VERIFY(!controller.statusBusy); QVERIFY(controller.device.isEmpty());
        server.respond=[](const Request&) { return Response{200,"{}"}; }; controller.refresh(); QTRY_VERIFY(!controller.statusBusy); QVERIFY(controller.device.isEmpty());
        server.respond=[](const Request&) { return Response{200,json(state("BT000")),400}; };
        controller.setTiming(50,2000,50,500); controller.refresh(); QTRY_VERIFY(!controller.statusBusy); QVERIFY(controller.statusText.contains(QStringLiteral("超时")));
        { Controller closing; closing.select(server.base(),"BT000",""); closing.refresh(); }
        QTest::qWait(450);
    }
    void encodedDevicePath() {
        FakeHttp server; server.respond=[](const Request&) { return Response{404,"{}"}; };
        Controller controller; controller.select(server.base(),QString::fromUtf8("电池 /A?B"),""); controller.refresh();
        QTRY_VERIFY(!controller.statusBusy); QVERIFY(server.received[0].target.contains("%2F")); QVERIFY(server.received[0].target.contains("%3F"));
    }
    void delayedAlarmFilterCannotPolluteNewPage() {
        FakeHttp server; server.respond=[](const Request& request) {
            const bool old=request.target.contains("status=open");
            return Response{200,json({{"items",old?QJsonArray{alarm(1)}:QJsonArray{}},{"nextBeforeId",QJsonValue::Null}}),old?250:0};
        };
        Controller controller; controller.select(server.base(),"BT000","open"); controller.alarms(true);
        QTRY_COMPARE(server.received.size(),1);
        controller.select(server.base(),"BT000","cleared"); controller.alarms(true); QTRY_VERIFY(!controller.alarmBusy);
        QTest::qWait(300); QVERIFY(controller.alarmRows.isEmpty()); QVERIFY(!controller.more);
    }
    void realServiceThroughSsh() {
        const auto base=qEnvironmentVariable("BATT_QT_REAL_URL");
        if (base.isEmpty()) QSKIP("Run separately with the owned simulator and SSH fixture");
        Controller controller; QVERIFY(controller.select(base,"BT000","")); controller.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.statusBusy,5000);
        QCOMPARE(controller.device.value("state").toString(),QString("online_fresh"));
        controller.createQuery(); QTRY_COMPARE_WITH_TIMEOUT(controller.task.value("state").toString(),QString("success"),7000);
        QVERIFY(controller.task.value("response").isObject());
        const auto id=controller.taskId; const auto key=controller.requestId;
        controller.retryQuery(); QTRY_VERIFY(!controller.queryBusy); QCOMPARE(controller.taskId,id); QCOMPARE(controller.requestId,key);
        controller.alarms(true); QTRY_VERIFY(!controller.alarmBusy); QVERIFY(!controller.alarmRows.isEmpty());
        Window window; window.show();
        window.findChild<QLineEdit*>("baseAddress")->setText(base);
        window.findChild<QPushButton*>("refreshDevice")->click();
        QTRY_VERIFY(window.findChild<QLabel*>("deviceStatus")->text().contains("online_fresh"));
        QVERIFY(window.grab().save("batt-ops-live.png"));
    }
    void windowPreview() {
        Window window; window.show(); QTest::qWait(50);
        QVERIFY(window.grab().save("batt-ops-preview.png"));
    }
};
QTEST_MAIN(Operations)
#include "tests.moc"
