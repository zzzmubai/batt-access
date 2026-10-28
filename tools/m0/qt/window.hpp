#pragma once
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

// M0 admission window, not the M5 operations client.
class ProbeWindow : public QWidget {
    Q_OBJECT
public:
    explicit ProbeWindow(QWidget* parent = nullptr);
    ~ProbeWindow() override;
    QLineEdit* address;
    QPushButton* request;
    QLabel* result;
    void setDeadline(int milliseconds) { deadline_ms = milliseconds; }
signals:
    void completed(bool ok);
private:
    QNetworkAccessManager manager;
    QPointer<QNetworkReply> pending;
    QTimer deadline;
    int deadline_ms = 3000;
    void start();
};
