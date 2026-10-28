#include "window.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

class Admission : public QObject {
    Q_OBJECT
private slots:
    void integerPrecision() {
        for (const qint64 id : {Q_INT64_C(9007199254740993), Q_INT64_C(9223372036854775807)}) {
            const auto json = QByteArray("{\"txnNo\":") + QByteArray::number(id) + '}';
            const auto parsed = QJsonDocument::fromJson(json);
            QCOMPARE(parsed.object().value(QStringLiteral("txnNo")).toInteger(-1), id);
            QCOMPARE(QJsonDocument::fromJson(parsed.toJson()).object().value(QStringLiteral("txnNo")).toInteger(-1), id);
        }
    }
    void asynchronousRequest() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto* socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                if (socket->property("scheduled").toBool()) return;
                socket->setProperty("scheduled", true);
                QTimer::singleShot(200, socket, [socket] {
                    const QByteArray body("{\"txnNo\":9007199254740993}");
                    socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\nContent-Type: application/json\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            });
        });
        ProbeWindow window;
        window.address->setText(QStringLiteral("http://127.0.0.1:%1/probe").arg(server.serverPort()));
        QSignalSpy complete(&window, &ProbeWindow::completed);
        unsigned ticks = 0; QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, &window, [&] { ++ticks; }); heartbeat.start(10);
        window.request->click();
        QVERIFY(!window.request->isEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(complete.count(), 1, 5000);
        QVERIFY(complete.at(0).at(0).toBool());
        QVERIFY(ticks > 2);
        QVERIFY(window.result->text().contains(QStringLiteral("9007199254740993")));
        QVERIFY(window.request->isEnabled());
    }
    void deadlineAndClose() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        ProbeWindow window;
        window.setDeadline(100);
        window.address->setText(QStringLiteral("http://127.0.0.1:%1/probe").arg(server.serverPort()));
        QSignalSpy complete(&window, &ProbeWindow::completed);
        window.request->click();
        QTRY_COMPARE_WITH_TIMEOUT(complete.count(), 1, 3000);
        QVERIFY(!complete.at(0).at(0).toBool());
        QVERIFY(window.result->text().contains(QStringLiteral("deadline")));
        {
            ProbeWindow closing;
            closing.address->setText(window.address->text());
            closing.request->click();
        }
        QTest::qWait(50); // Process deferred reply destruction after window teardown.
    }
};
QTEST_MAIN(Admission)
#include "smoke_test.moc"
