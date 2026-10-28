#include "window.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QVBoxLayout>

ProbeWindow::ProbeWindow(QWidget* parent) : QWidget(parent) {
    setWindowTitle(QStringLiteral("batt-access V3 / M0 HTTP admission"));
    address = new QLineEdit(QStringLiteral("http://127.0.0.1:19302/probe"), this);
    request = new QPushButton(QStringLiteral("GET (async)"), this);
    result = new QLabel(QStringLiteral("Idle / Qt ") + QString::fromLatin1(qVersion()), this);
    result->setWordWrap(true);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(address); layout->addWidget(request); layout->addWidget(result);
    deadline.setSingleShot(true);
    connect(request, &QPushButton::clicked, this, &ProbeWindow::start);
    connect(&deadline, &QTimer::timeout, this, [this] {
        if (pending) { pending->setProperty("deadline_expired", true); pending->abort(); }
    });
}
ProbeWindow::~ProbeWindow() {
    deadline.stop();
    if (pending) { disconnect(pending, nullptr, this, nullptr); pending->abort(); }
}
void ProbeWindow::start() {
    if (pending) return;
    const QUrl url(address->text());
    // This probe follows the local-only deployment contract, including redirects.
    if (!url.isValid() || url.scheme() != QStringLiteral("http") || url.host() != QStringLiteral("127.0.0.1") ||
        !url.userInfo().isEmpty() || url.hasFragment()) {
        result->setText(QStringLiteral("Use an HTTP URL on 127.0.0.1 without credentials."));
        emit completed(false); return;
    }
    QNetworkRequest options(url);
    options.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    pending = manager.get(options);
    auto* reply = pending.data();
    request->setEnabled(false);
    result->setText(QStringLiteral("Waiting (GUI remains responsive)"));
    deadline.start(deadline_ms);
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > 65536) { reply->setProperty("too_large", true); reply->abort(); }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        deadline.stop();
        const auto data = reply->isReadable() ? reply->readAll() : QByteArray{};
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(data, &error);
        const auto id = document.object().value(QStringLiteral("txnNo")).toInteger(-1);
        bool ok = false;
        if (reply->property("deadline_expired").toBool()) result->setText(QStringLiteral("HTTP deadline exceeded; result unknown."));
        else if (reply->property("too_large").toBool() || data.size() > 65536) result->setText(QStringLiteral("Response too large."));
        else if (reply->error() != QNetworkReply::NoError) result->setText(QStringLiteral("HTTP transport failed; device state unknown."));
        else if (status != 200) result->setText(QStringLiteral("HTTP status %1").arg(status));
        else if (error.error != QJsonParseError::NoError || !document.isObject() || id <= 0)
            result->setText(QStringLiteral("Invalid JSON or missing integer txnNo."));
        else { result->setText(QStringLiteral("HTTP 200 / txnNo=") + QString::number(id)); ok = true; }
        pending = nullptr;
        reply->deleteLater();
        request->setEnabled(true);
        emit completed(ok);
    });
}
