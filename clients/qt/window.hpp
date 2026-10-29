#pragma once
#include "controller.hpp"
#include <QWidget>
class QLineEdit;
class QComboBox;
class QLabel;
class QTextEdit;
class QTableWidget;
class QPushButton;
class Window : public QWidget {
public:
    Window();
private:
    Controller controller;
    QLineEdit *base, *device;
    QComboBox* filter;
    QLabel *status, *query, *alarm, *numbers;
    QTextEdit *readings, *response;
    QTableWidget* table;
    QPushButton *create, *retry, *next, *reset;
    bool apply();
    void render();
};
