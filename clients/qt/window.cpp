#include "window.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

Window::Window() {
    setWindowTitle(QStringLiteral("batt-access 电池运维 / V3.1")); resize(950, 700);
    QSettings settings("batt-access", "batt-ops");
    auto* layout = new QVBoxLayout(this);
    auto* connection = new QFormLayout;
    base = new QLineEdit(settings.value("base", "http://127.0.0.1:19002").toString());
    base->setObjectName("baseAddress");
    device = new QLineEdit(settings.value("device", "BT000").toString());
    connection->addRow(QStringLiteral("本地 HTTP 地址"), base); connection->addRow(QStringLiteral("设备 ID"), device);
    auto* refreshPeriod = new QSpinBox; refreshPeriod->setRange(100, 60000); refreshPeriod->setSuffix(" ms");
    refreshPeriod->setValue(settings.value("refresh_ms", 2000).toInt());
    auto* requestPeriod = new QSpinBox; requestPeriod->setRange(50, 30000); requestPeriod->setSuffix(" ms");
    requestPeriod->setValue(settings.value("request_ms", 3000).toInt());
    connection->addRow(QStringLiteral("自动刷新周期"), refreshPeriod); connection->addRow(QStringLiteral("HTTP 请求期限"), requestPeriod);
    layout->addLayout(connection);
    auto* tabs = new QTabWidget; layout->addWidget(tabs);
    auto* statusPage = new QWidget; auto* statusLayout = new QVBoxLayout(statusPage);
    status = new QLabel; status->setWordWrap(true); statusLayout->addWidget(status);
    status->setObjectName("deviceStatus");
    readings = new QTextEdit; readings->setReadOnly(true); statusLayout->addWidget(readings);
    auto* refresh = new QPushButton(QStringLiteral("立即读取设备")); auto* automatic = new QCheckBox(QStringLiteral("自动刷新"));
    refresh->setObjectName("refreshDevice");
    statusLayout->addWidget(refresh); statusLayout->addWidget(automatic); tabs->addTab(statusPage, QStringLiteral("设备状态"));
    auto* queryPage = new QWidget; auto* queryLayout = new QVBoxLayout(queryPage);
    query = new QLabel; query->setWordWrap(true); numbers = new QLabel; numbers->setWordWrap(true); numbers->setTextInteractionFlags(Qt::TextSelectableByMouse);
    queryLayout->addWidget(query); queryLayout->addWidget(numbers);
    create = new QPushButton(QStringLiteral("新建只读参数查询")); retry = new QPushButton(QStringLiteral("同号恢复 / 继续查看原任务"));
    queryLayout->addWidget(create); queryLayout->addWidget(retry);
    response = new QTextEdit; response->setReadOnly(true); queryLayout->addWidget(response); tabs->addTab(queryPage, QStringLiteral("主动查询"));
    auto* alarmPage = new QWidget; auto* alarmLayout = new QVBoxLayout(alarmPage);
    filter = new QComboBox; filter->addItem(QStringLiteral("全部告警"), ""); filter->addItem("open", "open"); filter->addItem("cleared", "cleared");
    alarmLayout->addWidget(filter); alarm = new QLabel; alarm->setWordWrap(true); alarmLayout->addWidget(alarm);
    table = new QTableWidget(0, 5); table->setHorizontalHeaderLabels({QStringLiteral("编号"), QStringLiteral("描述"), QStringLiteral("状态"), QStringLiteral("首次 UTC ms"), QStringLiteral("恢复 UTC ms")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); alarmLayout->addWidget(table);
    reset = new QPushButton(QStringLiteral("重置并读取第一页")); next = new QPushButton(QStringLiteral("下一页"));
    alarmLayout->addWidget(reset); alarmLayout->addWidget(next); tabs->addTab(alarmPage, QStringLiteral("告警记录"));
    connect(&controller, &Controller::changed, this, [this] { render(); });
    const auto select = [this] { apply(); };
    connect(base, &QLineEdit::editingFinished, this, select); connect(device, &QLineEdit::editingFinished, this, select);
    connect(filter, &QComboBox::currentIndexChanged, this, select);
    const auto timing = [this, refreshPeriod, requestPeriod] {
        controller.setTiming(requestPeriod->value(), refreshPeriod->value(), 500, 10000);
        QSettings settings("batt-access", "batt-ops"); settings.setValue("request_ms", requestPeriod->value()); settings.setValue("refresh_ms", refreshPeriod->value());
    };
    connect(refreshPeriod, &QSpinBox::valueChanged, this, timing); connect(requestPeriod, &QSpinBox::valueChanged, this, timing); timing();
    connect(refresh, &QPushButton::clicked, this, [this] { if (apply()) controller.refresh(); });
    connect(automatic, &QCheckBox::toggled, this, [this](bool enabled) { if (apply()) controller.setAutoRefresh(enabled); });
    connect(create, &QPushButton::clicked, this, [this] { if (apply()) controller.createQuery(); });
    connect(retry, &QPushButton::clicked, this, [this] { if (apply()) controller.retryQuery(); });
    connect(reset, &QPushButton::clicked, this, [this] { if (apply()) controller.alarms(true); });
    connect(next, &QPushButton::clicked, this, [this] { if (apply()) controller.alarms(false); });
    apply(); render();
}
bool Window::apply() {
    if (!controller.select(base->text(), device->text(), filter->currentData().toString())) return false;
    QSettings settings("batt-access", "batt-ops"); settings.setValue("base", base->text()); settings.setValue("device", device->text()); return true;
}
void Window::render() {
    status->setText(controller.statusText); query->setText(controller.queryText); alarm->setText(controller.alarmText);
    QStringList values;
    for (const auto& field : QList<QPair<QString,QString>>{{"voltage_v",QStringLiteral("电压 (V)")},{"current_a",QStringLiteral("电流 (A)")},
        {"soc",QStringLiteral("电量 (%)")},{"temperature_c",QStringLiteral("温度 (°C)")},{"data_age_ms",QStringLiteral("最旧属性年龄 (ms)")}}) {
        const auto value=controller.device.value(field.first);
        values.append(field.second+QStringLiteral("：")+(value.isUndefined()||value.isNull()?QStringLiteral("等待采集"):value.toVariant().toString()));
    }
    readings->setPlainText(values.join("\n\n"));
    response->setPlainText(QString::fromUtf8(QJsonDocument(controller.task).toJson(QJsonDocument::Indented)));
    numbers->setText(QStringLiteral("请求号：%1\n任务号：%2").arg(controller.requestId, controller.taskId));
    create->setEnabled(!controller.queryBusy && !controller.queryUncertain && !controller.queryActive);
    retry->setEnabled(!controller.queryBusy && !controller.requestId.isEmpty());
    reset->setEnabled(!controller.alarmBusy); next->setEnabled(!controller.alarmBusy && controller.more);
    table->setRowCount(controller.alarmRows.size());
    int rowNumber = 0;
    for (const auto& entry : controller.alarmRows) {
        const auto row = entry.toObject();
        const QStringList cells{exactId(row.value("id")), row.value("desc").toString(), row.value("state").toString(),
            QString::number(row.value("first_ms").toInteger()), row.value("clear_ms").isNull() ? QStringLiteral("—") : QString::number(row.value("clear_ms").toInteger())};
        for (int column = 0; column < cells.size(); ++column) table->setItem(rowNumber, column, new QTableWidgetItem(cells[column]));
        ++rowNumber;
    }
}
